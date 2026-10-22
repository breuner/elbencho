// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <algorithm>
#include <arpa/inet.h>
#include <cstdlib>
#include <cstring>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <thread>
#include <unistd.h>

#include "Logger.h"
#include "modes/netbench/NetBenchEngine.h"
#include "modes/netbench/NetBenchEngineIbverbs.h"
#include "modes/netbench/NetBenchEngineLibfabric.h"
#include "modes/netbench/NetBenchEngineRdmaCm.h"
#include "modes/netbench/NetBenchEngineTcp.h"
#include "modes/netbench/NetBenchProtocol.h"
#include "workers/WorkerException.h"

#define NETBENCH_NOWAIT_SLEEP_MS        1 // backoff for endpoints without a wait object


const char* netBenchEngineName(NetBenchEngineType engine)
{
    switch(engine)
    {
        case NetBenchEngineType::Tcp: return "tcp";
        case NetBenchEngineType::Ibverbs: return "ibverbs";
        case NetBenchEngineType::Rdmacm: return "rdmacm";
        case NetBenchEngineType::Libfabric: return "libfabric";
    }

    return "unknown";
}

bool netBenchEngineFromName(const std::string& name, NetBenchEngineType& outEngine)
{
    for(NetBenchEngineType engine : { NetBenchEngineType::Tcp, NetBenchEngineType::Ibverbs,
        NetBenchEngineType::Rdmacm, NetBenchEngineType::Libfabric } )
        if(name == netBenchEngineName(engine) )
        {
            outEngine = engine;
            return true;
        }

    return false;
}

bool netBenchEngineIsBuiltIn(NetBenchEngineType engine)
{
    switch(engine)
    {
        case NetBenchEngineType::Tcp: return true;
#ifdef IBVERBS_SUPPORT
        case NetBenchEngineType::Ibverbs: return true;
#endif
#ifdef RDMACM_SUPPORT
        case NetBenchEngineType::Rdmacm: return true;
#endif
#ifdef LIBFABRIC_SUPPORT
        case NetBenchEngineType::Libfabric: return true;
#endif
        default: return false;
    }
}

std::string netBenchEngineNamesStr()
{
    return std::string(netBenchEngineName(NetBenchEngineType::Tcp) ) + ", " +
        netBenchEngineName(NetBenchEngineType::Ibverbs) + ", " +
        netBenchEngineName(NetBenchEngineType::Rdmacm) + ", " +
        netBenchEngineName(NetBenchEngineType::Libfabric);
}

bool NetBenchAddr::isSockAddr() const
{
    const int family = getFamily();

    return ( (family == AF_INET) && (bytes.size() >= sizeof(sockaddr_in) ) ) ||
        ( (family == AF_INET6) && (bytes.size() >= sizeof(sockaddr_in6) ) );
}

int NetBenchAddr::getFamily() const
{
    if(bytes.size() < sizeof(struct sockaddr) )
        return AF_UNSPEC;

    const int family = getSockAddr()->sa_family;

    return ( (family == AF_INET) || (family == AF_INET6) ) ? family : AF_UNSPEC;
}

std::string NetBenchAddr::toStr() const
{
    return isSockAddr() ? NetBenchIP::sockAddrToStr(getSockAddr() ) : toHex();
}

std::string NetBenchAddr::ipToStr() const
{
    if(!isSockAddr() )
        return toHex();

    char ipStr[INET6_ADDRSTRLEN] = "";

    if(getFamily() == AF_INET)
        inet_ntop(AF_INET, &( (const sockaddr_in*)getSockAddr() )->sin_addr, ipStr, sizeof(ipStr) );
    else
        inet_ntop(AF_INET6, &( (const sockaddr_in6*)getSockAddr() )->sin6_addr, ipStr,
            sizeof(ipStr) );

    return ipStr;
}

std::string NetBenchAddr::toHex() const
{
    static const char digits[] = "0123456789abcdef";
    std::string hex;

    for(unsigned char c : bytes)
    {
        hex += digits[c >> 4];
        hex += digits[c & 0xF];
    }

    return hex;
}

