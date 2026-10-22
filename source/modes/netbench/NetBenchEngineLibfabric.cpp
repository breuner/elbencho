// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <netinet/in.h>
#include <set>
#include <sys/resource.h>

#include "Logger.h"
#include "modes/netbench/NetBenchEngineLibfabric.h"
#include "modes/netbench/NetBenchProtocol.h"
#include "ProgException.h"
#include "workers/WorkerException.h"

#ifdef LIBFABRIC_SUPPORT

#define FABRIC_AV_COUNT_HINT        1024 // initial address vector size (grows on demand)
#define FABRIC_POST_RETRY_SEC       20 // max time to retry a post that returned FI_EAGAIN

// the op context handed to libfabric is the NetBenchOpCtx itself (see enginePriv)
static_assert(offsetof(NetBenchOpCtx, enginePriv) == 0, "libfabric context must come first");
static_assert(sizeof(NetBenchOpCtx::enginePriv) >= sizeof(struct fi_context2),
    "libfabric context storage too small");

namespace
{
    std::string fiErrStr(const char* op, int ret)
    {
        return std::string(op) + " failed: " + fi_strerror(-ret) + " (" + std::to_string(-ret) +
            ")" + ( (-ret == EMFILE) ? ". Each libfabric endpoint needs about 10 file descriptors, "
            "so the open files limit (\"ulimit -n\") has to cover threads x interfaces x 10 on a "
            "server." : "");
    }

    /**
     * Raise the soft open files limit to the hard limit, because each libfabric endpoint costs
     * around 10 file descriptors (completion channels, event queues, listeners) and servers open
     * one endpoint per thread and interface.
     */
    void raiseOpenFilesLimit()
    {
        struct rlimit limit;

        if(getrlimit(RLIMIT_NOFILE, &limit) || (limit.rlim_cur >= limit.rlim_max) )
            return;

        limit.rlim_cur = limit.rlim_max;

        if(setrlimit(RLIMIT_NOFILE, &limit) )
            LOGGER(Log_DEBUG, "Raising the open files limit failed: " << strerror(errno) <<
                std::endl);
    }

    unsigned tagBits(const struct fi_info* info)
    {
        uint64_t format = info->ep_attr->mem_tag_format;
        return format ? (64 - __builtin_clzll(format) ) : 64;
    }

    NetBenchAddr infoSrcAddr(const struct fi_info* info)
    {
        NetBenchAddr addr;
        addr.format = info->addr_format;

        if(info->src_addr && info->src_addrlen)
            addr.bytes.assign( (const char*)info->src_addr, info->src_addrlen);

        return addr;
    }

    /**
     * Hints for all fi_getinfo() calls: reliable datagram endpoints with tagged messaging. The
     * memory registration modes are the ones this code can handle, the provider tells us which of
     * them it actually requires.
     */
    struct fi_info* makeHints(const char* provName)
    {
        struct fi_info* hints = fi_allocinfo();
        if(!hints)
            throw WorkerException("fi_allocinfo failed");

        hints->caps = FI_TAGGED;
        hints->mode = FI_CONTEXT | FI_CONTEXT2;
        hints->addr_format = FI_FORMAT_UNSPEC;
        hints->ep_attr->type = FI_EP_RDM;
        hints->domain_attr->threading = FI_THREAD_SAFE;
        hints->domain_attr->data_progress = FI_PROGRESS_MANUAL;
        hints->domain_attr->control_progress = FI_PROGRESS_MANUAL;
        hints->domain_attr->resource_mgmt = FI_RM_ENABLED;
        hints->domain_attr->av_type = FI_AV_TABLE;
        hints->domain_attr->mr_mode = FI_MR_LOCAL | FI_MR_ALLOCATED | FI_MR_PROV_KEY |
            FI_MR_VIRT_ADDR | FI_MR_ENDPOINT | FI_MR_RAW;

        if(provName)
            hints->fabric_attr->prov_name = strdup(provName);

        return hints;
    }
} // anonymous namespace


