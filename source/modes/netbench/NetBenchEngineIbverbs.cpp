// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <map>
#include <mutex>
#include <net/if.h>
#include <netinet/in.h>
#include <random>

#include "Logger.h"
#include "modes/netbench/NetBenchEngineIbverbs.h"
#include "modes/netbench/NetBenchProtocol.h"
#include "ProgException.h"
#include "workers/WorkerException.h"

#ifdef IBVERBS_SUPPORT

#define IBVERBS_QP_TIMEOUT          14 // ~67 ms ack timeout (4.096 us x 2^14)
#define IBVERBS_RETRY_CNT           7
#define IBVERBS_RNR_RETRY           7 // infinite
#define IBVERBS_HOP_LIMIT           64 // for routed RoCE

namespace
{
    bool isIPv4MappedGid(const union ibv_gid& gid)
    {
        static const uint8_t prefix[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };

        return !memcmp(gid.raw, prefix, sizeof(prefix) );
    }

    /**
     * The IP address that a RoCE GID encodes (IPv4-mapped or plain IPv6).
     */
    NetBenchAddr gidToAddr(const union ibv_gid& gid)
    {
        if(isIPv4MappedGid(gid) )
        {
            sockaddr_in addr4 = {};
            addr4.sin_family = AF_INET;
            memcpy(&addr4.sin_addr, gid.raw + 12, sizeof(addr4.sin_addr) );
            return NetBenchAddr::fromSockAddr( (const sockaddr*)&addr4, sizeof(addr4) );
        }

        sockaddr_in6 addr6 = {};
        addr6.sin6_family = AF_INET6;
        memcpy(&addr6.sin6_addr, gid.raw, sizeof(addr6.sin6_addr) );
        return NetBenchAddr::fromSockAddr( (const sockaddr*)&addr6, sizeof(addr6) );
    }

    /**
     * @return true if the RoCE GID encodes the given IP address.
     */
    bool gidMatchesAddr(const union ibv_gid& gid, const struct sockaddr* addr)
    {
        if(addr->sa_family == AF_INET)
            return isIPv4MappedGid(gid) &&
                !memcmp(gid.raw + 12, &( (const sockaddr_in*)addr)->sin_addr, 4);

        if(addr->sa_family == AF_INET6)
            return !memcmp(gid.raw, &( (const sockaddr_in6*)addr)->sin6_addr, 16);

        return false;
    }

    bool isLinkLocalGid(const union ibv_gid& gid)
    {
        return (gid.raw[0] == 0xfe) && ( (gid.raw[1] & 0xc0) == 0x80);
    }
} // anonymous namespace


/**
 * Open the endpoint on the RDMA device and port behind the selector's interface.
 *
 * @throw WorkerException on error.
 */
IbverbsEndpoint::IbverbsEndpoint(const NetBenchSelector& selector, size_t slabSize,
    size_t numConns, size_t recvsPerConn, size_t sendsPerConn) :
    IbverbsEndpointBase(slabSize, recvsPerConn, sendsPerConn, selector.rnrTimer)
{
    try
    {
        selectPort(selector);

        if(selector.maxMsgSize &&
            ( (selector.maxMsgSize > portAttr.max_msg_sz) || (selector.maxMsgSize > UINT32_MAX) ) )
            throw WorkerException("Message size " + std::to_string(selector.maxMsgSize) +
                " exceeds the maximum of " + std::to_string(portAttr.max_msg_sz) + " bytes of "
                "RDMA device " + device->context->device->name + ".");

        openDataPath(device->context, device->pd, numConns);

        LOGGER(Log_DEBUG, "Opened ibverbs endpoint: " << getDescription() << "; "
            "mtu: " << (128 << portAttr.active_mtu) << "; channel fd: " << channel->fd <<
            std::endl);
    }
    catch(...)
    {
        closeAll();
        throw;
    }
}

IbverbsEndpoint::~IbverbsEndpoint()
{
    closeAll();
}

/**
 * Destroy the queue pairs and the data path. The device stays open for the process.
 */
