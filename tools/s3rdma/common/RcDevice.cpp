// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifdef RC_SUPPORT

#include "RcDevice.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <thread>

#include "Log.h"

namespace
{
    const int CQ_DEPTH = 16; // completions per session queue
    const int MAX_WORK_REQUESTS = 8; // one transfer per session

    // IPv4-mapped IPv6 GID ("::ffff:a.b.c.d") for the RoCE GID table lookup
    bool ipv4ToGid(const std::string& ipAddr, ibv_gid& outGid)
    {
        in_addr addr;
        if(inet_pton(AF_INET, ipAddr.c_str(), &addr) != 1)
            return false;

        memset(&outGid, 0, sizeof(outGid) );
        outGid.raw[10] = 0xff;
        outGid.raw[11] = 0xff;
        memcpy(outGid.raw + 12, &addr, 4);

        return true;
    }
}

/**
 * Opens the RDMA device that owns the given IPv4 address.
 *
 * @param gidIndex GID table index to use as source for all connections, -1 to choose per peer.
 * @throw std::runtime_error if no device owns the address or the device cannot be set up.
 */
RcDevice::RcDevice(const std::string& ipAddr, int gidIndex) : gidIndexFixed(gidIndex >= 0)
{
    if(!openDeviceForAddr(ipAddr, gidIndex) )
        throw std::runtime_error("No RDMA device with a RoCE GID for address \"" + ipAddr +
            "\" found.");

    pd = ibv_alloc_pd(context);
    if(!pd)
        throw std::runtime_error("ibv_alloc_pd failed: " + std::string(strerror(errno) ) );

    ibv_port_attr portAttr;
    if(!ibv_query_port(context, portNum, &portAttr) && portAttr.active_mtu >= IBV_MTU_512)
        pathMtu = portAttr.active_mtu;

    // the RC transport needs no more than a queue pair, so check that here already
    Queue probe = createQueue();
    destroyQueue(probe);

    Log::info("RDMA: RC device ready: " + getDescription() );
}

RcDevice::~RcDevice()
{
    if(pd)
        ibv_dealloc_pd(pd);

    if(context)
        ibv_close_device(context);
}

std::string RcDevice::gidToStr(const ibv_gid& gid)
{
    char gidStr[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, gid.raw, gidStr, sizeof(gidStr) );
    return gidStr;
}

std::string RcDevice::getDescription() const
{
    char gidStr[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, getLocalGid().raw, gidStr, sizeof(gidStr) );

    return std::string(ibv_get_device_name(context->device) ) + " port " +
        std::to_string(portNum) + ", GID index " + std::to_string(defaultGidIndex) + " (" +
        gidStr + ")";
}

/**
 * Find the device and port whose GID table contains the IPv4-mapped GID of ipAddr. Among the
 * entries with that GID, the RoCE v2 one becomes the default (that is what cuObject and hipObject
 * clients pick as well), unless the user gave a fixed index.
 */
bool RcDevice::openDeviceForAddr(const std::string& ipAddr, int wantedGidIndex)
{
    ibv_gid wantedGid;
    if(!ipv4ToGid(ipAddr, wantedGid) )
        return false;

    int numDevices = 0;
    ibv_device** devices = ibv_get_device_list(&numDevices);
    if(!devices)
        return false;

    for(int i = 0; i < numDevices && !context; i++)
    {
        ibv_context* candidate = ibv_open_device(devices[i] );
        if(!candidate)
            continue;

        ibv_device_attr devAttr;
        if(!ibv_query_device(candidate, &devAttr) )
            for(uint8_t port = 1; port <= devAttr.phys_port_cnt && !context; port++)
            {
                std::vector<GidEntry> table = readGidTable(candidate, port);
                int found = -1;

                for(size_t idx = 0; idx < table.size(); idx++)
                    if(table[idx].valid &&
                        !memcmp(table[idx].gid.raw, wantedGid.raw, sizeof(wantedGid.raw) ) &&
                        (found < 0 || table[idx].isRoceV2) )
                        found = idx;

                if(found < 0 || (wantedGidIndex >= (int)table.size() ) )
                    continue;

                context = candidate;
                portNum = port;
                gids = std::move(table);
                defaultGidIndex = (wantedGidIndex >= 0) ? wantedGidIndex : found;
            }

        if(!context)
            ibv_close_device(candidate);
    }

    ibv_free_device_list(devices);

    return context != nullptr;
}

