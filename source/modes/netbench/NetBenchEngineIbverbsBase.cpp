// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>

#include "Logger.h"
#include "modes/netbench/NetBenchEngineIbverbsBase.h"
#include "modes/netbench/NetBenchProtocol.h"
#include "workers/WorkerException.h"

#ifdef IBVERBS_SUPPORT

/**
 * Create the completion channel and queue on the device and register the slab.
 *
 * @param pd protection domain for the registration (and for the derived class' queue pairs).
 * @param numConns for the completion queue size.
 * @throw WorkerException on error.
 */
void IbverbsEndpointBase::openDataPath(struct ibv_context* context, struct ibv_pd* pd,
    size_t numConns)
{
    this->pd = pd;

    struct ibv_device_attr attr;
    if(ibv_query_device(context, &attr) )
        throw WorkerException(ibvErrStr("ibv_query_device", errno) );

    channel = ibv_create_comp_channel(context);
    if(!channel)
        throw WorkerException(ibvErrStr("ibv_create_comp_channel", errno) );

    fcntl(channel->fd, F_SETFL, fcntl(channel->fd, F_GETFL, 0) | O_NONBLOCK);

    const size_t numCQEs = std::min<size_t>( (numConns * (recvsPerConn + sendsPerConn) ) + 1,
        attr.max_cqe);

    cq = ibv_create_cq(context, numCQEs, NULL, channel, 0);
    if(!cq)
        throw WorkerException(ibvErrStr("ibv_create_cq", errno) );

    mr = ibv_reg_mr(pd, slab, slabSize, IBV_ACCESS_LOCAL_WRITE);
    if(!mr)
        throw WorkerException(ibvErrStr("ibv_reg_mr", errno) + ". Registering memory for RDMA "
            "needs a sufficient locked memory limit (\"ulimit -l\").");
}

/**
 * Destroy the completion queue (after acknowledging its events), the channel and the memory
 * registration. The queue pairs have to be gone already.
 */
void IbverbsEndpointBase::closeDataPath()
{
    if(cq)
    {
        drainEvents();
        ibv_destroy_cq(cq);
    }

    if(channel)
        ibv_destroy_comp_channel(channel);

    if(mr)
        ibv_dereg_mr(mr);

    cq = NULL; channel = NULL; mr = NULL;
}

std::string IbverbsEndpointBase::ibvErrStr(const char* op, int err)
{
    return std::string(op) + " failed: " + strerror(err) + " (" + std::to_string(err) + ")";
}

void IbverbsEndpointBase::postRecv(NetBenchConn* conn, void* buf, size_t len, NetBenchOpCtx* ctx)
{
    struct ibv_sge sge = {};
    sge.addr = (uintptr_t)buf;
    sge.length = len;
    sge.lkey = mr->lkey;

    struct ibv_recv_wr wr = {};
    struct ibv_recv_wr* badWr;
    wr.wr_id = (uintptr_t)ctx;
    wr.sg_list = &sge;
    wr.num_sge = len ? 1 : 0;

    ctx->conn = conn;

    int ret = ibv_post_recv(static_cast<QpConn*>(conn)->qp, &wr, &badWr);
    if(ret)
        throw WorkerException(ibvErrStr("ibv_post_recv", ret) );

    ctx->posted = true;
}

void IbverbsEndpointBase::postSend(NetBenchConn* conn, const void* buf, size_t len,
    NetBenchOpCtx* ctx)
{
    struct ibv_sge sge = {};
    sge.addr = (uintptr_t)buf;
    sge.length = len;
    sge.lkey = mr->lkey;

    struct ibv_send_wr wr = {};
    struct ibv_send_wr* badWr;
    wr.wr_id = (uintptr_t)ctx;
    wr.sg_list = &sge;
    wr.num_sge = len ? 1 : 0;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;

    ctx->conn = conn;

    int ret = ibv_post_send(static_cast<QpConn*>(conn)->qp, &wr, &badWr);
    if(ret)
        throw WorkerException(ibvErrStr("ibv_post_send", ret) );
}

/**
 * Consume and acknowledge the completion channel events. After that, a new notification has to
 * be requested before waiting again.
 */
void IbverbsEndpointBase::drainEvents()
{
    unsigned numEvents = 0;
    struct ibv_cq* eventCQ;
    void* eventCtx;

    while(!ibv_get_cq_event(channel, &eventCQ, &eventCtx) )
        numEvents++;

    if(numEvents)
    {
        ibv_ack_cq_events(cq, numEvents);
        armed = false;
    }
}

/**
 * Read available completions without blocking: first the ones polled while arming the channel
 * (see completionsPending), then the queue.
 *
 * @throw WorkerException on an error completion.
 */
size_t IbverbsEndpointBase::readCompletions(NetBenchCompletion* entries, size_t maxEntries)
{
    struct ibv_wc wcs[NETBENCH_CQ_BATCH_SIZE];
    size_t numEntries = 0;

    drainEvents();

    while( (pendingPos < pending.size() ) && (numEntries < maxEntries) )
        wcs[numEntries++] = pending[pendingPos++];

    if(numEntries < maxEntries)
    {
        const int ret = ibv_poll_cq(cq, std::min<size_t>(maxEntries, NETBENCH_CQ_BATCH_SIZE) -
            numEntries, wcs + numEntries);
        if(ret < 0)
            throw WorkerException("ibv_poll_cq failed");

        numEntries += ret;
    }

    for(size_t i = 0; i < numEntries; i++)
    {
        const struct ibv_wc& wc = wcs[i];
        NetBenchOpCtx* ctx = (NetBenchOpCtx*)(uintptr_t)wc.wr_id;

        if(wc.status != IBV_WC_SUCCESS)
        { // (the opcode is undefined on errors, so the posted flag tells sends and receives apart)
            const bool wasRecv = ctx && ctx->posted;

            if(wasRecv)
                ctx->posted = false;

            throw WorkerException(std::string("Completion error on ") +
                (wasRecv ? "receive" : "send") + ": " + ibv_wc_status_str(wc.status) +
                " (" + std::to_string(wc.status) + "); endpoint: " + getDescription() + "; "
                "stream: " + ( (ctx && ctx->conn) ? std::to_string(ctx->conn->rank) : "-") );
        }

        const bool isRecv = wc.opcode & IBV_WC_RECV;

        if(isRecv)
            ctx->posted = false;

        entries[i] = { ctx, isRecv, isRecv ? wc.byte_len : 0 };
    }

    return numEntries;
}

/**
 * Request a notification for the next completion and check for completions that arrived before
 * that, which would otherwise be missed while waiting.
 *
 * @return true if completions are available, so the caller must not block.
 */
bool IbverbsEndpointBase::completionsPending()
{
    if(pendingPos < pending.size() )
        return true;

    if(!armed)
    {
        if(ibv_req_notify_cq(cq, 0) )
            throw WorkerException(ibvErrStr("ibv_req_notify_cq", errno) );

        armed = true;
    }

    pending.resize(NETBENCH_CQ_BATCH_SIZE);
    pendingPos = 0;

    const int ret = ibv_poll_cq(cq, NETBENCH_CQ_BATCH_SIZE, pending.data() );
    pending.resize(std::max(ret, 0) );

    return ret > 0;
}

bool IbverbsEndpointBase::addWaitFDs(std::vector<struct pollfd>& outFDs)
{
    outFDs.push_back( { channel->fd, POLLIN, 0 } );

    return true;
}

#endif // IBVERBS_SUPPORT
