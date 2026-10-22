// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <algorithm>
#include <client_http.hpp>
#include <deque>
#include <sstream>
#include <thread>

#include "Logger.h"
#include "modes/netbench/NetBenchMode.h"
#include "modes/netbench/NetBenchProgArgs.h"
#include "modes/netbench/NetBenchProtocol.h"
#include "ProgArgs.h"
#include "workers/LocalWorker.h"
#include "workers/WorkerException.h"


NetBenchMode::NetBenchMode(LocalWorker& worker) :
    worker(worker), progArgs(worker.progArgs), netBenchArgs(worker.progArgs->getNetBenchArgs() ),
    workerRank(worker.workerRank)
{
}

namespace
{
    /**
     * The engine options of the selector. "--netprov rdma" means any RDMA provider.
     */
    void setEngineSelector(NetBenchSelector& selector, const NetBenchProgArgs& netBenchArgs)
    {
        if(netBenchArgs.getNetProv() == NETPROV_RDMA_ARG)
            selector.rdma = true;
        else
            selector.provName = netBenchArgs.getNetProv();

        selector.rnrTimer = netBenchArgs.getNetRnrTimer();
    }
} // anonymous namespace


/**
 * Prepare the netbench role of this worker: open the server endpoints or connect to the server.
 * Only service instances take part in netbench.
 */
void NetBenchMode::init()
{
    if(!progArgs->getRunAsService() || (progArgs->getBenchMode() != BenchMode_NETBENCH) )
        return;

    if(netBenchArgs.isServer() )
        initServer();
    else
        initClient();
}

void NetBenchMode::uninit()
{
    if(clientEP)
    { // (see NetBenchEndpoint::drain)
        const size_t recvSize = netBenchArgs.getRecvSize();

        NetBenchEndpoint::drain( { clientEP.get() }, [&]
        {
            for(NetBenchOpCtx& ctx : recvCtxVec)
                if(!ctx.posted)
                    clientEP->postRecv(serverConn, recvBufs + (ctx.slot * recvSize), recvSize,
                        &ctx);
        } );
    }

    clientEP.reset();
    serverConn = NULL;
    serverEndpoints.clear(); // (owned by NetBenchServer)
}

/**
 * Release the server endpoints. Called sequentially for all workers after the threads ended, and
 * possibly more than once.
 */
void NetBenchMode::uninitAfterPhaseDone()
{
    NetBenchServer::get().reset();
}

/**
 * Called by ProgArgs to reset singleton members.
 */
void NetBenchMode::resetSingletons()
{
    NetBenchServer::get().reset();
}

/**
 * Try to open a server endpoint for the given selector. Used for the default interface choice
 * (the addresses of our own service host), where not every address may have a transport.
 */
bool NetBenchMode::tryServerDefaultEndpoint(NetBenchSelector selector, unsigned short fixedPort,
    size_t slabSize, size_t numStreams, size_t recvsPerConn,
    std::unique_ptr<NetBenchEndpoint>& outEP)
{
    try
    {
        outEP = NetBenchEndpoint::create(netBenchArgs.getNetEngine(), true, selector, fixedPort,
            slabSize, numStreams, recvsPerConn, progArgs->getIODepth() );
        return true;
    }
    catch(WorkerException& e)
    {
        LOGGER(Log_DEBUG, "No netbench endpoint for interface " << selector.netdev << ": " <<
            e.what() << std::endl);
        return false;
    }
}

/**
 * Server: open one endpoint per interface with its receive pool and register them, so the HTTP
 * connect handler can add client streams while this worker waits for the phase start.
 */