std::vector<RcDevice::GidEntry> RcDevice::readGidTable(ibv_context* context, uint8_t port)
{
    std::vector<GidEntry> table;
    ibv_port_attr portAttr;

    if(ibv_query_port(context, port, &portAttr) )
        return table;

    table.resize(portAttr.gid_tbl_len);

    for(int i = 0; i < portAttr.gid_tbl_len; i++)
    {
        ibv_gid_entry entry;
        if(ibv_query_gid_ex(context, port, i, &entry, 0) )
            continue;

        table[i].gid = entry.gid;
        table[i].valid = true;
        table[i].isRoceV2 = (entry.gid_type == IBV_GID_TYPE_ROCE_V2);
        table[i].netdevIndex = entry.ndev_ifindex;
    }

    return table;
}

bool RcDevice::isIPv4Mapped(const ibv_gid& gid)
{
    static const uint8_t prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    return !memcmp(gid.raw, prefix, sizeof(prefix) );
}

bool RcDevice::isLinkLocal(const ibv_gid& gid)
{
    return gid.raw[0] == 0xfe && (gid.raw[1] & 0xc0) == 0x80;
}

/**
 * Pick the local GID to connect to the given peer GID with. RoCE needs both sides to use the same
 * IP family and RoCE version, and the peer's token only tells us its GID. So: an IPv4-mapped peer
 * GID gets our RoCE v2 GID of the configured address (what production clients use), a link-local
 * peer GID gets our first link-local GID of the same netdev, preferring RoCE v1 (what e.g.
 * hipObject's test client uses via GID index 0). The first one is the kernel's EUI-64 address of
 * the netdev; later link-local entries can be IPv6 privacy addresses, which RoCE v1 cannot route
 * to, and a port can also carry the GIDs of other netdevs (e.g. VLANs). Everything else gets the
 * default GID.
 */
int RcDevice::chooseGidIndex(const ibv_gid& peerGid) const
{
    if(gidIndexFixed || isIPv4Mapped(peerGid) )
        return defaultGidIndex;

    int chosen = -1;

    if(isLinkLocal(peerGid) )
        for(size_t i = 0; i < gids.size(); i++)
            if(gids[i].valid && isLinkLocal(gids[i].gid) &&
                (gids[i].netdevIndex == gids[defaultGidIndex].netdevIndex) &&
                (chosen < 0 || (!gids[i].isRoceV2 && gids[chosen].isRoceV2) ) )
                chosen = i;

    return (chosen >= 0) ? chosen : defaultGidIndex;
}

/**
 * Create an RC queue pair in INIT state, with its own completion queue.
 *
 * @throw std::runtime_error on failure.
 */
RcDevice::Queue RcDevice::createQueue()
{
    Queue queue;

    queue.cq = ibv_create_cq(context, CQ_DEPTH, nullptr, nullptr, 0);
    if(!queue.cq)
        throw std::runtime_error("ibv_create_cq failed: " + std::string(strerror(errno) ) );

    ibv_qp_init_attr initAttr = {};
    initAttr.send_cq = queue.cq;
    initAttr.recv_cq = queue.cq;
    initAttr.cap.max_send_wr = MAX_WORK_REQUESTS;
    initAttr.cap.max_recv_wr = MAX_WORK_REQUESTS;
    initAttr.cap.max_send_sge = 1;
    initAttr.cap.max_recv_sge = 1;
    initAttr.qp_type = IBV_QPT_RC;

    queue.qp = ibv_create_qp(pd, &initAttr);
    if(!queue.qp)
    {
        const std::string reason = strerror(errno);
        destroyQueue(queue);
        throw std::runtime_error("ibv_create_qp (RC) failed: " + reason);
    }

    ibv_qp_attr attr = {};
    attr.qp_state = IBV_QPS_INIT;
    attr.pkey_index = 0;
    attr.port_num = portNum;
    attr.qp_access_flags = IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;

    if(ibv_modify_qp(queue.qp, &attr,
        IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS) )
    {
        const std::string reason = strerror(errno);
        destroyQueue(queue);
        throw std::runtime_error("Transition of RC queue pair to INIT failed: " + reason);
    }

    return queue;
}

void RcDevice::destroyQueue(Queue& queue)
{
    if(queue.qp)
        ibv_destroy_qp(queue.qp);

    if(queue.cq)
        ibv_destroy_cq(queue.cq);

    queue = Queue();
}

/**
 * Connect the queue pair to the peer's queue pair (RTR, then RTS). The packet sequence numbers
 * were exchanged with the peer beforehand.
 */
