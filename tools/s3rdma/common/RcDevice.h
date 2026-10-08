// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef RCDEVICE_H_
#define RCDEVICE_H_

#ifdef RC_SUPPORT

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <infiniband/verbs.h>

/**
 * A plain libibverbs RDMA device with Reliable Connection (RC) queue pairs. Each connection to a
 * peer gets its own queue pair; the peer's queue pair number and GID arrive via the protocol's
 * tokens and headers. Used by the server side of the hipObject protocol and by the RC transport
 * of the cuObject-style protocol (server and client shim).
 */
class RcDevice
{
    public:
        struct Queue
        {
            ibv_cq* cq = nullptr;
            ibv_qp* qp = nullptr;
        };

        RcDevice(const std::string& ipAddr, int gidIndex);
        ~RcDevice();

        Queue createQueue();
        void destroyQueue(Queue& queue);
        bool connectQueue(Queue& queue, uint32_t peerQpNum, const ibv_gid& peerGid,
            int localGidIndex, uint32_t rqPsn, uint32_t sqPsn);
        int chooseGidIndex(const ibv_gid& peerGid) const;

        ibv_mr* registerBuffer(void* buf, size_t len);
        void deregisterBuffer(ibv_mr* mr);

        bool postRecv(Queue& queue, ibv_mr* mr, size_t len);
        bool postWriteWithImm(Queue& queue, ibv_mr* mr, size_t len, uint64_t remoteAddr,
            uint32_t remoteKey, uint32_t immData);
        bool postRead(Queue& queue, ibv_mr* mr, size_t len, uint64_t remoteAddr,
            uint32_t remoteKey);
        bool postWrite(Queue& queue, ibv_mr* mr, size_t len, uint64_t remoteAddr,
            uint32_t remoteKey);
        bool waitForCompletion(Queue& queue, std::chrono::steady_clock::time_point deadline,
            ibv_wc& outWc);

        static std::string gidToStr(const ibv_gid& gid);

        const ibv_gid& getLocalGid() const { return gids[defaultGidIndex].gid; }
        const ibv_gid& getLocalGid(int gidIndex) const { return gids[gidIndex].gid; }
        int getDefaultGidIndex() const { return defaultGidIndex; }
        uint8_t getPortNum() const { return portNum; }
        std::string getDescription() const;

    private:
        struct GidEntry
        {
            ibv_gid gid = {};
            bool valid = false;
            bool isRoceV2 = false;
            uint32_t netdevIndex = 0; // the netdev this GID belongs to (a port can serve several)
        };

        ibv_context* context = nullptr;
        ibv_pd* pd = nullptr;
        uint8_t portNum = 1;
        std::vector<GidEntry> gids; // the port's GID table
        int defaultGidIndex = 0; // the RoCE v2 GID of the configured address, or the override
        bool gidIndexFixed = false; // true => defaultGidIndex was given by the user
        ibv_mtu pathMtu = IBV_MTU_1024;

        bool openDeviceForAddr(const std::string& ipAddr, int wantedGidIndex);
        bool postRdma(Queue& queue, ibv_wr_opcode opcode, ibv_mr* mr, size_t len,
            uint64_t remoteAddr, uint32_t remoteKey);
        static std::vector<GidEntry> readGidTable(ibv_context* context, uint8_t port);
        static bool isIPv4Mapped(const ibv_gid& gid);
        static bool isLinkLocal(const ibv_gid& gid);
};

#endif // RC_SUPPORT

#endif // RCDEVICE_H_
