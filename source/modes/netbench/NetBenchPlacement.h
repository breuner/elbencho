// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef MODES_NETBENCH_NETBENCHPLACEMENT_H_
#define MODES_NETBENCH_NETBENCHPLACEMENT_H_

#include <cstdint>
#include <string>
#include <vector>

#include "Common.h"
#include "modes/netbench/NetBenchEngine.h"

class ProgArgs;

/**
 * Where one client stream runs: which server, which of its interfaces and worker threads, and
 * which client interface. Computed by the coordinator (NetBenchPlacement) and sent to the
 * services in the prepare request.
 */
struct NetBenchStreamPlace
{
    uint32_t clientRank{0}; // global client thread rank
    size_t serverIdx{0}; // index in the servers list
    size_t serverIfaceIdx{0}; // index in the server's interfaces list of this run
    size_t serverWorkerIdx{0}; // worker thread of the server
    std::string serverIfaceAddr;
    std::string clientAddr;
};

typedef std::vector<NetBenchStreamPlace> NetBenchStreamPlaceVec;

/**
 * Coordinator side of the stream placement: asks every service for its current interfaces and
 * assigns each client stream to a server interface in the same subnet (unless streams across
 * subnets are allowed), balancing the stream counts over the servers first, then over the
 * interfaces of each server and of each client. With every interface in one subnet this is the
 * plain round-robin placement.
 */
class NetBenchPlacement
{
    public:
        void query(const ProgArgs& progArgs);
        void compute(const StringVec& hosts, unsigned numServers, size_t threadsPerClient,
            size_t threadsPerServer, bool allowCrossSubnet);
        std::string getIfacesStr(size_t hostIdx) const;
        std::string getStreamsStr(size_t hostIdx) const;
        size_t getMaxStreamsPerServer() const;
        size_t getMaxServerIfaces() const;

        static void parseStreams(const std::string& str, NetBenchStreamPlaceVec& out);

    private:
        std::vector<NetBenchIfaceVec> hostIfaces; // per host (server or client role)
        NetBenchStreamPlaceVec streams; // by client rank
        std::vector<size_t> streamsPerServer;
        unsigned numServers{0};
        size_t threadsPerClient{0};

        void logUneven(const StringVec& hosts, const std::vector<std::vector<bool>>& ifaceUsable,
            const std::vector<std::vector<size_t>>& streamsPerServerIface,
            const std::vector<std::vector<size_t>>& streamsPerClientIface) const;
};

#endif /* MODES_NETBENCH_NETBENCHPLACEMENT_H_ */