/**
 * Open an endpoint on the interface and provider matching the selector.
 *
 * @throw WorkerException on error.
 */
LibfabricEndpoint::LibfabricEndpoint(const NetBenchSelector& selector, unsigned short fixedPort,
    size_t slabSize) : NetBenchEndpoint(slabSize)
{
    static std::once_flag limitOnce;
    std::call_once(limitOnce, raiseOpenFilesLimit);

    try
    {
        info = selectInfo(selector);

        if(fixedPort && infoSrcAddr(info).isSockAddr() )
        {
            if(info->src_addr && ( (sockaddr*)info->src_addr)->sa_family == AF_INET6)
                ( (sockaddr_in6*)info->src_addr)->sin6_port = htons(fixedPort);
            else
                ( (sockaddr_in*)info->src_addr)->sin_port = htons(fixedPort);
        }

        int ret = fi_fabric(info->fabric_attr, &fabric, NULL);
        if(ret)
            throw WorkerException(fiErrStr("fi_fabric", ret) );

        ret = fi_domain(fabric, info, &domain, NULL);
        if(ret)
            throw WorkerException(fiErrStr("fi_domain", ret) );

        struct fi_av_attr avAttr = {};
        avAttr.type = FI_AV_TABLE;
        avAttr.count = FABRIC_AV_COUNT_HINT;

        ret = fi_av_open(domain, &avAttr, &av, NULL);
        if(ret)
            throw WorkerException(fiErrStr("fi_av_open", ret) );

        // completion queue with a wait file descriptor if the provider offers one
        struct fi_cq_attr cqAttr = {};
        cqAttr.format = FI_CQ_FORMAT_TAGGED;
        cqAttr.wait_obj = FI_WAIT_FD;

        ret = fi_cq_open(domain, &cqAttr, &cq, NULL);
        if(ret)
        {
            cqAttr.wait_obj = FI_WAIT_NONE;
            ret = fi_cq_open(domain, &cqAttr, &cq, NULL);
            if(ret)
                throw WorkerException(fiErrStr("fi_cq_open", ret) );
        }

        ret = fi_endpoint(domain, info, &ep, NULL);
        if(ret)
            throw WorkerException(fiErrStr("fi_endpoint", ret) );

        ret = fi_ep_bind(ep, &av->fid, 0);
        if(ret)
            throw WorkerException(fiErrStr("fi_ep_bind (av)", ret) );

        ret = fi_ep_bind(ep, &cq->fid, FI_TRANSMIT | FI_RECV);
        if(ret)
            throw WorkerException(fiErrStr("fi_ep_bind (cq)", ret) );

#ifdef NETBENCH_HAVE_FI_1_18
        // efa would otherwise use shared memory for peers on the same host (error ignored, as
        // other providers don't know this option)
        bool shmPermitted = false;
        fi_setopt(&ep->fid, FI_OPT_ENDPOINT, FI_OPT_SHARED_MEMORY_PERMITTED, &shmPermitted,
            sizeof(shmPermitted) );
#endif

        ret = fi_enable(ep);
        if(ret)
            throw WorkerException(fiErrStr("fi_enable", ret) + ( (ret == -FI_ENOMEM) ?
                ". RDMA providers register memory at this point, which needs a sufficient "
                "locked memory limit (\"ulimit -l\")." : "") );

        ret = fi_mr_reg(domain, slab, this->slabSize, FI_SEND | FI_RECV, 0, 0, 0, &mr, NULL);
        if(ret)
            throw WorkerException(fiErrStr("fi_mr_reg", ret) + ". Registering memory for RDMA "
                "needs a sufficient locked memory limit (\"ulimit -l\").");

        if(info->domain_attr->mr_mode & FI_MR_ENDPOINT)
        {
            ret = fi_mr_bind(mr, &ep->fid, 0);
            if(ret)
                throw WorkerException(fiErrStr("fi_mr_bind", ret) );

            ret = fi_mr_enable(mr);
            if(ret)
                throw WorkerException(fiErrStr("fi_mr_enable", ret) );
        }

        mrDesc = fi_mr_desc(mr);

        char addrBuf[512];
        size_t addrLen = sizeof(addrBuf);

        ret = fi_getname(&ep->fid, addrBuf, &addrLen);
        if(ret)
            throw WorkerException(fiErrStr("fi_getname", ret) );

        localAddr.format = info->addr_format;
        localAddr.bytes.assign(addrBuf, addrLen);

        if(cqAttr.wait_obj == FI_WAIT_FD)
        {
            int fd;
            if(!fi_control(&cq->fid, FI_GETWAIT, &fd) )
                waitFD = fd;
        }

        LOGGER(Log_DEBUG, "Opened fabric endpoint: " << getDescription() << "; "
            "mr_mode: " << info->domain_attr->mr_mode << "; "
            "wait fd: " << waitFD << std::endl);
    }
    catch(...)
    {
        closeAll();
        throw;
    }
}