NetBenchAddr NetBenchAddr::fromHex(uint32_t format, const std::string& hex)
{
    if(hex.size() % 2)
        throw WorkerException("Invalid address hex string: " + hex);

    NetBenchAddr addr;
    addr.format = format;

    for(size_t i = 0; i < hex.size(); i += 2)
        addr.bytes += (char)std::stoul(hex.substr(i, 2), NULL, 16);

    return addr;
}

NetBenchAddr NetBenchAddr::fromSockAddr(const struct sockaddr* sockAddr, size_t len)
{
    NetBenchAddr addr;
    addr.format = sockAddr->sa_family;
    addr.bytes.assign( (const char*)sockAddr, len);

    return addr;
}


/**
 * Open an endpoint of the given engine.
 *
 * @param isServer true for an endpoint that accepts client streams, false for a client endpoint.
 * @param fixedPort listen port for socket based engines; 0 for a port chosen by the OS.
 * @param slabSize size of the message buffer memory, accessible via getSlab().
 * @param numConns connections this endpoint will have (queue sizing hint).
 * @param recvsPerConn, sendsPerConn max receives/sends in flight per connection (queue sizing).
 * @throw WorkerException on error.
 */
std::unique_ptr<NetBenchEndpoint> NetBenchEndpoint::create(NetBenchEngineType engine,
    bool isServer, const NetBenchSelector& selector, unsigned short fixedPort, size_t slabSize,
    size_t numConns, size_t recvsPerConn, size_t sendsPerConn)
{
    switch(engine)
    {
        case NetBenchEngineType::Tcp:
            return std::make_unique<TcpEndpoint>(isServer, selector, fixedPort, slabSize,
                numConns);

#ifdef IBVERBS_SUPPORT
        case NetBenchEngineType::Ibverbs:
            return std::make_unique<IbverbsEndpoint>(selector, slabSize, numConns, recvsPerConn,
                sendsPerConn);
#endif
#ifdef RDMACM_SUPPORT
        case NetBenchEngineType::Rdmacm:
            return std::make_unique<RdmaCmEndpoint>(isServer, selector, fixedPort, slabSize,
                numConns, recvsPerConn, sendsPerConn);
#endif

#ifdef LIBFABRIC_SUPPORT
        case NetBenchEngineType::Libfabric:
            return std::make_unique<LibfabricEndpoint>(selector, fixedPort, slabSize);
#endif

        default:
            throw WorkerException(std::string("This executable was built without support for "
                "the netbench engine \"") + netBenchEngineName(engine) + "\".");
    }
}

/**
 * Allocate the message buffers: page-aligned and touched, because engines may pin them.
 */
NetBenchEndpoint::NetBenchEndpoint(size_t slabSize)
{
    const size_t pageSize = sysconf(_SC_PAGESIZE);
    this->slabSize = std::max(slabSize, pageSize);

    void* slabRaw;
    if(posix_memalign(&slabRaw, pageSize, this->slabSize) )
        throw WorkerException("Allocation of " + std::to_string(this->slabSize) +
            " bytes for message buffers failed");

    slab = (char*)slabRaw;
    memset(slab, 0, this->slabSize);
}

NetBenchEndpoint::~NetBenchEndpoint()
{
    free(slab);
}

NetBenchConn* NetBenchEndpoint::findConn(uint32_t rank) const
{
    for(const auto& conn : conns)
        if(conn->rank == rank)
            return conn.get();

    return NULL;
}

/**
 * Collect the wait objects of the given endpoints.
 *
 * @return false if the caller must not block, because completions are pending or an endpoint has
 *      no wait object.
 */
bool NetBenchEndpoint::prepareWait(const std::vector<NetBenchEndpoint*>& endpoints,
    std::vector<struct pollfd>& outFDs)
{
    for(NetBenchEndpoint* endpoint : endpoints)
        if(endpoint->completionsPending() || !endpoint->addWaitFDs(outFDs) )
            return false;

    return true;
}