void NetBenchMode::initServer()
{
    const StringVec& ifaces = netBenchArgs.getRunIfaces(); // (from the coordinator's placement)
    const size_t numIfaces = ifaces.size();
    const size_t localRank = workerRank - progArgs->getRankOffset();
    const size_t ioDepth = progArgs->getIODepth();
    const size_t sendSize = progArgs->getBlockSize();
    const size_t recvSize = netBenchArgs.getRecvSize();

    if(localRank >= netBenchArgs.getThreadsPerServer() )
        return; // (this thread stays idle, see "--tservers")

    if(ifaces.empty() )
        throw WorkerException("The run configuration has no interfaces for this server.");

    NetBenchServer& server = NetBenchServer::get();

    server.setRunConfig(netBenchArgs.getRunID(), netBenchArgs.getThreadsPerServer(), numIfaces,
        ioDepth, netBenchArgs.getRunStreams() );

    // our own service address: the address family for named interfaces
    const StringVec& serversVec = netBenchArgs.getServersVec();
    std::string ownHost, ownPort;

    if(netBenchArgs.getServiceIndex() < serversVec.size() )
        NetBenchIP::splitHostPort(serversVec[netBenchArgs.getServiceIndex() ], ownHost, ownPort);

    const std::vector<NetBenchAddr> ownAddrs = NetBenchIP::resolveHost(ownHost);

    for(size_t ifaceIdx = 0; ifaceIdx < numIfaces; ifaceIdx++)
    {
        /* a receive slot for every block that the streams of this endpoint can have in flight, so
            that no message has to be queued inside the transport (which also avoids crashes of
            older libfabric versions when an endpoint with queued messages is closed); send-only
            streams get "--netrecvpct" of that */
        const size_t numStreams = server.countStreams(localRank, ifaceIdx);
        const size_t slotsPerStream = sendSize ?
            netBenchArgs.getNumRecvSlots(ioDepth, recvSize != 0) : 0;
        const size_t numRecvSlots = numStreams * slotsPerStream;
        const size_t slabSize = (numRecvSlots * sendSize) + (numStreams * NETBENCH_PING_LEN) +
            recvSize;

        NetBenchSelector selector;
        setEngineSelector(selector, netBenchArgs);
        selector.maxMsgSize = std::max(sendSize, recvSize);

        const unsigned short fixedPort = netBenchArgs.getNetPort() ?
            (netBenchArgs.getNetPort() + (localRank * numIfaces) + ifaceIdx) : 0;

        std::unique_ptr<NetBenchEndpoint> ep;

        selector.netdev = ifaces[ifaceIdx];

        // a named interface may have addresses of both families, so take ours
        sockaddr_storage ipAddr;
        if(!NetBenchIP::parseIP(selector.netdev, ipAddr) && !ownAddrs.empty() )
            selector.family = ownAddrs[0].getFamily();

        if(!netBenchArgs.getNetDevsVec().empty() ||
            !tryServerDefaultEndpoint(selector, fixedPort, slabSize, numStreams, slotsPerStream + 1,
                ep) )
        {
            /* without "--netdevs" the interface is the one of our own service address, which may
                have no transport (e.g. no RDMA device), so any interface is the fallback then */
            if(netBenchArgs.getNetDevsVec().empty() )
                selector.netdev.clear();

            ep = NetBenchEndpoint::create(netBenchArgs.getNetEngine(), true, selector, fixedPort,
                slabSize, numStreams, slotsPerStream + 1, ioDepth);
        }

        NetBenchServerEndpoint* endpoint = server.addEndpoint(std::move(ep), localRank, ifaceIdx);

        // slab layout: receive pool, ping pool, response buffer

        char* slab = endpoint->ep->getSlab();

        endpoint->numStreams = numStreams;
        endpoint->recvBufs = slab;
        endpoint->recvSlotSize = sendSize;
        endpoint->numRecvSlots = numRecvSlots;
        endpoint->recvSlotsPerStream = slotsPerStream;
        endpoint->pingBufs = slab + (numRecvSlots * sendSize);
        endpoint->respBuf = endpoint->pingBufs + (numStreams * NETBENCH_PING_LEN);

        // (ctx vectors are sized once here, because posted contexts must not move; the receives
        // get posted when a stream connects)
        endpoint->recvCtxVec.resize(numRecvSlots);
        endpoint->pingCtxVec.resize(numStreams);

        for(size_t slot = 0; slot < numRecvSlots; slot++)
        {
            endpoint->recvCtxVec[slot].type = NetBenchMsg_DATA;
            endpoint->recvCtxVec[slot].slot = slot;
        }

        for(size_t slot = 0; slot < numStreams; slot++)
        {
            endpoint->pingCtxVec[slot].type = NetBenchMsg_PING;
            endpoint->pingCtxVec[slot].slot = slot;
        }

        serverEndpoints.push_back(endpoint);

        LOGGER(Log_VERBOSE, "Netbench server endpoint ready: " <<
            endpoint->ep->getDescription() << "; WorkerRank: " << workerRank << "; "
            "Streams: " << numStreams << std::endl);
    }
}

