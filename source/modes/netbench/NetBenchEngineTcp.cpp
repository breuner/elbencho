// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>

#include "Logger.h"
#include "modes/netbench/NetBenchEngineTcp.h"
#include "ProgException.h"
#include "workers/WorkerException.h"

#ifndef MSG_NOSIGNAL
    #define MSG_NOSIGNAL 0 // (macOS: SO_NOSIGPIPE is set on the sockets instead)
#endif


/**
 * Open the endpoint. A server listens, a client just resolves the address to bind its connection
 * to (if any).
 *
 * @throw WorkerException on error.
 */
TcpEndpoint::TcpEndpoint(bool isServer, const NetBenchSelector& selector, unsigned short fixedPort,
    size_t slabSize, size_t numConns) : NetBenchEndpoint(slabSize), isServer(isServer)
{
    try
    {
        resolveBindAddr(selector);

        if(isServer)
            openListener(fixedPort, numConns);

        LOGGER(Log_DEBUG, "Opened tcp endpoint: " << getDescription() << std::endl);
    }
    catch(...)
    {
        if(listenFD >= 0)
            close(listenFD);

        throw;
    }
}

TcpEndpoint::~TcpEndpoint()
{
    beginPhase(); // (closes the listener and pending sockets)

    for(const auto& conn : conns)
    {
        TcpConn* tcpConn = static_cast<TcpConn*>(conn.get() );

        if(tcpConn->fd >= 0)
            close(tcpConn->fd);
    }
}

/**
 * Turn the selector's interface into the address to listen on or to connect from: an IP literal
 * is taken as is, an interface name resolves to its first address of the wanted family. A client
 * without interface gets the address that routes to the server, just for the description.
 */
void TcpEndpoint::resolveBindAddr(const NetBenchSelector& selector)
{
    int family = selector.family;

    if( (family == AF_UNSPEC) && selector.peerAddr.isSockAddr() )
        family = selector.peerAddr.getFamily();

    if(NetBenchIP::parseIP(selector.netdev, bindAddr) )
        haveBindAddr = true;
    else
    if(!selector.netdev.empty() )
    {
        if(!NetBenchIP::resolveIfaceAddr(selector.netdev, family, bindAddr) )
            throw WorkerException("Interface \"" + selector.netdev + "\" has no " +
                ( (family == AF_INET) ? "IPv4 " : (family == AF_INET6) ? "IPv6 " : "IP ") +
                "address.");

        haveBindAddr = true;
    }
    else
    if(isServer)
        throw WorkerException("The tcp engine needs an address to listen on. Select interfaces "
            "via \"--" "netdevs" "\".");

    if(haveBindAddr)
        localAddr = NetBenchAddr::fromSockAddr( (const sockaddr*)&bindAddr,
            NetBenchIP::sockAddrLen( (const sockaddr*)&bindAddr) );
    else
    if(selector.peerAddr.isSockAddr() )
    {
        sockaddr_storage routeAddr;

        if(NetBenchIP::routeSourceAddr(selector.peerAddr.getSockAddr(),
            NetBenchIP::sockAddrLen(selector.peerAddr.getSockAddr() ), routeAddr) )
            localAddr = NetBenchAddr::fromSockAddr( (const sockaddr*)&routeAddr,
                NetBenchIP::sockAddrLen( (const sockaddr*)&routeAddr) );
    }

    if(localAddr.isSockAddr() )
        ifaceName = NetBenchIP::ifaceNameForAddr(localAddr.getSockAddr() );
}

void TcpEndpoint::openListener(unsigned short fixedPort, size_t backlog)
{
    const sockaddr* addr = (const sockaddr*)&bindAddr;

    listenFD = socket(addr->sa_family, SOCK_STREAM, 0);
    if(listenFD < 0)
        throw WorkerException(errnoStr("socket") );

    int one = 1;
    setsockopt(listenFD, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one) );

    if(addr->sa_family == AF_INET6)
    {
        setsockopt(listenFD, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one) );
        ( (sockaddr_in6*)&bindAddr)->sin6_port = htons(fixedPort);
    }
    else
        ( (sockaddr_in*)&bindAddr)->sin_port = htons(fixedPort);

    if(bind(listenFD, addr, NetBenchIP::sockAddrLen(addr) ) )
        throw WorkerException(errnoStr("bind to " + NetBenchIP::sockAddrToStr(addr) ) );

    if(listen(listenFD, std::max<int>(backlog, SOMAXCONN) ) )
        throw WorkerException(errnoStr("listen") );

    prepareSocket(listenFD);
    updateLocalAddr(listenFD);
}