/**
 * Block until one of the given endpoints has completions or the timeout expires.
 *
 * @return false if this returned immediately because completions are already pending.
 */
bool NetBenchEndpoint::waitForCompletions(const std::vector<NetBenchEndpoint*>& endpoints,
    int timeoutMS)
{
    std::vector<struct pollfd> pollFDs;

    if(!prepareWait(endpoints, pollFDs) )
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(NETBENCH_NOWAIT_SLEEP_MS) );
        return false;
    }

    poll(pollFDs.data(), pollFDs.size(), timeoutMS);

    return true;
}

/**
 * Prepare endpoints for a clean close: consume the completions of messages that still arrive
 * after the benchmark stopped (e.g. after an interrupt while the peers were still sending) and
 * keep the receives posted. Returns when nothing arrived for a short while or after a maximum
 * duration.
 *
 * This is for older libfabric versions: 1.17 and 2.1 process pending completions while they
 * close the connections of an endpoint and crash when one belongs to a connection that is
 * already closed (fixed in later versions). The callers' receive pools cover all messages that
 * the peers can have in flight, so nothing gets queued as unexpected message either. For the
 * other engines this just consumes what the peers still sent.
 *
 * @param repostRecvs reposts the receive slots that completed since the last call.
 */
void NetBenchEndpoint::drain(const std::vector<NetBenchEndpoint*>& endpoints,
    const std::function<void()>& repostRecvs)
{
    if(endpoints.empty() )
        return;

    const auto startT = std::chrono::steady_clock::now();
    auto lastCompletionT = startT;
    NetBenchCompletion entries[NETBENCH_CQ_BATCH_SIZE];

    try
    {
        for( ; ; )
        {
            bool gotCompletions = false;

            for(NetBenchEndpoint* endpoint : endpoints)
            {
                try
                {
                    if(endpoint->readCompletions(entries, NETBENCH_CQ_BATCH_SIZE) )
                        gotCompletions = true;
                }
                catch(WorkerException& e)
                { // (completion errors of a run that is over are irrelevant here)
                    gotCompletions = true;
                }
            }

            repostRecvs();

            const auto now = std::chrono::steady_clock::now();

            if(gotCompletions)
                lastCompletionT = now;
            else
            if( (now - lastCompletionT > std::chrono::milliseconds(NETBENCH_DRAIN_QUIET_MS) ) ||
                (now - startT > std::chrono::milliseconds(NETBENCH_DRAIN_MAX_MS) ) )
                return;
            else
                waitForCompletions(endpoints, NETBENCH_DRAIN_QUIET_MS);
        }
    }
    catch(WorkerException& e)
    { // posting failed, so the endpoint is beyond draining
        LOGGER(Log_DEBUG, "Netbench endpoint drain stopped: " << e.what() << std::endl);
    }
}