/**
 * Client: open the endpoint for this thread's stream and connect to the server.
 */
void NetBenchMode::initClient()
{
    const size_t localRank = workerRank - progArgs->getRankOffset();
    const unsigned numServers = netBenchArgs.getNumServers();
    const StringVec& serversVec = netBenchArgs.getServersVec();
    const NetBenchStreamPlaceVec& streams = netBenchArgs.getRunStreams(); // (by local rank)
    const size_t ioDepth = progArgs->getIODepth();
    const size_t sendSize = progArgs->getBlockSize();
    const size_t recvSize = netBenchArgs.getRecvSize();

    if(serversVec.size() < numServers)
        throw WorkerException("Received servers list is shorter than the number of servers.");

    if(localRank >= netBenchArgs.getThreadsPerClient() )
        return; // (this thread stays idle, see "--tclients")

    if( (localRank >= streams.size() ) || (streams[localRank].serverIdx >= serversVec.size() ) )
        throw WorkerException("The run configuration has no stream for this client thread.");

    // the coordinator's placement: our server and its interface, our own interface
    const NetBenchStreamPlace& place = streams[localRank];

    clientRank = (netBenchArgs.getServiceIndex() - numServers) *
        netBenchArgs.getThreadsPerClient() + localRank;
    serverHostPort = serversVec[place.serverIdx];
    clientNetDev = place.clientAddr;

    std::string serverHost, serverPort;
    NetBenchIP::splitHostPort(serverHostPort, serverHost, serverPort);

    NetBenchSelector selector;
    setEngineSelector(selector, netBenchArgs);
    selector.maxMsgSize = std::max(sendSize, recvSize);
    selector.netdev = clientNetDev;

    // the server's data interface as peer hint (its service address if that is no IP address)
    sockaddr_storage serverIfaceAddr;
    const std::vector<NetBenchAddr> serverAddrs =
        NetBenchIP::parseIP(place.serverIfaceAddr, serverIfaceAddr) ?
            std::vector<NetBenchAddr>( { NetBenchAddr::fromSockAddr(
                (const sockaddr*)&serverIfaceAddr,
                NetBenchIP::sockAddrLen( (const sockaddr*)&serverIfaceAddr) ) } ) :
            NetBenchIP::resolveHost(serverHost);

    if(!serverAddrs.empty() )
    {
        selector.peerAddr = serverAddrs[0];

        if(selector.netdev.empty() )
            selector.family = serverAddrs[0].getFamily();
    }

    // (a ping-pong stream needs one slot per round in flight, a receive-only stream gets more)
    const size_t numRecvSlots = recvSize ? netBenchArgs.getNumRecvSlots(ioDepth, sendSize != 0) : 0;
    const size_t slabSize = (ioDepth * sendSize) + (numRecvSlots * recvSize) + NETBENCH_PING_LEN;

    clientEP = NetBenchEndpoint::create(netBenchArgs.getNetEngine(), false, selector, 0, slabSize,
        1, numRecvSlots, ioDepth + 1);

    sendBufs = clientEP->getSlab();
    recvBufs = sendBufs + (ioDepth * sendSize);
    pingBuf = recvBufs + (numRecvSlots * recvSize);

    sendCtxVec.resize(ioDepth);
    recvCtxVec.resize(numRecvSlots);

    bpt::ptree request;
    request.put(NETBENCH_JSON_RUNID, netBenchArgs.getRunID() );
    request.put(NETBENCH_JSON_CLIENTRANK, clientRank);
    request.put(NETBENCH_JSON_ENGINE, netBenchEngineName(netBenchArgs.getNetEngine() ) );

    serverConn = clientEP->prepareConnect(clientRank, request);

    // responses (or stream blocks) may arrive as soon as the server starts its phase
    for(size_t slot = 0; slot < numRecvSlots; slot++)
    {
        NetBenchOpCtx& ctx = recvCtxVec[slot];
        ctx.type = NetBenchMsg_DATA;
        ctx.slot = slot;

        clientEP->postRecv(serverConn, recvBufs + (slot * recvSize), recvSize, &ctx);
    }

    connectToServer(request);
}