bool RcDevice::connectQueue(Queue& queue, uint32_t peerQpNum, const ibv_gid& peerGid,
    int localGidIndex, uint32_t rqPsn, uint32_t sqPsn)
{
    ibv_qp_attr attr = {};
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = pathMtu;
    attr.dest_qp_num = peerQpNum;
    attr.rq_psn = rqPsn;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;
    attr.ah_attr.is_global = 1;
    attr.ah_attr.port_num = portNum;
    attr.ah_attr.grh.dgid = peerGid;
    attr.ah_attr.grh.sgid_index = localGidIndex;
    attr.ah_attr.grh.hop_limit = 64;

    if(ibv_modify_qp(queue.qp, &attr, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
        IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER) )
    {
        Log::error("Transition of RC queue pair to RTR failed: " + std::string(strerror(errno) ) );
        return false;
    }

    attr = {};
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.sq_psn = sqPsn;
    attr.max_rd_atomic = 1;

    if(ibv_modify_qp(queue.qp, &attr, IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
        IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC) )
    {
        Log::error("Transition of RC queue pair to RTS failed: " + std::string(strerror(errno) ) );
        return false;
    }

    return true;
}

ibv_mr* RcDevice::registerBuffer(void* buf, size_t len)
{
    return ibv_reg_mr(pd, buf, len,
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
}

void RcDevice::deregisterBuffer(ibv_mr* mr)
{
    if(mr)
        ibv_dereg_mr(mr);
}

/**
 * Post a receive work request, which consumes the peer's RDMA WRITE WITH IMMEDIATE.
 */
bool RcDevice::postRecv(Queue& queue, ibv_mr* mr, size_t len)
{
    ibv_sge sge = {};
    sge.addr = (uintptr_t)mr->addr;
    sge.length = len;
    sge.lkey = mr->lkey;

    ibv_recv_wr wr = {};
    wr.sg_list = &sge;
    wr.num_sge = 1;

    ibv_recv_wr* bad = nullptr;

    return !ibv_post_recv(queue.qp, &wr, &bad);
}

/**
 * RDMA WRITE the buffer into the peer's memory, with immData delivered to the peer's receive.
 */
bool RcDevice::postWriteWithImm(Queue& queue, ibv_mr* mr, size_t len, uint64_t remoteAddr,
    uint32_t remoteKey, uint32_t immData)
{
    ibv_sge sge = {};
    sge.addr = (uintptr_t)mr->addr;
    sge.length = len;
    sge.lkey = mr->lkey;

    ibv_send_wr wr = {};
    wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.imm_data = htonl(immData);
    wr.wr.rdma.remote_addr = remoteAddr;
    wr.wr.rdma.rkey = remoteKey;
    wr.sg_list = &sge;
    wr.num_sge = 1;

    ibv_send_wr* bad = nullptr;

    return !ibv_post_send(queue.qp, &wr, &bad);
}

/**
 * RDMA READ from the peer's memory into the buffer (the peer does not notice).
 */
bool RcDevice::postRead(Queue& queue, ibv_mr* mr, size_t len, uint64_t remoteAddr,
    uint32_t remoteKey)
{
    return postRdma(queue, IBV_WR_RDMA_READ, mr, len, remoteAddr, remoteKey);
}

/**
 * RDMA WRITE the buffer into the peer's memory (the peer does not notice).
 */
bool RcDevice::postWrite(Queue& queue, ibv_mr* mr, size_t len, uint64_t remoteAddr,
    uint32_t remoteKey)
{
    return postRdma(queue, IBV_WR_RDMA_WRITE, mr, len, remoteAddr, remoteKey);
}

bool RcDevice::postRdma(Queue& queue, ibv_wr_opcode opcode, ibv_mr* mr, size_t len,
    uint64_t remoteAddr, uint32_t remoteKey)
{
    ibv_sge sge = {};
    sge.addr = (uintptr_t)mr->addr;
    sge.length = len;
    sge.lkey = mr->lkey;

    ibv_send_wr wr = {};
    wr.opcode = opcode;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = remoteAddr;
    wr.wr.rdma.rkey = remoteKey;
    wr.sg_list = &sge;
    wr.num_sge = 1;

    ibv_send_wr* bad = nullptr;

    return !ibv_post_send(queue.qp, &wr, &bad);
}

/**
 * Wait for the next work completion of the queue.
 *
 * @return false on timeout.
 */
bool RcDevice::waitForCompletion(Queue& queue, std::chrono::steady_clock::time_point deadline,
    ibv_wc& outWc)
{
    while(std::chrono::steady_clock::now() < deadline)
    {
        const int numPolled = ibv_poll_cq(queue.cq, 1, &outWc);

        if(numPolled)
            return numPolled > 0;

        std::this_thread::sleep_for(std::chrono::microseconds(200) );
    }

    return false;
}

#endif // RC_SUPPORT