namespace NetBenchIP
{

/**
 * Parse an IP address literal into a socket address.
 *
 * @return false if the string is not an IP address.
 */
bool parseIP(const std::string& str, sockaddr_storage& outAddr)
{
    memset(&outAddr, 0, sizeof(outAddr) );

    sockaddr_in* addr4 = (sockaddr_in*)&outAddr;
    if(inet_pton(AF_INET, str.c_str(), &addr4->sin_addr) == 1)
    {
        addr4->sin_family = AF_INET;
        return true;
    }

    sockaddr_in6* addr6 = (sockaddr_in6*)&outAddr;
    if(inet_pton(AF_INET6, str.c_str(), &addr6->sin6_addr) == 1)
    {
        addr6->sin6_family = AF_INET6;
        return true;
    }

    return false;
}

/**
 * Compare the IP part of two socket addresses (ignoring port and scope).
 */
bool sameIP(const struct sockaddr* a, const struct sockaddr* b)
{
    if(a->sa_family != b->sa_family)
        return false;

    if(a->sa_family == AF_INET)
        return !memcmp(&( (const sockaddr_in*)a)->sin_addr, &( (const sockaddr_in*)b)->sin_addr,
            sizeof(in_addr) );

    if(a->sa_family == AF_INET6)
        return !memcmp(&( (const sockaddr_in6*)a)->sin6_addr,
            &( (const sockaddr_in6*)b)->sin6_addr, sizeof(in6_addr) );

    return false;
}

/**
 * @param cidr e.g. "192.168.1.0/24" or "fd00::/64".
 */
bool subnetContains(const std::string& cidr, const struct sockaddr* addr)
{
    const size_t slashPos = cidr.find('/');
    if(slashPos == std::string::npos)
        return false;

    sockaddr_storage netAddr;
    if(!parseIP(cidr.substr(0, slashPos), netAddr) ||
        ( ( (const sockaddr*)&netAddr)->sa_family != addr->sa_family) )
        return false;

    unsigned prefixLen;
    try { prefixLen = std::stoul(cidr.substr(slashPos + 1) ); } catch(...) { return false; }

    const unsigned char* netBytes;
    const unsigned char* addrBytes;
    unsigned addrBits;

    if(addr->sa_family == AF_INET)
    {
        netBytes = (const unsigned char*)&( (const sockaddr_in*)&netAddr)->sin_addr;
        addrBytes = (const unsigned char*)&( (const sockaddr_in*)addr)->sin_addr;
        addrBits = 32;
    }
    else
    if(addr->sa_family == AF_INET6)
    {
        netBytes = (const unsigned char*)&( (const sockaddr_in6*)&netAddr)->sin6_addr;
        addrBytes = (const unsigned char*)&( (const sockaddr_in6*)addr)->sin6_addr;
        addrBits = 128;
    }
    else
        return false;

    if(prefixLen > addrBits)
        return false;

    for(unsigned bit = 0; bit < prefixLen; bit++)
    {
        const unsigned char mask = 0x80 >> (bit % 8);
        if( (netBytes[bit / 8] & mask) != (addrBytes[bit / 8] & mask) )
            return false;
    }

    return true;
}

/**
 * @param family required address family or AF_UNSPEC for the interface's first address.
 * @return false if the interface has no such address.
 */
bool resolveIfaceAddr(const std::string& ifaceName, int family, sockaddr_storage& outAddr)
{
    struct ifaddrs* ifAddrs;
    if(getifaddrs(&ifAddrs) )
        return false;

    bool found = false;

    for(struct ifaddrs* ifa = ifAddrs; ifa && !found; ifa = ifa->ifa_next)
    {
        if(!ifa->ifa_addr || (ifaceName != ifa->ifa_name) )
            continue;

        if( (ifa->ifa_addr->sa_family != AF_INET) && (ifa->ifa_addr->sa_family != AF_INET6) )
            continue;

        if( (family != AF_UNSPEC) && (ifa->ifa_addr->sa_family != family) )
            continue;

        memset(&outAddr, 0, sizeof(outAddr) );
        memcpy(&outAddr, ifa->ifa_addr, sockAddrLen(ifa->ifa_addr) );
        found = true;
    }

    freeifaddrs(ifAddrs);

    return found;
}

namespace
{
    /**
     * @return number of leading one bits of a netmask.
     */
    unsigned prefixLenOfMask(const struct sockaddr* mask)
    {
        const unsigned char* bytes = (mask->sa_family == AF_INET) ?
            (const unsigned char*)&( (const sockaddr_in*)mask)->sin_addr :
            (const unsigned char*)&( (const sockaddr_in6*)mask)->sin6_addr;
        const unsigned numBits = (mask->sa_family == AF_INET) ? 32 : 128;

        unsigned prefixLen = 0;

        for( ; (prefixLen < numBits) && (bytes[prefixLen / 8] & (0x80 >> (prefixLen % 8) ) );
            prefixLen++);

        return prefixLen;
    }