/**
 * Non-blocking, close-on-exec, no delay, no SIGPIPE.
 */
void TcpEndpoint::prepareSocket(int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one) );

#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one) );
#endif
}

std::string TcpEndpoint::errnoStr(const std::string& op)
{
    return op + " failed: " + strerror(errno);
}

/**
 * Take localAddr from the socket's bound address (for the OS-chosen port or the connect source).
 */
void TcpEndpoint::updateLocalAddr(int fd)
{
    sockaddr_storage addr;
    socklen_t addrLen = sizeof(addr);

    if(getsockname(fd, (sockaddr*)&addr, &addrLen) )
        return;

    localAddr = NetBenchAddr::fromSockAddr( (const sockaddr*)&addr, addrLen);
    ifaceName = NetBenchIP::ifaceNameForAddr( (const sockaddr*)&addr);
}

/**
 * @return "tcp socket: ADDRESS (INTERFACE)" for log messages.
 */
std::string TcpEndpoint::getDescription() const
{
    return "tcp socket: " + (localAddr.isSockAddr() ? localAddr.toStr() : std::string("any") ) +
        " (" + (ifaceName.empty() ? std::string("-") : ifaceName) + ")";
}

void TcpEndpoint::putLocalInfo(bpt::ptree& outTree) const
{
    outTree.put(NETBENCH_JSON_ADDRFORMAT, localAddr.format);
    outTree.put(NETBENCH_JSON_ADDR, localAddr.toHex() );
}

/**
 * Client: put our address (if we bind to one) into the connect request.
 */
NetBenchConn* TcpEndpoint::prepareConnect(uint32_t rank, bpt::ptree& request)
{
    putLocalInfo(request);

    conns.push_back(std::make_unique<TcpConn>() );
    conns.back()->rank = rank;

    return conns.back().get();
}

/**
 * Client: start the non-blocking connect to the server's listen address. Failures surface from
 * readCompletions() like any other connection error.
 */
void TcpEndpoint::completeConnect(NetBenchConn* conn, const bpt::ptree& reply)
{
    TcpConn* tcpConn = static_cast<TcpConn*>(conn);

    tcpConn->peerAddr = NetBenchAddr::fromHex(reply.get<uint32_t>(NETBENCH_JSON_ADDRFORMAT),
        reply.get<std::string>(NETBENCH_JSON_ADDR) );

    if(!tcpConn->peerAddr.isSockAddr() )
        throw WorkerException("Server endpoint address is not a socket address.");

    const sockaddr* serverAddr = tcpConn->peerAddr.getSockAddr();

    if(haveBindAddr && (serverAddr->sa_family != bindAddr.ss_family) )
        throw WorkerException("Address family mismatch. Server endpoint: " +
            tcpConn->peerAddr.toStr() + "; own endpoint: " + localAddr.toStr() + ". Select "
            "interfaces of the same family via \"--" "netdevs" "\".");

    tcpConn->fd = socket(serverAddr->sa_family, SOCK_STREAM, 0);
    if(tcpConn->fd < 0)
        throw WorkerException(errnoStr("socket") );

    prepareSocket(tcpConn->fd);

    if(haveBindAddr && bind(tcpConn->fd, (const sockaddr*)&bindAddr,
        NetBenchIP::sockAddrLen( (const sockaddr*)&bindAddr) ) )
        throw WorkerException(errnoStr("bind to " + localAddr.ipToStr() ) );

    if(!connect(tcpConn->fd, serverAddr, NetBenchIP::sockAddrLen(serverAddr) ) )
    {
        tcpConn->state = TcpConn::Connected;
        updateLocalAddr(tcpConn->fd);
    }
    else
    if(errno == EINPROGRESS)
        tcpConn->state = TcpConn::Connecting;
    else
        failConn(tcpConn, errnoStr("connect to " + tcpConn->peerAddr.toStr() ) );
}

/**
 * Server: register the client of a connect request. Its socket arrives later through the
 * listener, carrying the client rank in the hello.
 */
