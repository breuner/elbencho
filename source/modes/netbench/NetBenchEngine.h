// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef MODES_NETBENCH_NETBENCHENGINE_H_
#define MODES_NETBENCH_NETBENCHENGINE_H_

#include <boost/property_tree/ptree.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <vector>

#include "Common.h"

namespace bpt = boost::property_tree;

/**
 * The transport engines of netbench mode. Engine names are the values of "--netengine".
 */
enum class NetBenchEngineType
{
    Tcp = 0, // plain TCP sockets
    Ibverbs, // InfiniBand/RoCE reliable connections via libibverbs
    Rdmacm, // the same connections, set up by RDMA CM (librdmacm)
    Libfabric, // libfabric reliable datagram endpoints (any provider)
};

const char* netBenchEngineName(NetBenchEngineType engine);
bool netBenchEngineFromName(const std::string& name, NetBenchEngineType& outEngine);
bool netBenchEngineIsBuiltIn(NetBenchEngineType engine);
std::string netBenchEngineNamesStr(); // "tcp, ibverbs, rdmacm, libfabric"

/**
 * A raw endpoint address as exchanged between clients and servers. For the tcp and ibverbs
 * engines this is a socket address, for libfabric it may be any provider-specific format.
 */
struct NetBenchAddr
{
    uint32_t format{0}; // engine-specific (libfabric address format, address family otherwise)
    std::string bytes;

    bool isSockAddr() const;
    int getFamily() const; // AF_INET or AF_INET6 for socket addresses, AF_UNSPEC otherwise
    const struct sockaddr* getSockAddr() const { return (const struct sockaddr*)bytes.data(); }
    std::string toStr() const; // "ip:port" for socket addresses, hex otherwise
    std::string ipToStr() const; // "ip" for socket addresses, hex otherwise
    std::string toHex() const;
    static NetBenchAddr fromHex(uint32_t format, const std::string& hex);
    static NetBenchAddr fromSockAddr(const struct sockaddr* addr, size_t len);
};

/**
 * What to look for when opening an endpoint.
 */
struct NetBenchSelector
{
    std::string netdev; // interface name, IP address or (libfabric) domain name; empty for any
    int family{AF_UNSPEC}; // required address family; AF_UNSPEC for any
    NetBenchAddr peerAddr; // client: the server's service address, as hint for the interface
    size_t maxMsgSize{0}; // largest message that gets sent or received
    unsigned rnrTimer{1}; // ibverbs, rdmacm: receiver-not-ready retry timer code
    bool rdma{false}; // libfabric: RDMA provider instead of tcp
    std::string provName; // libfabric: explicit provider (overrides rdma); empty for automatic
};

/**
 * One stream connection to a peer, owned by the endpoint that created it. Engines derive their
 * own connection type from this.
 */
struct NetBenchConn
{
    virtual ~NetBenchConn() = default;

    uint32_t rank{0}; // global client thread rank of the stream
    NetBenchAddr peerAddr; // for log messages
};

/**
 * Context of a posted operation. The first member is storage for the libfabric context, the rest
 * is for the callers.
 */
struct NetBenchOpCtx
{
    void* enginePriv[8]; // libfabric fi_context2 storage, must stay the first member
    unsigned type{0}; // NetBenchMsgType
    size_t slot{0};
    bool posted{false}; // receive posted and not completed yet (maintained by the engines)
    std::chrono::steady_clock::time_point startT;
    NetBenchConn* conn{NULL}; // connection of the last post (set by the engines)
};

struct NetBenchCompletion
{
    NetBenchOpCtx* ctx;
    bool isRecv;
    size_t len; // bytes received (receives) or sent
};

/**
 * A data endpoint of one worker thread on one interface: one message buffer slab (registered
 * where the engine needs that) plus the engine's transport objects. A server endpoint accepts a
 * connection per client stream, a client endpoint connects to one server.
 *
 * Connections are set up out-of-band over the service HTTP API: the client puts its endpoint
 * details into the connect request (prepareConnect), the server registers the stream and answers
 * with its own details (acceptPeer), the client completes the connection from the reply
 * (completeConnect). The client's first send on a new connection is the ping that proves the
 * data path, so its completion means "connected" for every engine.
 *
 * Threading: nothing is locked here. NetBenchServer holds its mutex while the HTTP thread accepts
 * peers and posts receives and while the progress thread calls progress()/prepareWait(), the
 * worker threads own their endpoints exclusively once the phase started.
 */
class NetBenchEndpoint
{
    public:
        static std::unique_ptr<NetBenchEndpoint> create(NetBenchEngineType engine, bool isServer,
            const NetBenchSelector& selector, unsigned short fixedPort, size_t slabSize,
            size_t numConns, size_t recvsPerConn, size_t sendsPerConn);