    /**
     * @return "network/prefix" of the address with the given prefix length.
     */
    std::string subnetStr(const struct sockaddr* addr, unsigned prefixLen)
    {
        unsigned char bytes[sizeof(in6_addr)];
        const size_t len = (addr->sa_family == AF_INET) ? sizeof(in_addr) : sizeof(in6_addr);

        memcpy(bytes, (addr->sa_family == AF_INET) ?
            (const void*)&( (const sockaddr_in*)addr)->sin_addr :
            (const void*)&( (const sockaddr_in6*)addr)->sin6_addr, len);

        for(unsigned bit = prefixLen; bit < (len * 8); bit++)
            bytes[bit / 8] &= ~(0x80 >> (bit % 8) );

        char netStr[INET6_ADDRSTRLEN] = "";
        inet_ntop(addr->sa_family, bytes, netStr, sizeof(netStr) );

        return std::string(netStr) + "/" + std::to_string(prefixLen);
    }

    /**
     * Find the interface that has the given address, or whose subnet covers it (e.g. "lo" for
     * 127.0.0.2).
     *
     * @return false if there is none.
     */
    bool findIface(const struct sockaddr* addr, std::string& outName, unsigned& outPrefixLen)
    {
        struct ifaddrs* ifAddrs;
        if(getifaddrs(&ifAddrs) )
            return false;

        bool exact = false;
        bool found = false;

        for(struct ifaddrs* ifa = ifAddrs; ifa && !exact; ifa = ifa->ifa_next)
        {
            if(!ifa->ifa_addr || !ifa->ifa_netmask || (ifa->ifa_addr->sa_family != addr->sa_family) )
                continue;

            exact = NetBenchIP::sameIP(ifa->ifa_addr, addr);

            if(!exact && (found || !NetBenchIP::subnetContains(
                subnetStr(ifa->ifa_addr, prefixLenOfMask(ifa->ifa_netmask) ), addr) ) )
                continue;

            outName = ifa->ifa_name;
            outPrefixLen = prefixLenOfMask(ifa->ifa_netmask);
            found = true;
        }

        freeifaddrs(ifAddrs);

        return found;
    }