/**
 * Register our stream at the server through its HTTP API (retrying until the server is prepared
 * for this run), complete the connection from its reply and verify the data path with a ping.
 */
void NetBenchMode::connectToServer(const bpt::ptree& request)
{
    std::stringstream requestStream;
    bpt::write_json(requestStream, request, false);

    const std::string requestPath = HTTPCLIENTPATH_NETBENCHCONNECT "?"
        XFER_PREP_PROTCOLVERSION "=" HTTP_PROTOCOLVERSION "&"
        XFER_PREP_AUTHORIZATION "=" + progArgs->getSvcPasswordHash();

    const auto connectStartT = std::chrono::steady_clock::now();
    bpt::ptree reply;

    for( ; ; )
    {
        worker.checkInterruptionRequest();

        std::string retryReason;

        try
        {
            SimpleWeb::Client<SimpleWeb::HTTP> httpClient(serverHostPort);
            httpClient.config.timeout = 2;
            httpClient.config.timeout_connect = 2;

            auto response = httpClient.request("POST", requestPath, requestStream.str() );
            const std::string statusCode = response->status_code;

            if(!statusCode.rfind("200", 0) )
            {
                bpt::read_json(response->content, reply);
                break;
            }

            if(statusCode.rfind("503", 0) )
                throw WorkerException("Netbench connect rejected by server " + serverHostPort +
                    ": " + response->content.string() );

            retryReason = "server not prepared for this run yet";
        }
        catch(WorkerException& e)
        {
            throw;
        }
        catch(std::exception& e)
        {
            retryReason = e.what();
        }

        if(std::chrono::steady_clock::now() - connectStartT >
            std::chrono::seconds(NETBENCH_CONNECT_TIMEOUT_SEC) )
            throw WorkerException("Timed out connecting to netbench server " + serverHostPort +
                ". Last error: " + retryReason);

        LOGGER(Log_DEBUG, "Netbench connect to " << serverHostPort << " not possible yet, "
            "retrying. Reason: " << retryReason << std::endl);

        std::this_thread::sleep_for(std::chrono::milliseconds(NETBENCH_CONNECT_RETRY_MS) );
    }

    serverEPDescription = reply.get<std::string>(NETBENCH_JSON_ENDPOINT, "");

    const std::string serverEngine = reply.get<std::string>(NETBENCH_JSON_ENGINE, "");
    if(serverEngine != netBenchEngineName(netBenchArgs.getNetEngine() ) )
        throw WorkerException("Netbench server " + serverHostPort + " uses engine \"" +
            serverEngine + "\" instead of \"" + netBenchEngineName(netBenchArgs.getNetEngine() ) +
            "\".");

    // the ping's send completion proves that the data path to the server works
    try
    {
        clientEP->completeConnect(serverConn, reply);

        pingCtx.type = NetBenchMsg_PING;
        clientEP->postSend(serverConn, pingBuf, NETBENCH_PING_LEN, &pingCtx);

        const std::vector<NetBenchEndpoint*> endpoints = { clientEP.get() };
        const auto pingStartT = std::chrono::steady_clock::now();

        for( ; ; )
        {
            NetBenchCompletion entry;

            if(clientEP->readCompletions(&entry, 1) )
            {
                if(entry.ctx != &pingCtx)
                    throw WorkerException("Unexpected message before benchmark phase start.");

                break;
            }

            idleWait(endpoints, pingStartT, "connection check to server");
        }
    }
    catch(WorkerException& e)
    {
        throw WorkerException(std::string("Connection check to netbench server failed: ") +
            e.what() + ". Server: " + serverHostPort + "; server endpoint: " +
            serverEPDescription + "; own endpoint: " + clientEP->getDescription() +
            (clientNetDev.empty() ? "" : " (own interface " + clientNetDev + ")") +
            ". For RDMA, the route from the own address to the server endpoint address has to "
            "go through the own interface (see \"ip route get <server> from <own address>\").");
    }

    LOGGER(Log_VERBOSE, "Established connection to: " << serverHostPort << "; "
        "endpoint: " << serverEPDescription << "; "
        "via: " << clientEP->getDescription() << "; "
        "WorkerRank: " << workerRank << std::endl);
}

