// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <thread>

#include "Logger.h"
#include "modes/netbench/NetBenchProtocol.h"
#include "modes/netbench/NetBenchServer.h"
#include "ProgException.h"
#include "workers/WorkerException.h"

#define NETBENCH_PROGRESS_WAIT_MS       100 // wait slice of the progress thread
#define NETBENCH_PROGRESS_IDLE_MS       1 // backoff while completions are pending or no endpoints


NetBenchServer& NetBenchServer::get()
{
    static NetBenchServer instance;
    return instance;
}

/**
 * Set the configuration of the prepared run. Called by every server worker in its preparation
 * phase (same values each time).
 */
void NetBenchServer::setRunConfig(const std::string& runID, size_t numThreads, size_t numIfaces,
    size_t ioDepth, const NetBenchStreamPlaceVec& streams)
{
    std::lock_guard<std::mutex> lock(mutex);

    if(this->runID != runID)
    { // (other workers may already be counting the streams of this run)
        streamTable.clear();

        for(const NetBenchStreamPlace& place : streams)
            streamTable[place.clientRank] = place;
    }

    this->runID = runID;
    this->numThreads = numThreads;
    this->numIfaces = numIfaces;
    this->ioDepth = ioDepth;
    this->isPhaseStarted = false;

    startProgressThread();
}

/**
 * Register a worker's endpoint. The registry owns it from here on.
 */
NetBenchServerEndpoint* NetBenchServer::addEndpoint(std::unique_ptr<NetBenchEndpoint> ep,
    size_t workerIdx, size_t ifaceIdx)
{
    std::lock_guard<std::mutex> lock(mutex);

    endpoints.push_back(std::make_unique<NetBenchServerEndpoint>() );

    NetBenchServerEndpoint* endpoint = endpoints.back().get();
    endpoint->ep = std::move(ep);
    endpoint->workerIdx = workerIdx;
    endpoint->ifaceIdx = ifaceIdx;

    return endpoint;
}

/**
 * Handle a client's connect request from the HTTP service: register the stream on the endpoint
 * that the coordinator's placement assigns to the client rank, post its receives and reply with
 * that endpoint's details.
 *
 * @return false if no run with the requested ID is prepared yet, so the client should retry.
 * @throw ProgException if the request cannot be served (reported to the client as error).
 */
bool NetBenchServer::connectClient(const bpt::ptree& request, bpt::ptree& reply)
{
    std::lock_guard<std::mutex> lock(mutex);

    const std::string requestRunID = request.get<std::string>(NETBENCH_JSON_RUNID);
    const uint32_t clientRank = request.get<uint32_t>(NETBENCH_JSON_CLIENTRANK);

    if(runID.empty() || (requestRunID != runID) ||
        (endpoints.size() != (numThreads * numIfaces) ) )
        return false; // not (yet) prepared for this run

    if(isPhaseStarted)
        throw ProgException("Benchmark phase already started, no more connects accepted.");

    const auto placeIter = streamTable.find(clientRank);
    if(placeIter == streamTable.end() )
        throw ProgException("Client rank " + std::to_string(clientRank) + " is not placed on "
            "this server.");

    const NetBenchStreamPlace& place = placeIter->second;
    NetBenchServerEndpoint* endpoint = NULL;

    for(const auto& candidate : endpoints)
        if( (candidate->workerIdx == place.serverWorkerIdx) &&
            (candidate->ifaceIdx == place.serverIfaceIdx) )
            endpoint = candidate.get();

    if(!endpoint)
        throw ProgException("No endpoint for client rank " + std::to_string(clientRank) );

    NetBenchEndpoint& ep = *endpoint->ep;

    const std::string clientEngine = request.get<std::string>(NETBENCH_JSON_ENGINE, "");

    if(clientEngine != netBenchEngineName(ep.getEngine() ) )
        throw ProgException("Client and server use different netbench engines. Client: " +
            (clientEngine.empty() ? std::string("(none)") : clientEngine) + "; Server: " +
            netBenchEngineName(ep.getEngine() ) + " (" + ep.getDescription() + "). The engine "
            "is selected on the coordinator via \"--" "netengine" "\" and all services have to "
            "support it.");

    NetBenchConn* conn = ep.acceptPeer(request, reply);

    if(!endpoint->streams.count(clientRank) )
    { // new stream (a repeated connect of the same client just gets the same reply again)
        if(endpoint->numStreamsAccepted >= endpoint->numStreams)
            throw ProgException("More client streams than expected for endpoint " +
                ep.getDescription() + ": " + std::to_string(endpoint->numStreams) );

        const size_t streamIdx = endpoint->numStreamsAccepted++;

        // the ping receive first, because the tcp engine fills receives in posting order
        ep.postRecv(conn, endpoint->pingBufs + (streamIdx * NETBENCH_PING_LEN), NETBENCH_PING_LEN,
            &endpoint->pingCtxVec[streamIdx] );

        for(size_t i = 0; i < endpoint->recvSlotsPerStream; i++)
        {
            NetBenchOpCtx& ctx = endpoint->recvCtxVec[(streamIdx * endpoint->recvSlotsPerStream) + i];

            ep.postRecv(conn, endpoint->recvBufs + (ctx.slot * endpoint->recvSlotSize),
                endpoint->recvSlotSize, &ctx);
        }

        NetBenchStream stream;
        stream.rank = clientRank;
        stream.conn = conn;
        stream.sendCtxVec.resize(ioDepth);

        endpoint->streams[clientRank] = std::move(stream);

        LOGGER(Log_VERBOSE, "Accepted stream from client rank " << clientRank << ": " <<
            conn->peerAddr.toStr() << " -> " << ep.getDescription() << std::endl);
    }

    reply.put(NETBENCH_JSON_ENGINE, netBenchEngineName(ep.getEngine() ) );
    reply.put(NETBENCH_JSON_ENDPOINT, ep.getDescription() );

    return true;
}