void IbverbsEndpoint::closeAll()
{
    for(const auto& conn : conns)
    {
        IbvConn* ibvConn = static_cast<IbvConn*>(conn.get() );

        if(ibvConn->qp)
            ibv_destroy_qp(ibvConn->qp);

        ibvConn->qp = NULL;
    }

    closeDataPath();
}

/**
 * Open a device context and protection domain, or return the ones opened earlier. A device that
 * was removed and added again in the meantime (e.g. by a firmware reset) comes back under the
 * same name, but the old context is dead, so it gets reopened then.
 */
IbverbsEndpoint::Device* IbverbsEndpoint::openDevice(struct ibv_device* ibvDevice)
{
    static std::mutex devicesMutex;
    static std::map<std::string, Device> devices;

    std::lock_guard<std::mutex> lock(devicesMutex);

    const std::string name = ibvDevice->name;
    auto iter = devices.find(name);

    if(iter != devices.end() )
    {
        struct ibv_device_attr attr;

        if(!ibv_query_device(iter->second.context, &attr) )
            return &iter->second;

        /* (the old handles are not closed: endpoints of an interrupted run may still refer to
            them, and a device reset is rare, so the two leaked file descriptors are acceptable) */
        LOGGER(Log_DEBUG, "RDMA device was reset, reopening: " << name << std::endl);
    }

    Device device = {};

    device.context = ibv_open_device(ibvDevice);
    if(!device.context)
        throw WorkerException(ibvErrStr( ("ibv_open_device (" + name + ")").c_str(), errno) );

    if(ibv_query_device(device.context, &device.attr) )
    {
        ibv_close_device(device.context);
        throw WorkerException(ibvErrStr("ibv_query_device", errno) );
    }

    device.pd = ibv_alloc_pd(device.context);
    if(!device.pd)
    {
        ibv_close_device(device.context);
        throw WorkerException(ibvErrStr("ibv_alloc_pd", errno) );
    }

    devices[name] = device; // (replacing in place keeps the pointers of existing users valid)

    return &devices[name];
}

/**
 * Find the device, port and GID for the selector: an IP address (given, resolved from an
 * interface name, or the one that routes to the server) has exactly one RoCE GID (v2 preferred)
 * or belongs to an IPoIB interface of an InfiniBand port; an RDMA device name takes that device's
 * first active port. Without any of that, or if the address that routes to the server has no
 * RDMA device, the first active port with a usable GID is taken.
 */
void IbverbsEndpoint::selectPort(const NetBenchSelector& selector)
{
    int family = selector.family;

    if( (family == AF_UNSPEC) && selector.peerAddr.isSockAddr() )
        family = selector.peerAddr.getFamily();

    sockaddr_storage ipAddr;
    bool haveIP = false;
    bool ipIsHint = false; // the routed address is just a preference
    std::string deviceName;

    if(NetBenchIP::parseIP(selector.netdev, ipAddr) )
        haveIP = true;
    else
    if(!selector.netdev.empty() )
    {
        if(NetBenchIP::resolveIfaceAddr(selector.netdev, family, ipAddr) )
            haveIP = true;
        else
            deviceName = selector.netdev; // (checked against the device names below)
    }
    else
    if(selector.peerAddr.isSockAddr() )
        haveIP = ipIsHint = NetBenchIP::routeSourceAddr(selector.peerAddr.getSockAddr(),
            NetBenchIP::sockAddrLen(selector.peerAddr.getSockAddr() ), ipAddr);

    int numDevices = 0;
    struct ibv_device** deviceList = ibv_get_device_list(&numDevices);
    bool deviceNameFound = false;

    for(int pass = 0; (pass < 2) && !device; pass++)
    {
        if(pass)
        { // second pass without the routed address: any RDMA port of the family
            if(!ipIsHint)
                break;

            haveIP = false;
        }

        for(int i = 0; deviceList && (i < numDevices) && !device; i++)
        {
            if(!deviceName.empty() )
            {
                if(deviceName != deviceList[i]->name)
                    continue;

                deviceNameFound = true;
            }

            Device* candidate = openDevice(deviceList[i] );

            for(uint8_t port = 1; (port <= candidate->attr.phys_port_cnt) && !device; port++)
            {
                if(ibv_query_port(candidate->context, port, &portAttr) ||
                    (portAttr.state != IBV_PORT_ACTIVE) )
                    continue;

                device = candidate;
                portNum = port;

                if(!selectGid(haveIP ? (const sockaddr*)&ipAddr : NULL, family) )
                    device = NULL; // (port has no matching GID, try the next one)
            }
        }
    }

    if(deviceList)
        ibv_free_device_list(deviceList);

    if(device)
        return;

    if(!deviceName.empty() && !deviceNameFound)
        throw WorkerException("\"" + selector.netdev + "\" is neither a network interface nor "
            "an RDMA device of this host.");

    throw WorkerException("No active RDMA port found for " +
        (haveIP ? "address " + NetBenchAddr::fromSockAddr( (const sockaddr*)&ipAddr,
            NetBenchIP::sockAddrLen( (const sockaddr*)&ipAddr) ).ipToStr() :
        !deviceName.empty() ? "device " + deviceName : std::string("this host") ) +
        ". Select an interface with an RDMA device via \"--" "netdevs" "\".");
}