LibfabricEndpoint::~LibfabricEndpoint()
{
    closeAll();
}

/**
 * Close everything in the required order. Pending operations are discarded by the endpoint
 * close.
 */
void LibfabricEndpoint::closeAll()
{
    const bool mrBeforeEp = mr && (info->domain_attr->mr_mode & FI_MR_ENDPOINT);

    if(mrBeforeEp)
        fi_close(&mr->fid);

    if(ep)
        fi_close(&ep->fid);

    if(mr && !mrBeforeEp)
        fi_close(&mr->fid);

    if(cq)
        fi_close(&cq->fid);

    if(av)
        fi_close(&av->fid);

    if(domain)
        fi_close(&domain->fid);

    if(fabric)
        fi_close(&fabric->fid);

    if(info)
        fi_freeinfo(info);

    mr = NULL; ep = NULL; cq = NULL; av = NULL; domain = NULL; fabric = NULL; info = NULL;
}

/**
 * @return "provider protocol: local address (domain)" for log messages.
 */
std::string LibfabricEndpoint::getDescription() const
{
    std::string protocolStr = fi_tostr(&info->ep_attr->protocol, FI_TYPE_PROTOCOL);

    return getProvName() + " " + protocolStr + ": " + localAddr.toStr() + " (" + getDomainName() +
        ")";
}

/**
 * Add a peer address to the address vector.
 *
 * @return the fabric address to use as destination for sends.
 */
fi_addr_t LibfabricEndpoint::insertAddr(const NetBenchAddr& addr)
{
    fi_addr_t fiAddr;

    int ret = fi_av_insert(av, addr.bytes.data(), 1, &fiAddr, 0, NULL);
    if(ret != 1)
        throw WorkerException(fiErrStr("fi_av_insert", (ret < 0) ? ret : -FI_EINVAL) +
            "; address: " + addr.toStr() );

    return fiAddr;
}

void LibfabricEndpoint::putLocalInfo(bpt::ptree& outTree) const
{
    outTree.put(NETBENCH_JSON_PROVIDER, getProvName() );
    outTree.put(NETBENCH_JSON_PROTOCOL, getProtocol() );
    outTree.put(NETBENCH_JSON_PROTOCOLVERSION, getProtocolVersion() );
    outTree.put(NETBENCH_JSON_ADDRFORMAT, localAddr.format);
    outTree.put(NETBENCH_JSON_ADDR, localAddr.toHex() );
    outTree.put(NETBENCH_JSON_LIBVERSION, getRuntimeVersionStr() );
}

void LibfabricEndpoint::checkFamily(const NetBenchAddr& peerAddr, const char* peerRole) const
{
    if(peerAddr.isSockAddr() && localAddr.isSockAddr() &&
        (peerAddr.getFamily() != localAddr.getFamily() ) )
        throw ProgException(std::string("Address family mismatch. ") + peerRole + ": " +
            peerAddr.toStr() + "; own endpoint: " + localAddr.toStr() + ". Select interfaces of "
            "the same family via \"--" "netdevs" "\".");
}

/**
 * Client: put our endpoint details into the connect request.
 */