/**
 * Called by a server worker when the benchmark phase starts: no more connects from here on.
 *
 * @return the endpoints of this worker (owned by the registry).
 */
std::vector<NetBenchServerEndpoint*> NetBenchServer::beginPhase(size_t workerIdx)
{
    stopProgressThread(); // (the workers drive their endpoints from here on)

    std::lock_guard<std::mutex> lock(mutex);

    isPhaseStarted = true;

    std::vector<NetBenchServerEndpoint*> result;

    for(const auto& endpoint : endpoints)
        if(endpoint->workerIdx == workerIdx)
        {
            endpoint->ep->beginPhase();
            result.push_back(endpoint.get() );
        }

    return result;
}

/**
 * Close all endpoints and forget the run. Safe to call repeatedly and without a prepared run.
 */
void NetBenchServer::reset()
{
    stopProgressThread();

    std::lock_guard<std::mutex> lock(mutex);

    drainEndpoints();

    endpoints.clear();
    streamTable.clear();
    runID.clear();
    isPhaseStarted = false;
}

/**
 * Drain the endpoints before they get closed (see NetBenchEndpoint::drain), keeping the data
 * receive slots posted.
 */
void NetBenchServer::drainEndpoints()
{
    std::vector<NetBenchEndpoint*> eps;

    for(const auto& endpoint : endpoints)
        eps.push_back(endpoint->ep.get() );

    NetBenchEndpoint::drain(eps, [&]
    {
        for(const auto& endpoint : endpoints)
            for(NetBenchOpCtx& ctx : endpoint->recvCtxVec)
                if(!ctx.posted && ctx.conn)
                    endpoint->ep->postRecv(ctx.conn,
                        endpoint->recvBufs + (ctx.slot * endpoint->recvSlotSize),
                        endpoint->recvSlotSize, &ctx);
    } );
}

/**
 * @return number of client streams that the placement assigns to the given endpoint. Call after
 *      setRunConfig().
 */
size_t NetBenchServer::countStreams(size_t workerIdx, size_t ifaceIdx)
{
    std::lock_guard<std::mutex> lock(mutex);

    size_t numStreams = 0;

    for(const auto& placePair : streamTable)
        if( (placePair.second.serverWorkerIdx == workerIdx) &&
            (placePair.second.serverIfaceIdx == ifaceIdx) )
            numStreams++;

    return numStreams;
}

void NetBenchServer::startProgressThread()
{
    std::lock_guard<std::mutex> lock(threadMutex);

    if(progressThread.joinable() )
        return;

    stopProgress = false;
    progressThread = std::thread(&NetBenchServer::progressLoop, this);
}

/**
 * Stop and join the progress thread. No-op if it is not running.
 */
void NetBenchServer::stopProgressThread()
{
    std::lock_guard<std::mutex> lock(threadMutex);

    if(!progressThread.joinable() )
        return;

    stopProgress = true;
    progressThread.join();
}

/**
 * Drive progress on all registered endpoints until stopped: accept connections, read hellos,
 * let providers connect. Waits for events outside the lock, with a short backoff while
 * completions (the clients' pings) are pending, because those are left for the worker threads.
 */
void NetBenchServer::progressLoop()
{
    while(!stopProgress)
    {
        std::vector<struct pollfd> pollFDs;
        bool canWait = false;

        {
            std::lock_guard<std::mutex> lock(mutex);

            std::vector<NetBenchEndpoint*> eps;

            for(const auto& endpoint : endpoints)
                eps.push_back(endpoint->ep.get() );

            try
            {
                for(NetBenchEndpoint* ep : eps)
                    ep->progress();

                canWait = !eps.empty() && NetBenchEndpoint::prepareWait(eps, pollFDs);
            }
            catch(WorkerException& e)
            {
                LOGGER(Log_DEBUG, "Netbench progress error: " << e.what() << std::endl);
            }
        }

        if(canWait)
            poll(pollFDs.data(), pollFDs.size(), NETBENCH_PROGRESS_WAIT_MS);
        else
            std::this_thread::sleep_for(std::chrono::milliseconds(NETBENCH_PROGRESS_IDLE_MS) );
    }
}
