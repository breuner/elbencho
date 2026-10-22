// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef MODES_NETBENCH_NETBENCHENGINERDMACM_H_
#define MODES_NETBENCH_NETBENCHENGINERDMACM_H_

#include <string>
#include <vector>

#include "Common.h"
#include "modes/netbench/NetBenchEngine.h"
#include "modes/netbench/NetBenchEngineIbverbsBase.h"

#ifdef RDMACM_SUPPORT

#include <rdma/rdma_cma.h>

/**
 * The rdmacm engine: the same reliable connections as the ibverbs engine (data path in
 * IbverbsEndpointBase), but set up by RDMA CM (librdmacm): a server listens on its interface
 * address and the connect reply over the service HTTP API carries just that address, the client
 * resolves and connects to it, and the queue pairs are created on the connection identifiers.
 * So the adapters negotiate ECE (e.g. the congestion control of Zero Touch RoCE), the path MTU
 * and the GIDs among themselves, as any rdma_cm application gets it.
 *
 * Connection events arrive on one event channel per endpoint, which is part of the wait set, so
 * waiting stays event-based. A server's queue pair for a stream only exists once the client's
 * connect request arrived, so receives posted before that are queued on the connection.
 */
class RdmaCmEndpoint : public IbverbsEndpointBase
{
    public:
        RdmaCmEndpoint(bool isServer, const NetBenchSelector& selector, unsigned short fixedPort,
            size_t slabSize, size_t numConns, size_t recvsPerConn, size_t sendsPerConn);
        virtual ~RdmaCmEndpoint();

        virtual NetBenchEngineType getEngine() const override
            { return NetBenchEngineType::Rdmacm; }
        virtual std::string getDescription() const override;
        virtual const NetBenchAddr& getAddr() const override { return localAddr; }

        virtual NetBenchConn* prepareConnect(uint32_t rank, bpt::ptree& request) override;
        virtual void completeConnect(NetBenchConn* conn, const bpt::ptree& reply) override;
        virtual NetBenchConn* acceptPeer(const bpt::ptree& request, bpt::ptree& reply) override;

        virtual void postRecv(NetBenchConn* conn, void* buf, size_t len,
            NetBenchOpCtx* ctx) override;
        virtual size_t readCompletions(NetBenchCompletion* entries, size_t maxEntries) override;
        virtual void progress() override;
        virtual bool addWaitFDs(std::vector<struct pollfd>& outFDs) override;

    private:
        struct QueuedRecv
        {
            void* buf;
            size_t len;
            NetBenchOpCtx* ctx;
        };

        struct CmConn : public QpConn
        {
            struct rdma_cm_id* id{NULL}; // server: set when the connect request arrives
            std::vector<QueuedRecv> queuedRecvs; // server: receives posted before the id exists
            std::string errorStr; // set by a failed connection, thrown by readCompletions()
        };

        bool isServer;
        struct rdma_event_channel* cmChannel{NULL};
        struct rdma_cm_id* listenId{NULL}; // server
        NetBenchAddr localAddr; // bound address with the CM port
        std::string deviceName;

        void createQp(CmConn* conn);
        void setRnrTimer(struct ibv_qp* qp);
        void handleEvent(struct rdma_cm_event* event);
        void waitForEvent(CmConn* conn, enum rdma_cm_event_type expected, const char* step);
        void failConn(CmConn* conn, const std::string& errorStr);
        void putLocalAddr(bpt::ptree& outTree) const;
        void closeAll();
};

#endif // RDMACM_SUPPORT

#endif /* MODES_NETBENCH_NETBENCHENGINERDMACM_H_ */
