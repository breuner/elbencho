// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>

#include "Logger.h"
#include "modes/netbench/NetBenchEngineRdmaCm.h"
#include "modes/netbench/NetBenchProtocol.h"
#include "ProgException.h"
#include "workers/WorkerException.h"

#ifdef RDMACM_SUPPORT

#define RDMACM_RETRY_CNT            7
#define RDMACM_RNR_RETRY            7 // infinite
#define RDMACM_RD_ATOMIC            1 // responder resources / initiator depth (unused by sends)

namespace
{
    void setPort(sockaddr_storage& addr, unsigned short port)
    {
        if(addr.ss_family == AF_INET)
            ( (sockaddr_in*)&addr)->sin_port = htons(port);
        else
            ( (sockaddr_in6*)&addr)->sin6_port = htons(port);
    }

    std::string hexStr(uint32_t value)
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "0x%x", value);
        return buf;
    }
} // anonymous namespace


/**
 * Bind to the interface address of the selector. A server starts listening, a client creates
 * its queue pair right away (binding to an address already selects the device).
 *
 * @throw WorkerException on error.
 */
RdmaCmEndpoint::RdmaCmEndpoint(bool isServer, const NetBenchSelector& selector,
    unsigned short fixedPort, size_t slabSize, size_t numConns, size_t recvsPerConn,
    size_t sendsPerConn) :
    IbverbsEndpointBase(slabSize, recvsPerConn, sendsPerConn, selector.rnrTimer),
    isServer(isServer)
{
    try
    {
        int family = selector.family;
        if( (family == AF_UNSPEC) && selector.peerAddr.isSockAddr() )
            family = selector.peerAddr.getFamily();

        sockaddr_storage bindAddr;
        if(!NetBenchIP::parseIP(selector.netdev, bindAddr) && (selector.netdev.empty() ||
            !NetBenchIP::resolveIfaceAddr(selector.netdev, family, bindAddr) ) )
            throw WorkerException("The rdmacm engine needs an interface address to bind to" +
                (selector.netdev.empty() ? std::string() : ", but \"" + selector.netdev +
                "\" is none") + ". Give an IP address, subnet or interface name via "
                "\"--" "netdevs" "\".");

        setPort(bindAddr, isServer ? fixedPort : 0);

        cmChannel = rdma_create_event_channel();
        if(!cmChannel)
            throw WorkerException(ibvErrStr("rdma_create_event_channel", errno) );

        fcntl(cmChannel->fd, F_SETFL, fcntl(cmChannel->fd, F_GETFL, 0) | O_NONBLOCK);

        struct rdma_cm_id* id;
        if(rdma_create_id(cmChannel, &id, NULL, RDMA_PS_TCP) )
            throw WorkerException(ibvErrStr("rdma_create_id", errno) );

        if(isServer)
            listenId = id;
        else
        { // (the client's single connection owns its id from the start)
            auto conn = std::make_unique<CmConn>();
            conn->id = id;
            id->context = conn.get();
            conns.push_back(std::move(conn) );
        }

        const std::string bindAddrStr = NetBenchAddr::fromSockAddr( (const sockaddr*)&bindAddr,
            NetBenchIP::sockAddrLen( (const sockaddr*)&bindAddr) ).ipToStr();

        if(rdma_bind_addr(id, (struct sockaddr*)&bindAddr) )
            throw WorkerException(ibvErrStr("rdma_bind_addr", errno) + " for address " +
                bindAddrStr + ". Select an interface with an RDMA device via \"--" "netdevs" "\".");

        if(!id->verbs)
            throw WorkerException("No RDMA device for address " + bindAddrStr + ". Select an "
                "interface with an RDMA device via \"--" "netdevs" "\".");

        localAddr = NetBenchAddr::fromSockAddr(rdma_get_local_addr(id),
            NetBenchIP::sockAddrLen(rdma_get_local_addr(id) ) );
        deviceName = id->verbs->device->name;

        openDataPath(id->verbs, id->pd, numConns);

        if(isServer)
        {
            if(rdma_listen(listenId, std::max<int>(numConns, 1) ) )
                throw WorkerException(ibvErrStr("rdma_listen", errno) );
        }
        else
            createQp(static_cast<CmConn*>(conns.front().get() ) );

        LOGGER(Log_DEBUG, "Opened rdmacm endpoint: " << getDescription() << "; "
            "channel fds: " << channel->fd << ", " << cmChannel->fd << std::endl);
    }
    catch(...)
    {
        closeAll();
        throw;
    }
}

RdmaCmEndpoint::~RdmaCmEndpoint()
{
    closeAll();
}