NetBenchConn* TcpEndpoint::acceptPeer(const bpt::ptree& request, bpt::ptree& reply)
{
    const uint32_t clientRank = request.get<uint32_t>(NETBENCH_JSON_CLIENTRANK);

    putLocalInfo(reply);

    if(NetBenchConn* existing = findConn(clientRank) )
        return existing; // (a repeated connect of the same client just gets the same reply)

    const NetBenchAddr clientAddr = NetBenchAddr::fromHex(
        request.get<uint32_t>(NETBENCH_JSON_ADDRFORMAT),
        request.get<std::string>(NETBENCH_JSON_ADDR) );

    if(clientAddr.isSockAddr() && (clientAddr.getFamily() != localAddr.getFamily() ) )
        throw ProgException("Address family mismatch. Client: " + clientAddr.toStr() + "; "
            "Server: " + localAddr.toStr() + ". Select interfaces of the same family via "
            "\"--" "netdevs" "\".");

    auto conn = std::make_unique<TcpConn>();
    conn->rank = clientRank;
    conn->peerAddr = clientAddr;

    conns.push_back(std::move(conn) );

    return conns.back().get();
}

/**
 * Queue a receive. Receives of a connection complete in posting order as the bytes arrive.
 */
void TcpEndpoint::postRecv(NetBenchConn* conn, void* buf, size_t len, NetBenchOpCtx* ctx)
{
    TcpConn* tcpConn = static_cast<TcpConn*>(conn);

    ctx->conn = conn;

    if(!len)
    {
        ready.push_back( { ctx, true, 0 } );
        return;
    }

    ctx->posted = true;
    tcpConn->recvQ.push_back( { ctx, (char*)buf, len, 0 } );
}

/**
 * Queue a send and write as much of it as the socket takes right away. A client's first send on
 * a connection is the hello/ping, whose completion waits for the server's echo.
 */
void TcpEndpoint::postSend(NetBenchConn* conn, const void* buf, size_t len, NetBenchOpCtx* ctx)
{
    TcpConn* tcpConn = static_cast<TcpConn*>(conn);

    ctx->conn = conn;

    if(!len)
    {
        ready.push_back( { ctx, false, 0 } );
        return;
    }

    if(!isServer && !tcpConn->helloCtx && tcpConn->sendQ.empty() && !tcpConn->helloDone)
    { // first send of this connection: the hello carries our rank, the ping ctx waits for the echo
        const uint32_t magic = htonl(NETBENCH_TCP_HELLO_MAGIC);
        const uint32_t rank = htonl(tcpConn->rank);

        memcpy(tcpConn->helloBuf, &magic, sizeof(magic) );
        memcpy(tcpConn->helloBuf + sizeof(magic), &rank, sizeof(rank) );

        tcpConn->helloCtx = ctx;
        tcpConn->sendQ.push_back( { NULL, tcpConn->helloBuf, NETBENCH_PING_LEN, 0 } );
    }
    else
        tcpConn->sendQ.push_back( { ctx, (char*)buf, len, 0 } );

    if(tcpConn->state == TcpConn::Connected)
        writeConn(tcpConn);
}

/**
 * Do the socket I/O that is possible right now (see progress()) and return the completions.
 *
 * @throw WorkerException once for each failed connection.
 */
size_t TcpEndpoint::readCompletions(NetBenchCompletion* entries, size_t maxEntries)
{
    progress();

    for(const auto& conn : conns)
    {
        TcpConn* tcpConn = static_cast<TcpConn*>(conn.get() );

        if(!tcpConn->errorStr.empty() )
        {
            const std::string errorStr = tcpConn->errorStr;
            tcpConn->errorStr.clear();
            throw WorkerException(errorStr);
        }
    }

    const size_t numEntries = std::min(maxEntries, ready.size() );

    for(size_t i = 0; i < numEntries; i++)
    {
        entries[i] = ready.front();
        ready.pop_front();
    }

    return numEntries;
}

/**
 * Poll the sockets without blocking and do the I/O on the ready ones: accept new connections,
 * read hellos, read into the posted receives, write the queued sends.
 */
void TcpEndpoint::progress()
{
    buildPollFDs();

    if(pollFDs.empty() || (poll(pollFDs.data(), pollFDs.size(), 0) <= 0) )
        return;

    for(size_t i = 0; i < pollFDs.size(); i++)
    {
        if(!pollFDs[i].revents)
            continue;

        if(pollTargets[i].conn)
            handleConnEvents(pollTargets[i].conn, pollFDs[i].revents);
        else
        if(pollFDs[i].fd == listenFD)
            acceptConns();
        else
            readHello(pendingHellos[pollTargets[i].pendingIdx] );
    }

    // drop the pending hellos that got attached or closed
    pendingHellos.erase(std::remove_if(pendingHellos.begin(), pendingHellos.end(),
        [](const PendingHello& pending) { return pending.fd < 0; } ), pendingHellos.end() );
}

/**
 * The sockets to wait for: the listener and pending hellos for reading, connections for what they
 * have queued.
 */