NetBenchConn* LibfabricEndpoint::prepareConnect(uint32_t rank, bpt::ptree& request)
{
    putLocalInfo(request);

    conns.push_back(std::make_unique<LibfabricConn>() );
    conns.back()->rank = rank;

    return conns.back().get();
}

/**
 * Client: take the server's endpoint address from the connect reply.
 */
void LibfabricEndpoint::completeConnect(NetBenchConn* conn, const bpt::ptree& reply)
{
    LibfabricConn* fabricConn = static_cast<LibfabricConn*>(conn);

    fabricConn->peerAddr = NetBenchAddr::fromHex(reply.get<uint32_t>(NETBENCH_JSON_ADDRFORMAT),
        reply.get<std::string>(NETBENCH_JSON_ADDR) );

    checkFamily(fabricConn->peerAddr, "Server endpoint");

    fabricConn->fiAddr = insertAddr(fabricConn->peerAddr);
}

/**
 * Server: register the client of a connect request and put our endpoint details into the reply.
 */
NetBenchConn* LibfabricEndpoint::acceptPeer(const bpt::ptree& request, bpt::ptree& reply)
{
    const uint32_t clientRank = request.get<uint32_t>(NETBENCH_JSON_CLIENTRANK);
    const std::string provider = request.get<std::string>(NETBENCH_JSON_PROVIDER);
    const std::string clientLibVersion = request.get<std::string>(NETBENCH_JSON_LIBVERSION, "");
    uint32_t clientProtocol = request.get<uint32_t>(NETBENCH_JSON_PROTOCOL);

    putLocalInfo(reply);

    if(NetBenchConn* existing = findConn(clientRank) )
        return existing; // (a repeated connect of the same client just gets the same reply)

    if( (provider != getProvName() ) || (clientProtocol != getProtocol() ) ||
        (request.get<uint32_t>(NETBENCH_JSON_PROTOCOLVERSION) != getProtocolVersion() ) )
    {
        const std::string clientProtocolStr = fi_tostr(&clientProtocol, FI_TYPE_PROTOCOL);
        const bool isTcpMismatch = !provider.rfind("tcp", 0) && !getProvName().rfind("tcp", 0);

        throw ProgException("Client and server use different libfabric providers or protocols. "
            "Client: " + provider + " " + clientProtocolStr + " (libfabric " + clientLibVersion +
            "); Server: " + getDescription() + " (libfabric " + getRuntimeVersionStr() + "). All "
            "hosts need the same provider" +
            (isTcpMismatch ? " and, for tcp, the same libfabric version: the tcp provider's wire "
            "protocol changed in libfabric 1.18 (native tcp instead of tcp;ofi_rxm), so hosts "
            "with older and newer libfabric cannot exchange data over tcp. Build with "
            "\"make BUILD_LIBFABRIC=1\" on all hosts or use an RDMA provider, which "
            "interoperates across versions." : ".") );
    }

    const NetBenchAddr clientAddr = NetBenchAddr::fromHex(
        request.get<uint32_t>(NETBENCH_JSON_ADDRFORMAT),
        request.get<std::string>(NETBENCH_JSON_ADDR) );

    checkFamily(clientAddr, "Client");

    // (the same provider name does not guarantee the same wire protocol across libfabric versions,
    // e.g. the tcp provider was rewritten in 1.18, so this is worth a note)
    if(!clientLibVersion.empty() && (clientLibVersion != getRuntimeVersionStr() ) )
        LOGGER(Log_NORMAL, "NOTE: Netbench client rank " << clientRank << " uses libfabric " <<
            clientLibVersion << ", this server uses " << getRuntimeVersionStr() <<
            ". Mixed versions may not be able to exchange data." << std::endl);

    auto conn = std::make_unique<LibfabricConn>();
    conn->rank = clientRank;
    conn->peerAddr = clientAddr;
    conn->fiAddr = insertAddr(clientAddr);

    conns.push_back(std::move(conn) );

    return conns.back().get();
}

/**
 * Drive provider progress without consuming completions.
 */
void LibfabricEndpoint::progress()
{
    fi_cq_read(cq, NULL, 0);
}

