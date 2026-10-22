// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef MODES_NETBENCH_NETBENCHENGINELIBFABRIC_H_
#define MODES_NETBENCH_NETBENCHENGINELIBFABRIC_H_

#include <ostream>
#include <string>
#include <vector>

#include "Common.h"
#include "modes/netbench/NetBenchEngine.h"

#ifdef LIBFABRIC_SUPPORT

#include <rdma/fabric.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_eq.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_tagged.h>

// the API version this code is written against (also the minimum runtime library version)
#define NETBENCH_FI_VERSION             FI_VERSION(1, 11)

#if FI_VERSION_GE(FI_VERSION(FI_MAJOR_VERSION, FI_MINOR_VERSION), FI_VERSION(1, 18))
    #define NETBENCH_HAVE_FI_1_18 // native tcp RDM protocol and shared memory endpoint option
#endif

/**
 * The libfabric engine: a reliable datagram (FI_EP_RDM) endpoint with tagged messages and
 * everything it needs: fabric, domain, address vector, completion queue and the registered
 * message buffer slab. Streams are told apart by the rank in the message tag, so a connection is
 * just the peer's address vector entry.
 */
class LibfabricEndpoint : public NetBenchEndpoint
{
    public:
        LibfabricEndpoint(const NetBenchSelector& selector, unsigned short fixedPort,
            size_t slabSize);
        virtual ~LibfabricEndpoint();

        virtual NetBenchEngineType getEngine() const override
            { return NetBenchEngineType::Libfabric; }
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
        virtual bool completionsPending() override;
        virtual bool addWaitFDs(std::vector<struct pollfd>& outFDs) override;

        static void printProviders(std::ostream& out);
        static std::string getVersionStr();
        static std::string getRuntimeVersionStr();

    private:
        struct LibfabricConn : public NetBenchConn
        {
            fi_addr_t fiAddr{FI_ADDR_UNSPEC}; // destination for sends
        };

        struct fi_info* info{NULL};
        struct fid_fabric* fabric{NULL};
        struct fid_domain* domain{NULL};
        struct fid_av* av{NULL};
        struct fid_cq* cq{NULL};
        struct fid_ep* ep{NULL};
        struct fid_mr* mr{NULL};
        void* mrDesc{NULL};
        int waitFD{-1}; // completion queue wait file descriptor; -1 if the provider has none
        NetBenchAddr localAddr;

        static struct fi_info* getInfoList(const char* provName, const char* srcAddr);
        static struct fi_info* selectInfo(const NetBenchSelector& selector);
        static struct fi_info* pickBestInfo(struct fi_info* list,
            const NetBenchSelector& selector);
        static bool matchesNetdev(const struct fi_info* info, const std::string& netdev);

        void closeAll();
        fi_addr_t insertAddr(const NetBenchAddr& addr);
        void putLocalInfo(bpt::ptree& outTree) const;
        void checkFamily(const NetBenchAddr& peerAddr, const char* peerRole) const;

    // inliners
    public:
        std::string getProvName() const { return info->fabric_attr->prov_name; }
        std::string getDomainName() const { return info->domain_attr->name; }
        uint32_t getProtocol() const { return info->ep_attr->protocol; }
        uint32_t getProtocolVersion() const { return info->ep_attr->protocol_version; }
};

#endif // LIBFABRIC_SUPPORT

#endif /* MODES_NETBENCH_NETBENCHENGINELIBFABRIC_H_ */
