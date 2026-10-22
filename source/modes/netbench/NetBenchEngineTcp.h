// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef MODES_NETBENCH_NETBENCHENGINETCP_H_
#define MODES_NETBENCH_NETBENCHENGINETCP_H_

#include <deque>
#include <string>
#include <vector>

#include "Common.h"
#include "modes/netbench/NetBenchEngine.h"
#include "modes/netbench/NetBenchProtocol.h"

/**
 * The tcp engine: one plain TCP socket per stream, non-blocking, with poll() as wait object. A
 * server endpoint listens on one socket until the phase starts, so it costs one file descriptor
 * per stream plus the listener. Messages have fixed sizes (both sides know the send and receive
 * size), so the byte stream needs no framing: posted receives are filled in posting order.
 *
 * The client's first send on a connection (the ping) doubles as hello that carries the client
 * rank, so the server can attach the accepted socket to the stream that was registered through
 * the HTTP connect handler. The server echoes the hello and the ping's send completion is
 * reported when the echo arrived, so that the connection is fully set up on both sides.
 */
class TcpEndpoint : public NetBenchEndpoint
{
    public:
        TcpEndpoint(bool isServer, const NetBenchSelector& selector, unsigned short fixedPort,
            size_t slabSize, size_t numConns);
        virtual ~TcpEndpoint();

        virtual NetBenchEngineType getEngine() const override { return NetBenchEngineType::Tcp; }
        virtual std::string getDescription() const override;
        virtual const NetBenchAddr& getAddr() const override { return localAddr; }

        virtual NetBenchConn* prepareConnect(uint32_t rank, bpt::ptree& request) override;
        virtual void completeConnect(NetBenchConn* conn, const bpt::ptree& reply) override;
        virtual NetBenchConn* acceptPeer(const bpt::ptree& request, bpt::ptree& reply) override;

        virtual void postRecv(NetBenchConn* conn, void* buf, size_t len,
            NetBenchOpCtx* ctx) override;
        virtual void postSend(NetBenchConn* conn, const void* buf, size_t len,
            NetBenchOpCtx* ctx) override;
        virtual size_t readCompletions(NetBenchCompletion* entries, size_t maxEntries) override;
        virtual void progress() override;
        virtual bool completionsPending() override { return !ready.empty(); }
        virtual bool addWaitFDs(std::vector<struct pollfd>& outFDs) override;
        virtual void beginPhase() override;

    private:
        struct Op
        {
            NetBenchOpCtx* ctx; // NULL for internal ops (hello, echo)
            char* buf;
            size_t len;
            size_t done;
        };

        struct TcpConn : public NetBenchConn
        {
            enum State { Unattached, Connecting, Connected, Failed };

            int fd{-1};
            State state{Unattached};
            std::deque<Op> sendQ;
            std::deque<Op> recvQ;
            char helloBuf[NETBENCH_PING_LEN]; // hello (client) or echo (server) bytes
            size_t helloDone{0}; // client: echo bytes received so far
            NetBenchOpCtx* helloCtx{NULL}; // client: the ping send waiting for the echo
            std::string errorStr; // set when the connection failed, reported once
        };

        struct PendingHello // an accepted socket whose hello is not complete yet
        {
            int fd;
            char hello[NETBENCH_PING_LEN];
            size_t done;
            NetBenchAddr peerAddr;
        };

        struct PollTarget
        {
            TcpConn* conn; // NULL for the listener and pending hellos
            size_t pendingIdx;
        };

        const bool isServer;
        int listenFD{-1};
        NetBenchAddr localAddr; // listen address (server) or bind/connect source address (client)
        sockaddr_storage bindAddr;
        bool haveBindAddr{false};
        std::string ifaceName; // of localAddr, for the description
        std::vector<PendingHello> pendingHellos;
        std::deque<NetBenchCompletion> ready;
        std::vector<struct pollfd> pollFDs; // (reused between calls)
        std::vector<PollTarget> pollTargets;

        void resolveBindAddr(const NetBenchSelector& selector);
        void openListener(unsigned short fixedPort, size_t backlog);
        void buildPollFDs();
        void acceptConns();
        void readHello(PendingHello& pending);
        void handleConnEvents(TcpConn* conn, short revents);
        void readConn(TcpConn* conn);
        void writeConn(TcpConn* conn);
        void failConn(TcpConn* conn, const std::string& errorStr);
        void updateLocalAddr(int fd);
        void putLocalInfo(bpt::ptree& outTree) const;

        static void prepareSocket(int fd);
        static std::string errnoStr(const std::string& op);
};

#endif /* MODES_NETBENCH_NETBENCHENGINETCP_H_ */