/**
 * Post a tagged receive for the stream of the given connection. Retries with progress while the
 * provider has no free queue slots.
 */
void LibfabricEndpoint::postRecv(NetBenchConn* conn, void* buf, size_t len, NetBenchOpCtx* ctx)
{
    const auto startT = std::chrono::steady_clock::now();
    const uint64_t tag = netBenchMakeTag( (NetBenchMsgType)ctx->type, conn->rank);

    ctx->conn = conn;

    for( ; ; )
    {
        int ret = fi_trecv(ep, buf, len, mrDesc, FI_ADDR_UNSPEC, tag, 0,
            (struct fi_context2*)ctx->enginePriv);
        if(!ret)
        {
            ctx->posted = true;
            return;
        }

        if(ret != -FI_EAGAIN)
            throw WorkerException(fiErrStr("fi_trecv", ret) );

        progress();

        if(std::chrono::steady_clock::now() - startT > std::chrono::seconds(FABRIC_POST_RETRY_SEC) )
            throw WorkerException("fi_trecv kept returning FI_EAGAIN for " +
                std::to_string(FABRIC_POST_RETRY_SEC) + "s");
    }
}

/**
 * Post a tagged send. Retries with progress while the provider has no free queue slots.
 */
void LibfabricEndpoint::postSend(NetBenchConn* conn, const void* buf, size_t len,
    NetBenchOpCtx* ctx)
{
    const auto startT = std::chrono::steady_clock::now();
    const uint64_t tag = netBenchMakeTag( (NetBenchMsgType)ctx->type, conn->rank);
    const fi_addr_t dest = static_cast<LibfabricConn*>(conn)->fiAddr;

    ctx->conn = conn;

    for( ; ; )
    {
        int ret = fi_tsend(ep, buf, len, mrDesc, dest, tag, (struct fi_context2*)ctx->enginePriv);
        if(!ret)
            return;

        if(ret != -FI_EAGAIN)
            throw WorkerException(fiErrStr("fi_tsend", ret) );

        progress();

        if(std::chrono::steady_clock::now() - startT > std::chrono::seconds(FABRIC_POST_RETRY_SEC) )
            throw WorkerException("fi_tsend kept returning FI_EAGAIN for " +
                std::to_string(FABRIC_POST_RETRY_SEC) + "s");
    }
}

/**
 * Read available completions without blocking.
 *
 * @return number of entries filled, 0 if none available.
 * @throw WorkerException on an error completion (except for canceled operations).
 */
size_t LibfabricEndpoint::readCompletions(NetBenchCompletion* entries, size_t maxEntries)
{
    struct fi_cq_tagged_entry cqEntries[NETBENCH_CQ_BATCH_SIZE];
    maxEntries = std::min<size_t>(maxEntries, NETBENCH_CQ_BATCH_SIZE);

    ssize_t ret = fi_cq_read(cq, cqEntries, maxEntries);

    if(ret > 0)
    {
        for(ssize_t i = 0; i < ret; i++)
        {
            NetBenchOpCtx* ctx = (NetBenchOpCtx*)cqEntries[i].op_context;
            const bool isRecv = cqEntries[i].flags & FI_RECV;

            if(isRecv)
                ctx->posted = false;

            entries[i] = { ctx, isRecv, cqEntries[i].len };
        }

        return ret;
    }

    if(ret == -FI_EAGAIN)
        return 0;

    if(ret != -FI_EAVAIL)
        throw WorkerException(fiErrStr("fi_cq_read", ret) );

    struct fi_cq_err_entry err;
    memset(&err, 0, sizeof(err) );

    ret = fi_cq_readerr(cq, &err, 0);
    if(ret < 0)
        throw WorkerException(fiErrStr("fi_cq_readerr", ret) );

    NetBenchOpCtx* ctx = (NetBenchOpCtx*)err.op_context;

    if(ctx && (err.flags & FI_RECV) )
        ctx->posted = false;

    if(err.err == FI_ECANCELED)
        return 0;

    char provErrBuf[256];

    throw WorkerException(std::string("Completion error on ") +
        ( (err.flags & FI_SEND) ? "send" : "receive") + ": " + fi_strerror(err.err) +
        " (" + fi_cq_strerror(cq, err.prov_errno, err.err_data, provErrBuf, sizeof(provErrBuf) ) +
        "); endpoint: " + getDescription() + "; " +
        "stream: " + (ctx ? std::to_string(netBenchTagRank(err.tag) ) : "-") );
}