void TcpEndpoint::buildPollFDs()
{
    pollFDs.clear();
    pollTargets.clear();

    if(listenFD >= 0)
    {
        pollFDs.push_back( { listenFD, POLLIN, 0 } );
        pollTargets.push_back( { NULL, 0 } );
    }

    for(size_t i = 0; i < pendingHellos.size(); i++)
    {
        pollFDs.push_back( { pendingHellos[i].fd, POLLIN, 0 } );
        pollTargets.push_back( { NULL, i } );
    }

    for(const auto& conn : conns)
    {
        TcpConn* tcpConn = static_cast<TcpConn*>(conn.get() );
        short events = 0;

        if(tcpConn->fd < 0)
            continue;

        if(tcpConn->state == TcpConn::Connecting)
            events = POLLOUT;
        else
        {
            if(!tcpConn->recvQ.empty() || tcpConn->helloCtx)
                events |= POLLIN;

            if(!tcpConn->sendQ.empty() )
                events |= POLLOUT;
        }

        if(!events)
            continue;

        pollFDs.push_back( { tcpConn->fd, events, 0 } );
        pollTargets.push_back( { tcpConn, 0 } );
    }
}

bool TcpEndpoint::addWaitFDs(std::vector<struct pollfd>& outFDs)
{
    buildPollFDs();

    outFDs.insert(outFDs.end(), pollFDs.begin(), pollFDs.end() );

    return true;
}

/**
 * Close the listener: all streams are connected when the phase starts.
 */
void TcpEndpoint::beginPhase()
{
    if(listenFD >= 0)
        close(listenFD);

    listenFD = -1;

    for(PendingHello& pending : pendingHellos)
        if(pending.fd >= 0)
            close(pending.fd);

    pendingHellos.clear();
}

void TcpEndpoint::acceptConns()
{
    for( ; ; )
    {
        sockaddr_storage peer;
        socklen_t peerLen = sizeof(peer);

        int fd = accept(listenFD, (sockaddr*)&peer, &peerLen);
        if(fd < 0)
        {
            if(errno == EINTR)
                continue;

            if( (errno != EAGAIN) && (errno != EWOULDBLOCK) )
                LOGGER(Log_DEBUG, "Netbench tcp accept failed: " << strerror(errno) << std::endl);

            return;
        }

        prepareSocket(fd);

        PendingHello pending = { fd, {}, 0, NetBenchAddr::fromSockAddr( (sockaddr*)&peer, peerLen) };
        pendingHellos.push_back(pending);
    }
}

/**
 * Read the hello of an accepted socket and attach the socket to the stream named in it. The
 * hello is the client's ping, so it completes the ping receive and gets echoed.
 */
void TcpEndpoint::readHello(PendingHello& pending)
{
    while(pending.done < NETBENCH_PING_LEN)
    {
        ssize_t ret = recv(pending.fd, pending.hello + pending.done,
            NETBENCH_PING_LEN - pending.done, 0);

        if(ret > 0)
        {
            pending.done += ret;
            continue;
        }

        if( (ret < 0) && (errno == EINTR) )
            continue;

        if( (ret < 0) && ( (errno == EAGAIN) || (errno == EWOULDBLOCK) ) )
            return; // (rest of the hello comes later)

        LOGGER(Log_DEBUG, "Netbench tcp connection from " << pending.peerAddr.toStr() <<
            " closed before its hello: " << (ret ? strerror(errno) : "closed by peer") <<
            std::endl);
        close(pending.fd);
        pending.fd = -1;
        return;
    }

    uint32_t magic, rank;
    memcpy(&magic, pending.hello, sizeof(magic) );
    memcpy(&rank, pending.hello + sizeof(magic), sizeof(rank) );

    TcpConn* conn = static_cast<TcpConn*>(findConn(ntohl(rank) ) );

    if( (ntohl(magic) != NETBENCH_TCP_HELLO_MAGIC) || !conn ||
        (conn->state != TcpConn::Unattached) )
    {
        LOGGER(Log_DEBUG, "Ignoring netbench tcp connection from " << pending.peerAddr.toStr() <<
            ": " << ( (ntohl(magic) != NETBENCH_TCP_HELLO_MAGIC) ? "not a netbench hello" :
            !conn ? "unknown stream" : "stream already connected") << std::endl);
        close(pending.fd);
        pending.fd = -1;
        return;
    }

    conn->fd = pending.fd;
    conn->state = TcpConn::Connected;
    conn->peerAddr = pending.peerAddr;
    pending.fd = -1;

    // the hello is the ping, so it belongs into the first posted receive
    if(conn->recvQ.empty() || (conn->recvQ.front().len != NETBENCH_PING_LEN) )
    {
        failConn(conn, "Protocol error: no ping receive posted for stream " +
            std::to_string(conn->rank) );
        return;
    }

    Op& pingOp = conn->recvQ.front();
    memcpy(pingOp.buf, pending.hello, NETBENCH_PING_LEN);
    pingOp.ctx->posted = false;
    ready.push_back( { pingOp.ctx, true, NETBENCH_PING_LEN } );
    conn->recvQ.pop_front();

    // echo it before any data goes out
    memcpy(conn->helloBuf, pending.hello, NETBENCH_PING_LEN);
    conn->sendQ.push_front( { NULL, conn->helloBuf, NETBENCH_PING_LEN, 0 } );
    writeConn(conn);
}