        virtual ~NetBenchEndpoint();

        NetBenchEndpoint(const NetBenchEndpoint&) = delete;
        NetBenchEndpoint& operator=(const NetBenchEndpoint&) = delete;

        virtual NetBenchEngineType getEngine() const = 0;
        virtual std::string getDescription() const = 0; // "<engine words>: ADDRESS (DOMAIN)"
        virtual const NetBenchAddr& getAddr() const = 0;

        virtual NetBenchConn* prepareConnect(uint32_t rank, bpt::ptree& request) = 0;
        virtual void completeConnect(NetBenchConn* conn, const bpt::ptree& reply) = 0;
        virtual NetBenchConn* acceptPeer(const bpt::ptree& request, bpt::ptree& reply) = 0;

        virtual void postRecv(NetBenchConn* conn, void* buf, size_t len, NetBenchOpCtx* ctx) = 0;
        virtual void postSend(NetBenchConn* conn, const void* buf, size_t len,
            NetBenchOpCtx* ctx) = 0;
        virtual size_t readCompletions(NetBenchCompletion* entries, size_t maxEntries) = 0;
        virtual void progress() = 0;
        virtual bool completionsPending() = 0; // true means the caller must not block
        virtual bool addWaitFDs(std::vector<struct pollfd>& outFDs) = 0; // false: no wait object
        virtual void beginPhase() {}

        static bool prepareWait(const std::vector<NetBenchEndpoint*>& endpoints,
            std::vector<struct pollfd>& outFDs);
        static bool waitForCompletions(const std::vector<NetBenchEndpoint*>& endpoints,
            int timeoutMS);
        static void drain(const std::vector<NetBenchEndpoint*>& endpoints,
            const std::function<void()>& repostRecvs);

    protected:
        explicit NetBenchEndpoint(size_t slabSize);

        char* slab{NULL}; // page-aligned message buffers, zeroed
        size_t slabSize;
        std::vector<std::unique_ptr<NetBenchConn>> conns;

        NetBenchConn* findConn(uint32_t rank) const;
        std::string getEngineName() const { return netBenchEngineName(getEngine() ); }

    // inliners
    public:
        char* getSlab() const { return slab; }
        size_t getSlabSize() const { return slabSize; }
};

/**
 * A network interface as a service reports it for the stream placement: its address and the
 * subnet it is in. A device or domain name that only an engine understands is passed through
 * with an empty subnet.
 */
struct NetBenchIface
{
    std::string addr; // IP address, or a device/domain name for the engine
    std::string subnet; // "network/prefix" of the address; empty means unknown (matches any)
};

typedef std::vector<NetBenchIface> NetBenchIfaceVec;

/**
 * IP address and interface helpers shared by the engines and the option parsing.
 */
namespace NetBenchIP
{
    bool parseIP(const std::string& str, sockaddr_storage& outAddr); // IPv4 or IPv6 literal
    bool sameIP(const struct sockaddr* a, const struct sockaddr* b); // ignores port and scope
    bool subnetContains(const std::string& cidr, const struct sockaddr* addr);
    bool resolveIfaceAddr(const std::string& ifaceName, int family, sockaddr_storage& outAddr);
    std::string ifaceNameForAddr(const struct sockaddr* addr); // "-" if unknown
    std::string ifaceSubnet(const struct sockaddr* addr, unsigned minPrefixLen); // "net/prefix"
    void ifaceAddrs(const std::string& ifaceName, int family, NetBenchIfaceVec& out); // 1/subnet
    bool routeSourceAddr(const struct sockaddr* peer, socklen_t peerLen, sockaddr_storage& outAddr);
    void expandSubnet(const std::string& cidr, bool skipLinkLocal, NetBenchIfaceVec& out,
        bool cidrSubnet = false); // cidrSubnet: the cidr is the subnet, not the netmask
    bool ifaceFamilyMatch(const NetBenchIface& a, const NetBenchIface& b); // or unknown
    bool ifaceSubnetMatch(const NetBenchIface& a, const NetBenchIface& b); // mutual, or unknown
    void splitHostPort(const std::string& hostPort, std::string& outHost, std::string& outPort);
    std::vector<NetBenchAddr> resolveHost(const std::string& host); // empty if unresolvable
    socklen_t sockAddrLen(const struct sockaddr* addr);
    std::string sockAddrToStr(const struct sockaddr* addr); // "ip:port" or "[ip]:port"
}

#endif /* MODES_NETBENCH_NETBENCHENGINE_H_ */