bool LibfabricEndpoint::completionsPending()
{
    if(waitFD < 0)
        return false;

    struct fid* fids[1] = { &cq->fid };

    return fi_trywait(fabric, fids, 1) == -FI_EAGAIN;
}

bool LibfabricEndpoint::addWaitFDs(std::vector<struct pollfd>& outFDs)
{
    if(waitFD < 0)
        return false;

    outFDs.push_back( { waitFD, POLLIN, 0 } );

    return true;
}

/**
 * @param srcAddr source IP address to bind to; NULL for a list of all interfaces.
 * @return matching infos or NULL if the provider has none (caller frees the list).
 */
struct fi_info* LibfabricEndpoint::getInfoList(const char* provName, const char* srcAddr)
{
    struct fi_info* hints = makeHints(provName);
    struct fi_info* list = NULL;

    int ret = fi_getinfo(NETBENCH_FI_VERSION, srcAddr, NULL, srcAddr ? FI_SOURCE : 0, hints,
        &list);

    fi_freeinfo(hints);

    if(ret == -FI_ENODATA)
        return NULL;

    if(ret)
        throw WorkerException(fiErrStr("fi_getinfo", ret) );

    return list;
}

/**
 * Find the fi_info for the selected provider class and interface.
 *
 * @return a copy that the caller frees via fi_freeinfo().
 */
struct fi_info* LibfabricEndpoint::selectInfo(const NetBenchSelector& selector)
{
    std::vector<std::string> candidates;

    if(!selector.provName.empty() )
        candidates = { selector.provName };
    else
    if(selector.rdma)
        candidates = { "verbs;ofi_rxm", "efa", "cxi", "psm3", "opx", "ucx" };
    else
        candidates = { "tcp" };

    // an IP address is given to the provider directly, so that it also works for addresses that
    // belong to no interface (e.g. 127.0.0.2); anything else is matched against the interface list
    sockaddr_storage ipAddr;
    const char* srcAddr = NetBenchIP::parseIP(selector.netdev, ipAddr) ?
        selector.netdev.c_str() : NULL;

    for(const std::string& candidate : candidates)
    {
        struct fi_info* list = getInfoList(candidate.c_str(), srcAddr);
        if(!list)
            continue;

        struct fi_info* best = pickBestInfo(list, selector);
        struct fi_info* result = best ? fi_dupinfo(best) : NULL;

        fi_freeinfo(list);

        if(result)
            return result;
    }

    std::string candidatesStr;
    for(const std::string& candidate : candidates)
        candidatesStr += (candidatesStr.empty() ? "" : ", ") + candidate;

    throw WorkerException("No usable libfabric provider found. "
        "Tried: " + candidatesStr + "; "
        "interface: " + (selector.netdev.empty() ? std::string("any") : selector.netdev) + "; "
        "address family: " + ( (selector.family == AF_INET) ? "IPv4" :
            (selector.family == AF_INET6) ? "IPv6" : "any") + ". "
        "(\"--netprov list\" lists the available providers and interfaces.)");
}

/**
 * Pick the best entry of a fi_getinfo() list: it has to match the selector, then an exactly
 * named provider, a native provider protocol and the peer's subnet are preferred.
 */