/**
 * Destroy the queue pairs and connection identifiers, the data path, the listener and the event
 * channel. The device contexts belong to librdmacm.
 */
void RdmaCmEndpoint::closeAll()
{
    for(const auto& conn : conns)
    {
        CmConn* cmConn = static_cast<CmConn*>(conn.get() );

        if(cmConn->qp)
            rdma_destroy_qp(cmConn->id);

        if(cmConn->id)
            rdma_destroy_id(cmConn->id);

        cmConn->qp = NULL;
        cmConn->id = NULL;
    }

    closeDataPath();

    if(listenId)
        rdma_destroy_id(listenId);

    if(cmChannel)
        rdma_destroy_event_channel(cmChannel);

    listenId = NULL;
    cmChannel = NULL;
}

std::string RdmaCmEndpoint::getDescription() const
{
    return "rdmacm rc: " + localAddr.toStr() + " (" + deviceName + ")";
}

/**
 * Create the queue pair of a connection on our completion queue.
 */
void RdmaCmEndpoint::createQp(CmConn* conn)
{
    struct ibv_qp_init_attr initAttr = {};
    initAttr.send_cq = cq;
    initAttr.recv_cq = cq;
    initAttr.cap.max_send_wr = sendsPerConn;
    initAttr.cap.max_recv_wr = recvsPerConn;
    initAttr.cap.max_send_sge = 1;
    initAttr.cap.max_recv_sge = 1;
    initAttr.qp_type = IBV_QPT_RC;
    initAttr.sq_sig_all = 1;

    if(rdma_create_qp(conn->id, pd, &initAttr) )
        throw WorkerException(ibvErrStr("rdma_create_qp", errno) );

    conn->qp = conn->id->qp;
}

/**
 * Set the receiver-not-ready retry timer ("--netrnr") of a connected queue pair: a server reposts
 * a consumed receive only after it saw the completion, so a peer that already sent the next
 * block gets a receiver-not-ready, which rdma_cm would retry after 655 ms by default.
 */
void RdmaCmEndpoint::setRnrTimer(struct ibv_qp* qp)
{
    struct ibv_qp_attr attr = {};
    attr.min_rnr_timer = rnrTimer;

    const int ret = ibv_modify_qp(qp, &attr, IBV_QP_MIN_RNR_TIMER);
    if(ret)
        LOGGER(Log_DEBUG, ibvErrStr("ibv_modify_qp (min_rnr_timer)", ret) << std::endl);

    struct ibv_qp_init_attr initAttr;
    if(!ibv_query_qp(qp, &attr, IBV_QP_MIN_RNR_TIMER, &initAttr) )
        LOGGER(Log_DEBUG, "Queue pair " << qp->qp_num << " min_rnr_timer: " <<
            (unsigned)attr.min_rnr_timer << std::endl);
}

void RdmaCmEndpoint::putLocalAddr(bpt::ptree& outTree) const
{
    outTree.put(NETBENCH_JSON_ADDRFORMAT, localAddr.format);
    outTree.put(NETBENCH_JSON_ADDR, localAddr.toHex() );
}

/**
 * Client: our connection with its rank; the request just tells the server our address.
 */
NetBenchConn* RdmaCmEndpoint::prepareConnect(uint32_t rank, bpt::ptree& request)
{
    CmConn* conn = static_cast<CmConn*>(conns.front().get() );
    conn->rank = rank;

    putLocalAddr(request);

    return conn;
}

/**
 * Client: resolve the server's listen address from the reply and connect to it, with our rank as
 * private data of the connect request. Blocks (event-based) for the connection steps.
 */