/**
 * Pick the GID of the current device and port for the given address (or any usable GID of the
 * given family if no address is given). Sets gidIndex, gid, gidType and localAddr.
 *
 * @return false if the port has no matching GID.
 */
bool IbverbsEndpoint::selectGid(const struct sockaddr* addr, int family)
{
    const bool isEthernet = (portAttr.link_layer == IBV_LINK_LAYER_ETHERNET);
    unsigned ifIndex = 0;

    if(!isEthernet && addr)
    { // InfiniBand: the address belongs to an IPoIB interface, which the GID entries name
        const std::string ifName = NetBenchIP::ifaceNameForAddr(addr);
        ifIndex = if_nametoindex(ifName.c_str() );

        if(!ifIndex)
            return false;
    }

    gidIndex = -1;

    for(int idx = 0; idx < portAttr.gid_tbl_len; idx++)
    {
        struct ibv_gid_entry entry;

        if(ibv_query_gid_ex(device->context, portNum, idx, &entry, 0) )
            continue;

        if(isEthernet)
        {
            if(addr ? !gidMatchesAddr(entry.gid, addr) :
                ( (entry.gid_type == IBV_GID_TYPE_IB) || isLinkLocalGid(entry.gid) ||
                ( (family == AF_INET) && !isIPv4MappedGid(entry.gid) ) ||
                ( (family == AF_INET6) && isIPv4MappedGid(entry.gid) ) ) )
                continue;

            // RoCE v2 is preferred over v1, so keep looking after a v1 match
            if( (gidIndex >= 0) && (gidType == IBV_GID_TYPE_ROCE_V2) )
                continue;
        }
        else
        if( (entry.gid_type != IBV_GID_TYPE_IB) || (addr && (entry.ndev_ifindex != ifIndex) ) ||
            (gidIndex >= 0) )
            continue;

        gidIndex = idx;
        gid = entry.gid;
        gidType = entry.gid_type;

        if(isEthernet)
            localAddr = gidToAddr(entry.gid);
        else
        { // the IPoIB interface's address is what the peers see of us
            char ifName[IF_NAMESIZE] = "";
            sockaddr_storage ifAddr;

            if(addr)
                localAddr = NetBenchAddr::fromSockAddr(addr, NetBenchIP::sockAddrLen(addr) );
            else
            if(if_indextoname(entry.ndev_ifindex, ifName) &&
                NetBenchIP::resolveIfaceAddr(ifName, family, ifAddr) )
                localAddr = NetBenchAddr::fromSockAddr( (const sockaddr*)&ifAddr,
                    NetBenchIP::sockAddrLen( (const sockaddr*)&ifAddr) );
        }
    }

    return gidIndex >= 0;
}

/**
 * @return "ibverbs rc: IP (DEVICE port N gid I)" for log messages.
 */
std::string IbverbsEndpoint::getDescription() const
{
    return "ibverbs rc: " + (localAddr.isSockAddr() ? localAddr.ipToStr() : std::string("-") ) +
        " (" + device->context->device->name + " port " + std::to_string(portNum) + " gid " +
        std::to_string(gidIndex) + ")";
}

/**
 * Create the queue pair of a new connection and move it to the INIT state.
 */