struct fi_info* LibfabricEndpoint::pickBestInfo(struct fi_info* list,
    const NetBenchSelector& selector)
{
    struct fi_info* best = NULL;
    int bestScore = -1;

    for(struct fi_info* info = list; info; info = info->next)
    {
        if(selector.maxMsgSize && (info->ep_attr->max_msg_size < selector.maxMsgSize) )
            continue;

        if(tagBits(info) < NETBENCH_MIN_TAG_BITS)
            continue;

        const NetBenchAddr srcAddr = infoSrcAddr(info);

        if( (selector.family != AF_UNSPEC) && srcAddr.isSockAddr() &&
            (srcAddr.getFamily() != selector.family) )
            continue;

        if(!selector.netdev.empty() && !matchesNetdev(info, selector.netdev) )
            continue;

        int score = 0;

        if(!selector.provName.empty() )
        { // explicit provider: exact name preferred (e.g. "tcp;ofi_rxm" over native "tcp"), a
          // layered variant is accepted where the exact one does not exist (e.g. "tcp" before 1.18)
            const std::string provName = info->fabric_attr->prov_name;

            if(provName == selector.provName)
                score += 4;
            else
            if(provName.rfind(selector.provName + ";", 0) )
                continue;
        }

#ifdef NETBENCH_HAVE_FI_1_18
        if(info->ep_attr->protocol == FI_PROTO_XNET)
            score += 2; // native tcp RDM instead of the rxm layer
#endif

        if(selector.peerAddr.isSockAddr() && info->fabric_attr->name &&
            NetBenchIP::subnetContains(info->fabric_attr->name, selector.peerAddr.getSockAddr() ) )
            score += 1;

        if(score > bestScore)
        {
            best = info;
            bestScore = score;
        }
    }

    return best;
}

/**
 * @param netdev libfabric domain name (e.g. "mlx5_0"), IP address or interface name (e.g. "eth0").
 */
bool LibfabricEndpoint::matchesNetdev(const struct fi_info* info, const std::string& netdev)
{
    if(info->domain_attr->name && (netdev == info->domain_attr->name) )
        return true;

    const NetBenchAddr srcAddr = infoSrcAddr(info);
    if(!srcAddr.isSockAddr() )
        return false;

    sockaddr_storage ipAddr;
    if(NetBenchIP::parseIP(netdev, ipAddr) )
        return NetBenchIP::sameIP( (const sockaddr*)&ipAddr, srcAddr.getSockAddr() );

    // interface name: the info's address must be one of the interface's addresses
    return NetBenchIP::ifaceNameForAddr(srcAddr.getSockAddr() ) == netdev;
}

/**
 * Print the providers and interfaces usable for netbench on this host (for "--netprov list").
 */
void LibfabricEndpoint::printProviders(std::ostream& out)
{
    struct fi_info* list = getInfoList(NULL, NULL);
    std::set<std::string> seenLines; // (providers report some entries more than once)

    out << "libfabric " << getVersionStr() << std::endl;
    out << "Providers and interfaces usable for netbench on this host:" << std::endl;

    if(!list)
        out << "  (none)" << std::endl;

    for(struct fi_info* info = list; info; info = info->next)
    {
        if(tagBits(info) < NETBENCH_MIN_TAG_BITS)
            continue;

        const std::string line = std::string("  ") + info->fabric_attr->prov_name + ": "
            "protocol " + fi_tostr(&info->ep_attr->protocol, FI_TYPE_PROTOCOL) + "; "
            "domain " + info->domain_attr->name + "; "
            "fabric " + info->fabric_attr->name + "; "
            "address " + infoSrcAddr(info).toStr();

        if(seenLines.insert(line).second)
            out << line << std::endl;
    }

    if(list)
        fi_freeinfo(list);
}

/**
 * @return "x.y (runtime: a.b)" versions of the headers and the loaded library.
 */
std::string LibfabricEndpoint::getVersionStr()
{
    return std::to_string(FI_MAJOR_VERSION) + "." + std::to_string(FI_MINOR_VERSION) +
        " (runtime: " + getRuntimeVersionStr() + ")";
}

/**
 * @return "a.b" version of the loaded library.
 */
std::string LibfabricEndpoint::getRuntimeVersionStr()
{
    const uint32_t runtime = fi_version();

    return std::to_string(FI_MAJOR(runtime) ) + "." + std::to_string(FI_MINOR(runtime) );
}

#endif // LIBFABRIC_SUPPORT
