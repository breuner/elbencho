// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef MODES_NETBENCH_NETBENCHENGINEIBVERBS_H_
#define MODES_NETBENCH_NETBENCHENGINEIBVERBS_H_

#include <string>
#include <vector>

#include "Common.h"
#include "modes/netbench/NetBenchEngine.h"
#include "modes/netbench/NetBenchEngineIbverbsBase.h"

#ifdef IBVERBS_SUPPORT

/**
 * The ibverbs engine: InfiniBand/RoCE reliable connections straight on libibverbs, one queue
 * pair per stream (data path in IbverbsEndpointBase). Queue pairs are connected by exchanging
 * their numbers, GIDs and sequence numbers over the service HTTP API, so there is no rdma_cm (and
 * none of its ECE negotiation) involved; see RdmaCmEndpoint for that.
 *
 * Device contexts and protection domains are opened once per device and kept for the lifetime of
 * the process, because they are shared by all endpoints and runs.
 */
class IbverbsEndpoint : public IbverbsEndpointBase
{
    public:
        IbverbsEndpoint(const NetBenchSelector& selector, size_t slabSize, size_t numConns,
            size_t recvsPerConn, size_t sendsPerConn);
        virtual ~IbverbsEndpoint();

        virtual NetBenchEngineType getEngine() const override
            { return NetBenchEngineType::Ibverbs; }
        virtual std::string getDescription() const override;
        virtual const NetBenchAddr& getAddr() const override { return localAddr; }

        virtual NetBenchConn* prepareConnect(uint32_t rank, bpt::ptree& request) override;
        virtual void completeConnect(NetBenchConn* conn, const bpt::ptree& reply) override;
        virtual NetBenchConn* acceptPeer(const bpt::ptree& request, bpt::ptree& reply) override;

    private:
        struct Device // process-wide, see openDevice()
        {
            struct ibv_context* context;
            struct ibv_pd* pd;
            struct ibv_device_attr attr;
        };

        struct IbvConn : public QpConn
        {
            uint32_t psn{0}; // our initial send sequence number
        };

        struct PeerInfo // what the HTTP connect request/reply carries about a queue pair
        {
            NetBenchAddr addr; // IP of the interface
            uint16_t lid;
            union ibv_gid gid;
            int gidType; // enum ibv_gid_type
            uint32_t qpn;
            uint32_t psn;
            int mtu; // enum ibv_mtu
        };

        Device* device{NULL};
        uint8_t portNum{0};
        struct ibv_port_attr portAttr;
        int gidIndex{-1};
        union ibv_gid gid;
        int gidType{0};
        NetBenchAddr localAddr; // IP of the interface behind the GID (port 0)

        static Device* openDevice(struct ibv_device* ibvDevice);
        void selectPort(const NetBenchSelector& selector);
        bool selectGid(const struct sockaddr* addr, int family);
        IbvConn* createConn(uint32_t rank);
        void connectQp(IbvConn* conn, const PeerInfo& peer);
        void putLocalInfo(bpt::ptree& outTree, const IbvConn* conn) const;
        PeerInfo readPeerInfo(const bpt::ptree& tree) const;
        void closeAll();

        static std::string gidToHex(const union ibv_gid& gid);
        static bool gidFromHex(const std::string& hex, union ibv_gid& outGid);
};

#endif // IBVERBS_SUPPORT

#endif /* MODES_NETBENCH_NETBENCHENGINEIBVERBS_H_ */