IbverbsEndpoint::IbvConn* IbverbsEndpoint::createConn(uint32_t rank)
{
    if( (recvsPerConn > (size_t)device->attr.max_qp_wr) ||
        (sendsPerConn > (size_t)device->attr.max_qp_wr) )
        throw WorkerException("The IO depth exceeds the queue pair limit of RDMA device " +
            std::string(device->context->device->name) + ": " +
            std::to_string(device->attr.max_qp_wr) );

    auto conn = std::make_unique<IbvConn>();
    conn->rank = rank;

    struct ibv_qp_init_attr initAttr = {};
    initAttr.send_cq = cq;
    initAttr.recv_cq = cq;
    initAttr.cap.max_send_wr = sendsPerConn;
    initAttr.cap.max_recv_wr = recvsPerConn;
    initAttr.cap.max_send_sge = 1;
    initAttr.cap.max_recv_sge = 1;
    initAttr.qp_type = IBV_QPT_RC;
    initAttr.sq_sig_all = 1;

    conn->qp = ibv_create_qp(device->pd, &initAttr);
    if(!conn->qp)
        throw WorkerException(ibvErrStr("ibv_create_qp", errno) );

    static thread_local std::mt19937 randGen{std::random_device{}()};
    conn->psn = randGen() & 0xffffff;

    struct ibv_qp_attr attr = {};
    attr.qp_state = IBV_QPS_INIT;
    attr.pkey_index = 0;
    attr.port_num = portNum;
    attr.qp_access_flags = 0;

    int ret = ibv_modify_qp(conn->qp, &attr,
        IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
    if(ret)
    {
        ibv_destroy_qp(conn->qp);
        throw WorkerException(ibvErrStr("ibv_modify_qp (INIT)", ret) );
    }

    conns.push_back(std::move(conn) );

    return static_cast<IbvConn*>(conns.back().get() );
}

/**
 * Connect the queue pair to its peer: INIT -> RTR -> RTS.
 */
void IbverbsEndpoint::connectQp(IbvConn* conn, const PeerInfo& peer)
{
    struct ibv_qp_attr attr = {};
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = (enum ibv_mtu)std::min<int>(portAttr.active_mtu, peer.mtu);
    attr.dest_qp_num = peer.qpn;
    attr.rq_psn = peer.psn;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = rnrTimer;
    attr.ah_attr.port_num = portNum;
    attr.ah_attr.dlid = peer.lid;

    if( (gidType == IBV_GID_TYPE_IB) && (peer.gidType == IBV_GID_TYPE_IB) )
        attr.ah_attr.is_global = 0; // InfiniBand: routed by LID within the subnet
    else
    {
        attr.ah_attr.is_global = 1;
        attr.ah_attr.grh.dgid = peer.gid;
        attr.ah_attr.grh.sgid_index = gidIndex;
        attr.ah_attr.grh.hop_limit = IBVERBS_HOP_LIMIT;
    }

    int ret = ibv_modify_qp(conn->qp, &attr, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
        IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);
    if(ret)
        throw WorkerException(ibvErrStr("ibv_modify_qp (RTR)", ret) + "; peer: " +
            peer.addr.ipToStr() + " qpn " + std::to_string(peer.qpn) );

    memset(&attr, 0, sizeof(attr) );
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = IBVERBS_QP_TIMEOUT;
    attr.retry_cnt = IBVERBS_RETRY_CNT;
    attr.rnr_retry = IBVERBS_RNR_RETRY;
    attr.sq_psn = conn->psn;
    attr.max_rd_atomic = 1;

    ret = ibv_modify_qp(conn->qp, &attr, IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
        IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC);
    if(ret)
        throw WorkerException(ibvErrStr("ibv_modify_qp (RTS)", ret) );
}

void IbverbsEndpoint::putLocalInfo(bpt::ptree& outTree, const IbvConn* conn) const
{
    outTree.put(NETBENCH_JSON_ADDRFORMAT, localAddr.format);
    outTree.put(NETBENCH_JSON_ADDR, localAddr.toHex() );
    outTree.put(NETBENCH_JSON_IBV_LID, portAttr.lid);
    outTree.put(NETBENCH_JSON_IBV_GID, gidToHex(gid) );
    outTree.put(NETBENCH_JSON_IBV_GIDTYPE, gidType);
    outTree.put(NETBENCH_JSON_IBV_QPN, conn->qp->qp_num);
    outTree.put(NETBENCH_JSON_IBV_PSN, conn->psn);
    outTree.put(NETBENCH_JSON_IBV_MTU, (int)portAttr.active_mtu);
}

IbverbsEndpoint::PeerInfo IbverbsEndpoint::readPeerInfo(const bpt::ptree& tree) const
{
    PeerInfo peer = {};

    peer.addr = NetBenchAddr::fromHex(tree.get<uint32_t>(NETBENCH_JSON_ADDRFORMAT),
        tree.get<std::string>(NETBENCH_JSON_ADDR) );
    peer.lid = tree.get<uint16_t>(NETBENCH_JSON_IBV_LID);
    peer.gidType = tree.get<int>(NETBENCH_JSON_IBV_GIDTYPE);
    peer.qpn = tree.get<uint32_t>(NETBENCH_JSON_IBV_QPN);
    peer.psn = tree.get<uint32_t>(NETBENCH_JSON_IBV_PSN);
    peer.mtu = tree.get<int>(NETBENCH_JSON_IBV_MTU);

    if(!gidFromHex(tree.get<std::string>(NETBENCH_JSON_IBV_GID), peer.gid) )
        throw WorkerException("Invalid GID in netbench connect message: " +
            tree.get<std::string>(NETBENCH_JSON_IBV_GID) );

    return peer;
}

/**
 * Client: create our queue pair and put its details into the connect request.
 */
NetBenchConn* IbverbsEndpoint::prepareConnect(uint32_t rank, bpt::ptree& request)
{
    IbvConn* conn = createConn(rank);

    putLocalInfo(request, conn);

    return conn;
}

/**
 * Client: connect our queue pair to the server's from the reply.
 */
void IbverbsEndpoint::completeConnect(NetBenchConn* conn, const bpt::ptree& reply)
{
    IbvConn* ibvConn = static_cast<IbvConn*>(conn);
    const PeerInfo peer = readPeerInfo(reply);

    ibvConn->peerAddr = peer.addr;

    connectQp(ibvConn, peer);
}

/**
 * Server: create a queue pair for the client, connect it (the client cannot send before it got
 * our reply) and put its details into the reply.
 */
NetBenchConn* IbverbsEndpoint::acceptPeer(const bpt::ptree& request, bpt::ptree& reply)
{
    const uint32_t clientRank = request.get<uint32_t>(NETBENCH_JSON_CLIENTRANK);

    if(NetBenchConn* existing = findConn(clientRank) )
    { // (a repeated connect of the same client just gets the same reply)
        putLocalInfo(reply, static_cast<IbvConn*>(existing) );
        return existing;
    }

    const PeerInfo peer = readPeerInfo(request);

    if(peer.addr.isSockAddr() && localAddr.isSockAddr() &&
        (peer.addr.getFamily() != localAddr.getFamily() ) )
        throw ProgException("Address family mismatch. Client: " + peer.addr.ipToStr() + "; "
            "Server: " + localAddr.ipToStr() + ". Select interfaces of the same family via "
            "\"--" "netdevs" "\".");

    IbvConn* conn = createConn(clientRank);
    conn->peerAddr = peer.addr;

    connectQp(conn, peer);
    putLocalInfo(reply, conn);

    return conn;
}

std::string IbverbsEndpoint::gidToHex(const union ibv_gid& gid)
{
    static const char digits[] = "0123456789abcdef";
    std::string hex;

    for(uint8_t c : gid.raw)
    {
        hex += digits[c >> 4];
        hex += digits[c & 0xF];
    }

    return hex;
}

bool IbverbsEndpoint::gidFromHex(const std::string& hex, union ibv_gid& outGid)
{
    if(hex.size() != 2 * sizeof(outGid.raw) )
        return false;

    for(size_t i = 0; i < sizeof(outGid.raw); i++)
    {
        try { outGid.raw[i] = std::stoul(hex.substr(2 * i, 2), NULL, 16); }
        catch(...) { return false; }
    }

    return true;
}

#endif // IBVERBS_SUPPORT