void NetBenchMode::doTransfer()
{
    if(!serverEndpoints.empty() )
        doTransferServer();
    else
    if(clientEP)
        doTransferClient();
    else
        worker.workerGotPhaseWork = false;
}

/**
 * Nothing was completed in the last poll: keep polling for a short while (to not add wakeup
 * latency to ping-pong rounds), then wait for events, checking interruption and the timeout.
 *
 * @param lastProgressT time of the last completion.
 * @param waitingFor for the timeout error message.
 */
void NetBenchMode::idleWait(const std::vector<NetBenchEndpoint*>& endpoints,
    std::chrono::steady_clock::time_point lastProgressT, const char* waitingFor)
{
    const auto now = std::chrono::steady_clock::now();

    if(now - lastProgressT < std::chrono::microseconds(NETBENCH_SPIN_USEC) )
        return;

    if(now - lastProgressT > std::chrono::seconds(NETBENCH_RECEIVE_TIMEOUT_SEC) )
        throw WorkerException(std::string("Timed out waiting for ") + waitingFor + ". "
            "Timeout: " + std::to_string(NETBENCH_RECEIVE_TIMEOUT_SEC) + "s; "
            "WorkerRank: " + std::to_string(workerRank) );

    worker.checkInterruptionRequest();

    NetBenchEndpoint::waitForCompletions(endpoints, NETBENCH_WAIT_TIMEOUT_MS);
}

void NetBenchMode::logStreamDone(uint32_t rank, uint64_t numBytes,
    std::chrono::steady_clock::time_point startT, const std::string& pathStr)
{
    const double elapsedSecs = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - startT).count();
    const double mibPerSec = elapsedSecs ? (numBytes / (1024.0 * 1024.0) / elapsedSecs) : 0;

    LOGGER(Log_DEBUG, "Stream " << rank << " done: " << numBytes << " bytes; " <<
        elapsedSecs << "s; " << mibPerSec << " MiB/s; " << pathStr << std::endl);
}

/**
 * Server: serve the streams of this worker's endpoints until all of them finished their rounds.
 */