void TcpEndpoint::handleConnEvents(TcpConn* conn, short revents)
{
    if(conn->state == TcpConn::Connecting)
    {
        int err = 0;
        socklen_t errLen = sizeof(err);

        if(getsockopt(conn->fd, SOL_SOCKET, SO_ERROR, &err, &errLen) )
            err = errno;

        if(err)
        {
            failConn(conn, "connect to " + conn->peerAddr.toStr() + " failed: " + strerror(err) );
            return;
        }

        conn->state = TcpConn::Connected;
        updateLocalAddr(conn->fd);
    }

    if(conn->state != TcpConn::Connected)
        return;

    if(revents & (POLLIN | POLLHUP | POLLERR) )
        readConn(conn);

    if( (conn->state == TcpConn::Connected) && (revents & POLLOUT) )
        writeConn(conn);
}

/**
 * Read what the socket has into the echo buffer and the posted receives, in order.
 */
void TcpEndpoint::readConn(TcpConn* conn)
{
    for( ; ; )
    {
        char* buf;
        size_t len;

        if(conn->helloCtx)
        { // client: the echo comes first
            buf = conn->helloBuf + conn->helloDone;
            len = NETBENCH_PING_LEN - conn->helloDone;
        }
        else
        if(!conn->recvQ.empty() )
        {
            Op& op = conn->recvQ.front();
            buf = op.buf + op.done;
            len = op.len - op.done;
        }
        else
            return;

        ssize_t ret = recv(conn->fd, buf, len, 0);

        if(ret < 0)
        {
            if(errno == EINTR)
                continue;

            if( (errno == EAGAIN) || (errno == EWOULDBLOCK) )
                return;

            failConn(conn, "receive from " + conn->peerAddr.toStr() + " failed: " +
                strerror(errno) );
            return;
        }

        if(!ret)
        {
            failConn(conn, "connection closed by peer " + conn->peerAddr.toStr() );
            return;
        }

        if(conn->helloCtx)
        {
            conn->helloDone += ret;

            if(conn->helloDone == NETBENCH_PING_LEN)
            {
                ready.push_back( { conn->helloCtx, false, NETBENCH_PING_LEN } );
                conn->helloCtx = NULL;
            }

            continue;
        }

        Op& op = conn->recvQ.front();
        op.done += ret;

        if(op.done == op.len)
        {
            op.ctx->posted = false;
            ready.push_back( { op.ctx, true, op.len } );
            conn->recvQ.pop_front();
        }
    }
}

/**
 * Write the queued sends as far as the socket takes them.
 */
void TcpEndpoint::writeConn(TcpConn* conn)
{
    while(!conn->sendQ.empty() )
    {
        Op& op = conn->sendQ.front();

        ssize_t ret = send(conn->fd, op.buf + op.done, op.len - op.done, MSG_NOSIGNAL);

        if(ret < 0)
        {
            if(errno == EINTR)
                continue;

            if( (errno == EAGAIN) || (errno == EWOULDBLOCK) )
                return;

            failConn(conn, "send to " + conn->peerAddr.toStr() + " failed: " + strerror(errno) );
            return;
        }

        op.done += ret;

        if(op.done == op.len)
        {
            if(op.ctx)
                ready.push_back( { op.ctx, false, op.len } );

            conn->sendQ.pop_front();
        }
    }
}

/**
 * Close a failed connection and remember the error for the next readCompletions().
 */
void TcpEndpoint::failConn(TcpConn* conn, const std::string& errorStr)
{
    if(conn->fd >= 0)
        close(conn->fd);

    conn->fd = -1;
    conn->state = TcpConn::Failed;
    conn->errorStr = "Stream " + std::to_string(conn->rank) + ": " + errorStr;

    LOGGER(Log_DEBUG, "Netbench tcp " << conn->errorStr << std::endl);
}