    bool isLinkLocal(const struct sockaddr* addr)
    {
        return (addr->sa_family == AF_INET6) &&
            IN6_IS_ADDR_LINKLOCAL(&( (const sockaddr_in6*)addr)->sin6_addr);
    }
} // anonymous namespace

/**
 * @return name of the interface that has the given address, or whose subnet covers it (e.g.
 *      "lo" for 127.0.0.2); "-" if there is none.
 */
std::string ifaceNameForAddr(const struct sockaddr* addr)
{
    std::string name;
    unsigned prefixLen;

    return findIface(addr, name, prefixLen) ? name : "-";
}

/**
 * The subnet of an address: the network of the interface that has or covers the address, with
 * the longer of the interface's prefix and the given minimum (a subnet given by the user narrows
 * a wider netmask). An address that no interface covers is a subnet of its own.
 */
std::string ifaceSubnet(const struct sockaddr* addr, unsigned minPrefixLen)
{
    const unsigned addrBits = (addr->sa_family == AF_INET) ? 32 : 128;
    std::string name;
    unsigned prefixLen = addrBits;

    findIface(addr, name, prefixLen);

    return subnetStr(addr, std::min(addrBits, std::max(prefixLen, minPrefixLen) ) );
}

/**
 * Append the addresses of a named interface, one per subnet, without IPv6 link-local addresses.
 *
 * @param family AF_INET, AF_INET6 or AF_UNSPEC for both.
 */
void ifaceAddrs(const std::string& ifaceName, int family, NetBenchIfaceVec& out)
{
    struct ifaddrs* ifAddrs;
    if(getifaddrs(&ifAddrs) )
        return;

    StringVec seenSubnets; // (of this interface; other interfaces may be in the same subnet)

    for(struct ifaddrs* ifa = ifAddrs; ifa; ifa = ifa->ifa_next)
    {
        if(!ifa->ifa_addr || !ifa->ifa_netmask || (ifaceName != ifa->ifa_name) )
            continue;

        if( (ifa->ifa_addr->sa_family != AF_INET) && (ifa->ifa_addr->sa_family != AF_INET6) )
            continue;

        if( ( (family != AF_UNSPEC) && (ifa->ifa_addr->sa_family != family) ) ||
            isLinkLocal(ifa->ifa_addr) )
            continue;

        NetBenchIface iface;
        iface.addr = NetBenchAddr::fromSockAddr(ifa->ifa_addr, sockAddrLen(ifa->ifa_addr) ).ipToStr();
        iface.subnet = subnetStr(ifa->ifa_addr, prefixLenOfMask(ifa->ifa_netmask) );

        if(std::find(seenSubnets.begin(), seenSubnets.end(), iface.subnet) == seenSubnets.end() )
        {
            seenSubnets.push_back(iface.subnet);
            out.push_back(iface);
        }
    }

    freeifaddrs(ifAddrs);
}

/**
 * Find the local address that the kernel would use to reach a peer (via a connected UDP socket,
 * which sends nothing).
 */
bool routeSourceAddr(const struct sockaddr* peer, socklen_t peerLen, sockaddr_storage& outAddr)
{
    int fd = socket(peer->sa_family, SOCK_DGRAM, 0);
    if(fd < 0)
        return false;

    sockaddr_storage peerCopy;
    memset(&peerCopy, 0, sizeof(peerCopy) );
    memcpy(&peerCopy, peer, peerLen);

    // (a connect needs a port, any non-zero one does)
    if(peer->sa_family == AF_INET)
        ( (sockaddr_in*)&peerCopy)->sin_port = htons(9);
    else
        ( (sockaddr_in6*)&peerCopy)->sin6_port = htons(9);

    socklen_t outLen = sizeof(outAddr);
    memset(&outAddr, 0, sizeof(outAddr) );

    const bool ok = !connect(fd, (const sockaddr*)&peerCopy, peerLen) &&
        !getsockname(fd, (sockaddr*)&outAddr, &outLen);

    close(fd);

    if(ok)
    { // (only the IP is of interest)
        if(outAddr.ss_family == AF_INET)
            ( (sockaddr_in*)&outAddr)->sin_port = 0;
        else
            ( (sockaddr_in6*)&outAddr)->sin6_port = 0;
    }

    return ok;
}

/**
 * Append every interface address inside the subnet, one per interface and subnet. The subnet of
 * an appended address is the interface's own, narrowed to the given prefix (see ifaceSubnet), or
 * the given subnet itself with cidrSubnet (for host addresses in a routed network, whose netmask
 * says nothing about the network).
 *
 * @param skipLinkLocal true to leave out IPv6 link-local addresses (every link has fe80::/64).
 */
void expandSubnet(const std::string& cidr, bool skipLinkLocal, NetBenchIfaceVec& out,
    bool cidrSubnet)
{
    const size_t slashPos = cidr.find('/');
    unsigned minPrefixLen = 0;
    sockaddr_storage netAddr;

    if( (slashPos == std::string::npos) || !parseIP(cidr.substr(0, slashPos), netAddr) )
        return;

    try { minPrefixLen = std::stoul(cidr.substr(slashPos + 1) ); } catch(...) { return; }

    const std::string cidrSubnetStr = subnetStr( (const sockaddr*)&netAddr, minPrefixLen);

    struct ifaddrs* ifAddrs;
    if(getifaddrs(&ifAddrs) )
        return;

    std::vector<std::pair<std::string, std::string>> seen; // (interface name, subnet)

    for(struct ifaddrs* ifa = ifAddrs; ifa; ifa = ifa->ifa_next)
    {
        if(!ifa->ifa_addr || !ifa->ifa_netmask || !subnetContains(cidr, ifa->ifa_addr) )
            continue;

        if(skipLinkLocal && isLinkLocal(ifa->ifa_addr) )
            continue;

        NetBenchIface iface;
        iface.addr = NetBenchAddr::fromSockAddr(ifa->ifa_addr, sockAddrLen(ifa->ifa_addr) ).ipToStr();
        iface.subnet = cidrSubnet ? cidrSubnetStr : subnetStr(ifa->ifa_addr,
            std::max(prefixLenOfMask(ifa->ifa_netmask), minPrefixLen) );

        const std::pair<std::string, std::string> key(ifa->ifa_name, iface.subnet);

        if(std::find(seen.begin(), seen.end(), key) == seen.end() )
        {
            seen.push_back(key);
            out.push_back(iface);
        }
    }

    freeifaddrs(ifAddrs);
}

/**
 * @return true if both addresses have the same family, or if one is no IP address (a device
 *      name for the engine).
 */
bool ifaceFamilyMatch(const NetBenchIface& a, const NetBenchIface& b)
{
    sockaddr_storage addrA, addrB;

    if(!parseIP(a.addr, addrA) || !parseIP(b.addr, addrB) )
        return true;

    return addrA.ss_family == addrB.ss_family;
}

/**
 * @return true if each address lies in the other's subnet (so a netmask mismatch between the
 *      hosts does not matter), or if one of the subnets is unknown.
 */
bool ifaceSubnetMatch(const NetBenchIface& a, const NetBenchIface& b)
{
    sockaddr_storage addrA, addrB;

    if(a.subnet.empty() || b.subnet.empty() || !parseIP(a.addr, addrA) || !parseIP(b.addr, addrB) )
        return true;

    return subnetContains(a.subnet, (const sockaddr*)&addrB) &&
        subnetContains(b.subnet, (const sockaddr*)&addrA);
}

/**
 * Split "host:port" or "[v6addr]:port" into its parts.
 */
void splitHostPort(const std::string& hostPort, std::string& outHost, std::string& outPort)
{
    const size_t bracketPos = hostPort.rfind(']');
    const size_t colonPos = hostPort.rfind(':');

    if( (colonPos == std::string::npos) ||
        ( (bracketPos != std::string::npos) && (colonPos < bracketPos) ) )
    {
        outHost = hostPort;
        outPort.clear();
    }
    else
    {
        outHost = hostPort.substr(0, colonPos);
        outPort = hostPort.substr(colonPos + 1);
    }

    if( (outHost.size() > 1) && (outHost.front() == '[') && (outHost.back() == ']') )
        outHost = outHost.substr(1, outHost.size() - 2);
}

/**
 * @return all IP addresses of a host name (or the address itself); empty if unresolvable.
 */
std::vector<NetBenchAddr> resolveHost(const std::string& host)
{
    std::vector<NetBenchAddr> result;

    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* addrList;
    if(getaddrinfo(host.c_str(), NULL, &hints, &addrList) )
        return result;

    for(struct addrinfo* addr = addrList; addr; addr = addr->ai_next)
        result.push_back(NetBenchAddr::fromSockAddr(addr->ai_addr, addr->ai_addrlen) );

    freeaddrinfo(addrList);

    return result;
}

socklen_t sockAddrLen(const struct sockaddr* addr)
{
    return (addr->sa_family == AF_INET6) ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
}

std::string sockAddrToStr(const struct sockaddr* addr)
{
    char ipStr[INET6_ADDRSTRLEN] = "";

    if(addr->sa_family == AF_INET)
    {
        const sockaddr_in* addr4 = (const sockaddr_in*)addr;
        inet_ntop(AF_INET, &addr4->sin_addr, ipStr, sizeof(ipStr) );
        return std::string(ipStr) + ":" + std::to_string(ntohs(addr4->sin_port) );
    }

    const sockaddr_in6* addr6 = (const sockaddr_in6*)addr;
    inet_ntop(AF_INET6, &addr6->sin6_addr, ipStr, sizeof(ipStr) );
    return "[" + std::string(ipStr) + "]:" + std::to_string(ntohs(addr6->sin6_port) );
}

} // namespace NetBenchIP