void NetBenchMode::doTransferServer()
{
    const uint64_t numRounds = netBenchArgs.getNumRounds(*progArgs);
    const size_t sendSize = progArgs->getBlockSize();
    const size_t recvSize = netBenchArgs.getRecvSize();
    const size_t localRank = workerRank - progArgs->getRankOffset();

    const std::vector<NetBenchServerEndpoint*> endpoints = NetBenchServer::get().beginPhase(
        localRank);

    std::vector<NetBenchEndpoint*> eps;
    size_t numStreams = 0;

    for(NetBenchServerEndpoint* endpoint : endpoints)
    {
        eps.push_back(endpoint->ep.get() );
        numStreams += endpoint->streams.size();
    }

    if(!numStreams)
    {
        LOGGER(Log_DEBUG, "This server worker has no client streams. "
            "WorkerRank: " << workerRank << std::endl);

        worker.workerGotPhaseWork = false;
        return;
    }

    // send a response (or the next block of a receive-only stream) to a client
    auto postStreamSend = [&](NetBenchServerEndpoint* endpoint, NetBenchStream& stream)
    {
        if(stream.freeSendCtxVec.empty() )
        { // (send completions may lag behind the client's next block, so this is a normal case)
            stream.numSendPending++;
            return;
        }

        NetBenchOpCtx* ctx = stream.freeSendCtxVec.back();
        stream.freeSendCtxVec.pop_back();

        endpoint->ep->postSend(stream.conn, endpoint->respBuf, recvSize, ctx);

        stream.numSendInFlight++;
    };

    // (re-)arm the streams; receive-only client streams are driven by us, so start them here
    const auto phaseStartT = std::chrono::steady_clock::now();

    for(NetBenchServerEndpoint* endpoint : endpoints)
        for(auto& streamPair : endpoint->streams)
        {
            NetBenchStream& stream = streamPair.second;

            stream.numRecvDone = 0;
            stream.numSendDone = 0;
            stream.numSendInFlight = 0;
            stream.numSendPending = 0;
            stream.isDone = false;
            stream.startT = phaseStartT;
            stream.freeSendCtxVec.clear();

            for(NetBenchOpCtx& ctx : stream.sendCtxVec)
            {
                ctx.type = NetBenchMsg_DATA;
                stream.freeSendCtxVec.push_back(&ctx);
            }

            if(!sendSize)
                for(size_t i = 0; (i < stream.sendCtxVec.size() ) && (i < numRounds); i++)
                    postStreamSend(endpoint, stream);
        }

    size_t numStreamsDone = 0;
    auto lastProgressT = std::chrono::steady_clock::now();
    NetBenchCompletion entries[NETBENCH_CQ_BATCH_SIZE];

    while(numStreamsDone < numStreams)
    {
        worker.checkInterruptionRequest();

        bool gotCompletions = false;

        for(NetBenchServerEndpoint* endpoint : endpoints)
        {
            const size_t numEntries = endpoint->ep->readCompletions(entries,
                NETBENCH_CQ_BATCH_SIZE);

            for(size_t i = 0; i < numEntries; i++)
            {
                const NetBenchCompletion& entry = entries[i];
                NetBenchOpCtx* ctx = entry.ctx;

                if(entry.isRecv && (ctx->type == NetBenchMsg_PING) )
                    continue; // connection check of a client (one per stream)

                auto streamIter = endpoint->streams.find(ctx->conn->rank);
                if(streamIter == endpoint->streams.end() )
                {
                    LOGGER(Log_DEBUG, "Completion of unknown stream: " << ctx->conn->rank <<
                        std::endl);
                    continue;
                }

                NetBenchStream* stream = &streamIter->second;

                if(entry.isRecv)
                { // block from a client: count, repost the slot, answer
                    worker.atomicLiveOpsReadMix.numBytesDone += entry.len;

                    endpoint->ep->postRecv(ctx->conn,
                        endpoint->recvBufs + (ctx->slot * endpoint->recvSlotSize),
                        endpoint->recvSlotSize, ctx);

                    stream->numRecvDone++;
                    worker.atomicLiveOpsReadMix.numIOPSDone++;

                    if(recvSize)
                        postStreamSend(endpoint, *stream);
                }
                else
                { // our send completed
                    stream->numSendInFlight--;
                    stream->numSendDone++;
                    stream->freeSendCtxVec.push_back(ctx);
                    worker.atomicLiveOps.numBytesDone += recvSize;
                    worker.atomicLiveOps.numIOPSDone++;

                    if(stream->numSendPending)
                    {
                        stream->numSendPending--;
                        postStreamSend(endpoint, *stream);
                    }
                    else
                    if(!sendSize && ( (stream->numSendDone + stream->numSendInFlight) < numRounds) )
                        postStreamSend(endpoint, *stream);
                }

                const bool isDone = sendSize ?
                    ( (stream->numRecvDone >= numRounds) && !stream->numSendInFlight &&
                        !stream->numSendPending) :
                    (stream->numSendDone >= numRounds);

                if(isDone && !stream->isDone)
                {
                    stream->isDone = true;
                    numStreamsDone++;

                    logStreamDone(stream->rank,
                        (stream->numRecvDone * sendSize) + (stream->numSendDone * recvSize),
                        stream->startT, stream->conn->peerAddr.toStr() + " -> " +
                        endpoint->ep->getDescription() );
                }
            }

            if(numEntries)
                gotCompletions = true;
        }

        if(gotCompletions)
            lastProgressT = std::chrono::steady_clock::now();
        else
            idleWait(eps, lastProgressT, "data from clients");
    }
}

/**
 * Client: run this thread's stream for the configured number of rounds, with up to iodepth
 * rounds in flight.
 */