void RdmaCmEndpoint::completeConnect(NetBenchConn* conn, const bpt::ptree& reply)
{
    CmConn* cmConn = static_cast<CmConn*>(conn);

    cmConn->peerAddr = NetBenchAddr::fromHex(reply.get<uint32_t>(NETBENCH_JSON_ADDRFORMAT),
        reply.get<std::string>(NETBENCH_JSON_ADDR) );

    if(!cmConn->peerAddr.isSockAddr() || (cmConn->peerAddr.getFamily() != localAddr.getFamily() ) )
        throw WorkerException("Address family mismatch. Server: " + cmConn->peerAddr.ipToStr() +
            "; own: " + localAddr.ipToStr() + ". Select interfaces of the same family via "
            "\"--" "netdevs" "\".");

    if(rdma_resolve_addr(cmConn->id, NULL, (struct sockaddr*)cmConn->peerAddr.getSockAddr(),
        NETBENCH_CM_STEP_TIMEOUT_MS) )
        throw WorkerException(ibvErrStr("rdma_resolve_addr", errno) );

    waitForEvent(cmConn, RDMA_CM_EVENT_ADDR_RESOLVED, "address resolution");

    if(rdma_resolve_route(cmConn->id, NETBENCH_CM_STEP_TIMEOUT_MS) )
        throw WorkerException(ibvErrStr("rdma_resolve_route", errno) );

    waitForEvent(cmConn, RDMA_CM_EVENT_ROUTE_RESOLVED, "route resolution");

    const uint32_t privateData[2] = { htonl(NETBENCH_CM_MAGIC), htonl(cmConn->rank) };

    struct rdma_conn_param param = {};
    param.private_data = privateData;
    param.private_data_len = sizeof(privateData);
    param.responder_resources = RDMACM_RD_ATOMIC;
    param.initiator_depth = RDMACM_RD_ATOMIC;
    param.retry_count = RDMACM_RETRY_CNT;
    param.rnr_retry_count = RDMACM_RNR_RETRY;

    if(rdma_connect(cmConn->id, &param) )
        throw WorkerException(ibvErrStr("rdma_connect", errno) );

    waitForEvent(cmConn, RDMA_CM_EVENT_ESTABLISHED, "connection");

    setRnrTimer(cmConn->qp);

    struct ibv_ece ece = {};
    const int eceRes = ibv_query_ece(cmConn->qp, &ece);

    LOGGER(Log_VERBOSE, "RDMA CM connection to " << cmConn->peerAddr.toStr() << ": " <<
        ( (eceRes || !ece.vendor_id) ? std::string("no ECE") :
        "ECE vendor " + hexStr(ece.vendor_id) + " options " + hexStr(ece.options) ) <<
        "; WorkerRank: " << cmConn->rank << std::endl);
}

/**
 * Client: wait for the next event on our channel (which only carries our connection's events).
 *
 * @throw WorkerException on timeout or on any other event than the expected one.
 */
void RdmaCmEndpoint::waitForEvent(CmConn* conn, enum rdma_cm_event_type expected,
    const char* step)
{
    for( ; ; )
    {
        struct pollfd pfd = { cmChannel->fd, POLLIN, 0 };

        if(poll(&pfd, 1, NETBENCH_CM_STEP_TIMEOUT_MS) <= 0)
            throw WorkerException(std::string("Timed out waiting for RDMA CM ") + step + " (" +
                std::to_string(NETBENCH_CM_STEP_TIMEOUT_MS) + " ms); server: " +
                conn->peerAddr.toStr() );

        struct rdma_cm_event* event;
        if(rdma_get_cm_event(cmChannel, &event) )
        {
            if(errno == EAGAIN)
                continue;

            throw WorkerException(ibvErrStr("rdma_get_cm_event", errno) );
        }

        const enum rdma_cm_event_type type = event->event;
        const int status = event->status;

        rdma_ack_cm_event(event);

        if(type == expected)
            return;

        throw WorkerException(std::string("RDMA CM ") + step + " failed: " +
            rdma_event_str(type) + " (status " + std::to_string(status) + "); server: " +
            conn->peerAddr.toStr() );
    }
}

/**
 * Server: register the client's stream; the reply carries our listen address. The queue pair
 * comes with the client's connect request (see progress).
 */
NetBenchConn* RdmaCmEndpoint::acceptPeer(const bpt::ptree& request, bpt::ptree& reply)
{
    const uint32_t clientRank = request.get<uint32_t>(NETBENCH_JSON_CLIENTRANK);

    if(NetBenchConn* existing = findConn(clientRank) )
    { // (a repeated connect of the same client just gets the same reply)
        putLocalAddr(reply);
        return existing;
    }

    const NetBenchAddr peerAddr = NetBenchAddr::fromHex(
        request.get<uint32_t>(NETBENCH_JSON_ADDRFORMAT),
        request.get<std::string>(NETBENCH_JSON_ADDR) );

    if(peerAddr.isSockAddr() && (peerAddr.getFamily() != localAddr.getFamily() ) )
        throw ProgException("Address family mismatch. Client: " + peerAddr.ipToStr() + "; "
            "Server: " + localAddr.ipToStr() + ". Select interfaces of the same family via "
            "\"--" "netdevs" "\".");

    auto conn = std::make_unique<CmConn>();
    conn->rank = clientRank;
    conn->peerAddr = peerAddr;

    conns.push_back(std::move(conn) );

    putLocalAddr(reply);

    return conns.back().get();
}

/**
 * Receives for a connection whose queue pair does not exist yet are queued and posted when the
 * client's connect request arrives.
 */
