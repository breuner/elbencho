// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef MODES_NETBENCH_NETBENCHENGINEIBVERBSBASE_H_
#define MODES_NETBENCH_NETBENCHENGINEIBVERBSBASE_H_

#include <string>
#include <vector>

#include "Common.h"
#include "modes/netbench/NetBenchEngine.h"

#ifdef IBVERBS_SUPPORT

#include <infiniband/verbs.h>

/**
 * The data path shared by the engines that run on libibverbs queue pairs: one completion queue
 * with a completion channel per endpoint (waiting is event-based through the channel's file
 * descriptor), the memory registration of the slab, posting of sends and receives and reading
 * of completions. How a queue pair gets connected is up to the derived engine: IbverbsEndpoint
 * exchanges the queue pair details over the service HTTP API, RdmaCmEndpoint uses rdma_cm.
 */
class IbverbsEndpointBase : public NetBenchEndpoint
{
    public:
        virtual void postRecv(NetBenchConn* conn, void* buf, size_t len,
            NetBenchOpCtx* ctx) override;
        virtual void postSend(NetBenchConn* conn, const void* buf, size_t len,
            NetBenchOpCtx* ctx) override;
        virtual size_t readCompletions(NetBenchCompletion* entries, size_t maxEntries) override;
        virtual void progress() override {}
        virtual bool completionsPending() override;
        virtual bool addWaitFDs(std::vector<struct pollfd>& outFDs) override;

    protected:
        struct QpConn : public NetBenchConn
        {
            struct ibv_qp* qp{NULL};
        };

        IbverbsEndpointBase(size_t slabSize, size_t recvsPerConn, size_t sendsPerConn,
            unsigned rnrTimer) :
            NetBenchEndpoint(slabSize), recvsPerConn(recvsPerConn), sendsPerConn(sendsPerConn),
            rnrTimer(rnrTimer) {}
        virtual ~IbverbsEndpointBase() {} // (derived classes destroy their queue pairs first)

        struct ibv_pd* pd{NULL}; // owned by the derived class or by librdmacm
        struct ibv_comp_channel* channel{NULL};
        struct ibv_cq* cq{NULL};
        struct ibv_mr* mr{NULL};
        size_t recvsPerConn;
        size_t sendsPerConn;
        unsigned rnrTimer; // receiver-not-ready retry timer code of the queue pairs
        bool armed{false}; // completion channel notification requested
        std::vector<struct ibv_wc> pending; // completions polled while arming (see below)
        size_t pendingPos{0};

        void openDataPath(struct ibv_context* context, struct ibv_pd* pd, size_t numConns);
        void closeDataPath();
        void drainEvents();

        static std::string ibvErrStr(const char* op, int err);
};

#endif // IBVERBS_SUPPORT

#endif /* MODES_NETBENCH_NETBENCHENGINEIBVERBSBASE_H_ */
