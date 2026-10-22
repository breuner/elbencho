// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef MODES_NETBENCH_NETBENCHSERVER_H_
#define MODES_NETBENCH_NETBENCHSERVER_H_

#include <atomic>
#include <boost/property_tree/ptree.hpp>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Common.h"
#include "modes/netbench/NetBenchEngine.h"
#include "modes/netbench/NetBenchPlacement.h"

namespace bpt = boost::property_tree;

/**
 * One client stream as seen by the server: the connection and the progress counters. The receive
 * buffers belong to the endpoint, so a stream costs no memory registration.
 */
struct NetBenchStream
{
    uint32_t rank; // global client thread rank
    NetBenchConn* conn; // owned by the endpoint
    uint64_t numRecvDone{0}; // blocks received in the current phase
    uint64_t numSendDone{0}; // responses or stream blocks sent in the current phase
    uint64_t numSendInFlight{0};
    uint64_t numSendPending{0}; // responses waiting for a free send slot
    bool isDone{false};
    std::chrono::steady_clock::time_point startT;
    std::vector<NetBenchOpCtx> sendCtxVec; // one per possible send in flight (iodepth)
    std::vector<NetBenchOpCtx*> freeSendCtxVec;
};

/**
 * A server data endpoint of one worker thread on one interface, with its receive pool. Every
 * stream gets its receive slots plus a ping slot when it connects.
 */
struct NetBenchServerEndpoint
{
    std::unique_ptr<NetBenchEndpoint> ep;
    size_t workerIdx; // local worker thread that owns this endpoint
    size_t ifaceIdx; // index in the service's interfaces list
    size_t numStreams{0}; // streams the placement assigns to this endpoint
    size_t numStreamsAccepted{0};
    std::map<uint32_t, NetBenchStream> streams; // by client rank

    // layout of the slab
    char* recvBufs{NULL}; // numRecvSlots x recvSlotSize for client blocks
    size_t recvSlotSize{0};
    size_t numRecvSlots{0};
    size_t recvSlotsPerStream{0}; // iodepth, or "--netrecvpct" of it for send-only streams
    char* pingBufs{NULL}; // one NETBENCH_PING_LEN slot per stream
    char* respBuf{NULL}; // one static response buffer, shared by all streams

    std::vector<NetBenchOpCtx> recvCtxVec; // one per receive slot
    std::vector<NetBenchOpCtx> pingCtxVec; // one per ping slot
};

/**
 * Process-wide registry of the netbench server side of a service: the data endpoints that the
 * worker threads create in their preparation phase and the client streams that the HTTP service
 * registers through the connect handler. The worker threads take over their endpoints when the
 * benchmark phase starts, so the connect handler only touches endpoints of parked workers.
 *
 * Until then, a progress thread drives the endpoints, because the engines need progress calls to
 * accept the transport connections of the clients (tcp sockets, lazily connecting libfabric
 * providers).
 */
class NetBenchServer
{
    public:
        static NetBenchServer& get();

        void setRunConfig(const std::string& runID, size_t numThreads, size_t numIfaces,
            size_t ioDepth, const NetBenchStreamPlaceVec& streams);
        NetBenchServerEndpoint* addEndpoint(std::unique_ptr<NetBenchEndpoint> ep,
            size_t workerIdx, size_t ifaceIdx);
        bool connectClient(const bpt::ptree& request, bpt::ptree& reply);
        std::vector<NetBenchServerEndpoint*> beginPhase(size_t workerIdx);
        void reset();
        size_t countStreams(size_t workerIdx, size_t ifaceIdx);

    private:
        std::mutex mutex;
        std::mutex threadMutex; // for start/stop of the progress thread
        std::thread progressThread;
        std::atomic_bool stopProgress{false};
        std::string runID; // of the currently prepared run; empty if none
        size_t numThreads{0};
        size_t numIfaces{0};
        size_t ioDepth{0};
        std::map<uint32_t, NetBenchStreamPlace> streamTable; // by client rank (from coordinator)
        bool isPhaseStarted{false}; // no more connects once the workers poll their endpoints
        std::vector<std::unique_ptr<NetBenchServerEndpoint>> endpoints;

        NetBenchServer() = default;

        void startProgressThread();
        void stopProgressThread();
        void progressLoop();
        void drainEndpoints();
};

#endif /* MODES_NETBENCH_NETBENCHSERVER_H_ */
