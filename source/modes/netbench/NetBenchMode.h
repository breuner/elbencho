// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef MODES_NETBENCH_NETBENCHMODE_H_
#define MODES_NETBENCH_NETBENCHMODE_H_

#include <chrono>
#include <memory>
#include <functional>
#include <string>
#include <vector>

#include "Common.h"
#include "modes/netbench/NetBenchEngine.h"
#include "modes/netbench/NetBenchServer.h"

class LocalWorker;
class NetBenchProgArgs;
class ProgArgs;

/**
 * The netbench mode of a worker thread. A server worker owns one data endpoint per interface
 * (registered in NetBenchServer, so the HTTP connect handler can add client streams to it), a
 * client worker owns one endpoint and runs one stream to its server. Works on the worker's stats.
 * The transport is one of the engines behind NetBenchEndpoint.
 */
class NetBenchMode
{
    public:
        explicit NetBenchMode(LocalWorker& worker);

        void init();
        void uninit();
        void uninitAfterPhaseDone();
        void doTransfer();

        static void resetSingletons();

    private:
        LocalWorker& worker;
        ProgArgs* progArgs; // shortcut for worker member
        const NetBenchProgArgs& netBenchArgs; // shortcut for progArgs member
        size_t workerRank; // shortcut for worker member

        std::vector<NetBenchServerEndpoint*> serverEndpoints; // server: owned by NetBenchServer

        std::unique_ptr<NetBenchEndpoint> clientEP; // client: own endpoint
        NetBenchConn* serverConn{NULL}; // client: the connection to the server (owned by clientEP)
        std::string serverHostPort; // client: service address of the server
        std::string serverEPDescription; // client: for log messages
        std::string clientNetDev; // client: own interface address from the placement
        uint32_t clientRank{0}; // client: global client thread rank
        char* sendBufs{NULL}; // client: iodepth x sendSize
        char* recvBufs{NULL}; // client: 2 x iodepth x recvSize
        char* pingBuf{NULL};
        std::vector<NetBenchOpCtx> sendCtxVec;
        std::vector<NetBenchOpCtx> recvCtxVec;
        NetBenchOpCtx pingCtx;

        void initServer();
        void initClient();
        void connectToServer(const bpt::ptree& request);
        void doTransferServer();
        void doTransferClient();
        void idleWait(const std::vector<NetBenchEndpoint*>& endpoints,
            std::chrono::steady_clock::time_point lastProgressT, const char* waitingFor,
            const std::function<std::string()>& detailFn);
        void logStreamDone(uint32_t rank, uint64_t numBytes,
            std::chrono::steady_clock::time_point startT, const std::string& pathStr);
        bool tryServerDefaultEndpoint(NetBenchSelector selector, unsigned short fixedPort,
            size_t slabSize, size_t numStreams, size_t recvsPerConn,
            std::unique_ptr<NetBenchEndpoint>& outEP);
};

#endif /* MODES_NETBENCH_NETBENCHMODE_H_ */