void RdmaCmEndpoint::postRecv(NetBenchConn* conn, void* buf, size_t len, NetBenchOpCtx* ctx)
{
    CmConn* cmConn = static_cast<CmConn*>(conn);

    if(cmConn->qp)
    {
        IbverbsEndpointBase::postRecv(conn, buf, len, ctx);
        return;
    }

    cmConn->queuedRecvs.push_back( { buf, len, ctx } );
    ctx->conn = conn;
    ctx->posted = true;
}

/**
 * Handle the connection events: a server creates the queue pair for a requesting client and
 * accepts, errors are remembered for readCompletions(). Non-blocking.
 */
void RdmaCmEndpoint::progress()
{
    for( ; ; )
    {
        struct rdma_cm_event* event;

        if(rdma_get_cm_event(cmChannel, &event) )
            return; // (EAGAIN: no more events)

        handleEvent(event);
    }
}

void RdmaCmEndpoint::handleEvent(struct rdma_cm_event* event)
{
    const enum rdma_cm_event_type type = event->event;
    const int status = event->status;
    struct rdma_cm_id* id = event->id;
    CmConn* conn = static_cast<CmConn*>(id->context);

    uint32_t privateData[2] = {};
    if(event->param.conn.private_data_len >= sizeof(privateData) )
        memcpy(privateData, event->param.conn.private_data, sizeof(privateData) );

    rdma_ack_cm_event(event); // (before anything that may destroy the id)

    switch(type)
    {
        case RDMA_CM_EVENT_CONNECT_REQUEST:
        {
            conn = (ntohl(privateData[0] ) == NETBENCH_CM_MAGIC) ?
                static_cast<CmConn*>(findConn(ntohl(privateData[1] ) ) ) : NULL;

            if(!conn || conn->id)
            {
                LOGGER(Log_DEBUG, "Rejecting RDMA CM connect request without a registered "
                    "stream. Endpoint: " << getDescription() << std::endl);

                rdma_reject(id, NULL, 0);
                rdma_destroy_id(id);
                return;
            }

            conn->id = id;
            id->context = conn;

            try
            {
                createQp(conn);

                for(const QueuedRecv& recv : conn->queuedRecvs)
                    IbverbsEndpointBase::postRecv(conn, recv.buf, recv.len, recv.ctx);

                conn->queuedRecvs.clear();

                struct rdma_conn_param param = {};
                param.responder_resources = RDMACM_RD_ATOMIC;
                param.initiator_depth = RDMACM_RD_ATOMIC;
                param.rnr_retry_count = RDMACM_RNR_RETRY;

                if(rdma_accept(id, &param) )
                    throw WorkerException(ibvErrStr("rdma_accept", errno) );

                setRnrTimer(conn->qp); // (rdma_accept moved the queue pair to RTS)
            }
            catch(WorkerException& e)
            {
                failConn(conn, e.what() );
            }
        } break;

        case RDMA_CM_EVENT_ESTABLISHED:
        case RDMA_CM_EVENT_DISCONNECTED:
        {
            LOGGER(Log_DEBUG, "RDMA CM event: " << rdma_event_str(type) << "; stream: " <<
                (conn ? std::to_string(conn->rank) : "-") << std::endl);
        } break;

        default:
        {
            if(conn)
                failConn(conn, std::string("RDMA CM ") + rdma_event_str(type) + " (status " +
                    std::to_string(status) + "); peer: " + conn->peerAddr.toStr() );
        } break;
    }
}

/**
 * Remember a connection error for the next readCompletions().
 */
void RdmaCmEndpoint::failConn(CmConn* conn, const std::string& errorStr)
{
    conn->errorStr = "Stream " + std::to_string(conn->rank) + ": " + errorStr;

    LOGGER(Log_DEBUG, "Netbench rdmacm " << conn->errorStr << std::endl);
}

/**
 * Connection events first (they share the wait set), then the completions.
 */
size_t RdmaCmEndpoint::readCompletions(NetBenchCompletion* entries, size_t maxEntries)
{
    progress();

    for(const auto& conn : conns)
    {
        CmConn* cmConn = static_cast<CmConn*>(conn.get() );

        if(!cmConn->errorStr.empty() )
        {
            const std::string errorStr = cmConn->errorStr;
            cmConn->errorStr.clear();
            throw WorkerException(errorStr);
        }
    }

    return IbverbsEndpointBase::readCompletions(entries, maxEntries);
}

bool RdmaCmEndpoint::addWaitFDs(std::vector<struct pollfd>& outFDs)
{
    IbverbsEndpointBase::addWaitFDs(outFDs);
    outFDs.push_back( { cmChannel->fd, POLLIN, 0 } );

    return true;
}

#endif // RDMACM_SUPPORT