void NetBenchMode::doTransferClient()
{
    const uint64_t numRounds = netBenchArgs.getNumRounds(*progArgs);
    const size_t sendSize = progArgs->getBlockSize();
    const size_t recvSize = netBenchArgs.getRecvSize();
    const size_t ioDepth = progArgs->getIODepth();
    const std::vector<NetBenchEndpoint*> endpoints = { clientEP.get() };

    uint64_t numRoundsStarted = 0;
    uint64_t numRoundsDone = 0;
    std::vector<size_t> freeSlots; // send slots not in use
    std::deque<size_t> pendingSlots; // send slots waiting for their response, in send order
    std::vector<bool> slotSendDone(ioDepth);
    std::vector<bool> slotRespDone(ioDepth);

    for(size_t slot = 0; slot < ioDepth; slot++)
        freeSlots.push_back(slot);

    auto addLatency = [&](std::chrono::steady_clock::time_point startT)
    {
        worker.iopsLatHisto.addLatency(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - startT).count() );
    };

    const auto streamStartT = std::chrono::steady_clock::now();
    auto lastProgressT = streamStartT;
    NetBenchCompletion entries[NETBENCH_CQ_BATCH_SIZE];

    while(numRoundsDone < numRounds)
    {
        worker.checkInterruptionRequest();

        // start rounds up to iodepth
        while(sendSize && (numRoundsStarted < numRounds) && !freeSlots.empty() )
        {
            const size_t slot = freeSlots.back();
            freeSlots.pop_back();

            char* buf = sendBufs + (slot * sendSize);

            worker.rwRateLimiter(sendSize);
            worker.preWriteBlockModifier(buf, NULL, sendSize, numRoundsStarted * sendSize);

            NetBenchOpCtx& ctx = sendCtxVec[slot];
            ctx.type = NetBenchMsg_DATA;
            ctx.slot = slot;
            ctx.startT = std::chrono::steady_clock::now();
            slotSendDone[slot] = false;
            slotRespDone[slot] = false;

            clientEP->postSend(serverConn, buf, sendSize, &ctx);

            pendingSlots.push_back(slot);
            numRoundsStarted++;
        }

        const size_t numEntries = clientEP->readCompletions(entries, NETBENCH_CQ_BATCH_SIZE);

        if(!numEntries)
        {
            idleWait(endpoints, lastProgressT, "response from server");
            continue;
        }

        lastProgressT = std::chrono::steady_clock::now();

        for(size_t i = 0; i < numEntries; i++)
        {
            const NetBenchCompletion& entry = entries[i];
            NetBenchOpCtx* ctx = entry.ctx;

            if(!entry.isRecv)
            {
                worker.atomicLiveOps.numBytesDone += sendSize;
                worker.atomicLiveOps.numIOPSDone++;

                if(!recvSize)
                { // send-only stream: the round is complete with the send
                    addLatency(ctx->startT);
                    numRoundsDone++;
                    pendingSlots.pop_front();
                    freeSlots.push_back(ctx->slot);
                }
                else
                {
                    slotSendDone[ctx->slot] = true;
                    if(slotRespDone[ctx->slot] )
                        freeSlots.push_back(ctx->slot);
                }

                continue;
            }

            // response or block from the server: count and repost the receive slot

            worker.atomicLiveOpsReadMix.numBytesDone += entry.len;
            worker.atomicLiveOpsReadMix.numIOPSDone++;

            clientEP->postRecv(serverConn, recvBufs + (ctx->slot * recvSize), recvSize, ctx);

            if(sendSize)
            { // ping-pong: this is the response to the oldest pending round
                if(pendingSlots.empty() )
                    throw WorkerException("Received a response without a pending round. "
                        "Server: " + serverHostPort);

                const size_t slot = pendingSlots.front();
                pendingSlots.pop_front();

                addLatency(sendCtxVec[slot].startT);
                slotRespDone[slot] = true;
                if(slotSendDone[slot] )
                    freeSlots.push_back(slot);
            }

            numRoundsDone++;
        }
    }

    logStreamDone(clientRank, numRounds * (sendSize + recvSize), streamStartT,
        clientEP->getDescription() + " -> " + serverEPDescription);
}
