// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef MODES_NETBENCH_NETBENCHPROGARGS_H_
#define MODES_NETBENCH_NETBENCHPROGARGS_H_

#include <boost/program_options.hpp>
#include <boost/property_tree/ptree.hpp>
#include <sstream>
#include <string>

#include "Common.h"
#include "modes/netbench/NetBenchEngine.h"
#include "modes/netbench/NetBenchPlacement.h"

namespace bpo = boost::program_options;
namespace bpt = boost::property_tree;

class ProgArgs;

#define ARG_CLIENTS_LONG                 "clients"
#define ARG_CLIENTSFILE_LONG             "clientsfile"
#define ARG_NETBENCH_LONG                "netbench"
#define ARG_NETBENCHIFACES_LONG          "netbenchifaces" // internal (not set by user)
#define ARG_NETBENCHRUNID_LONG           "netbenchrunid" // internal (not set by user)
#define ARG_NETBENCHSERVERSSTR_LONG      "netbenchservers" // internal (not set by user)
#define ARG_NETBENCHSVCIDX_LONG          "netbenchsvcidx" // internal (not set by user)
#define ARG_NETBENCHSTREAMS_LONG         "netbenchstreams" // internal (not set by user)
#define ARG_NETCROSSSUBNET_LONG          "netcrosssubnet"
#define ARG_NETDEVS_LONG                 "netdevs"
#define ARG_NETDEVSUBNETS_LONG           "netdevsubnets"
#define ARG_NETENGINE_LONG               "netengine"
#define ARG_NETPORT_LONG                 "netport"
#define ARG_NETPROV_LONG                 "netprov"
#define ARG_NETMEMMAX_LONG               "netmemmax"
#define ARG_NETRECVPCT_LONG              "netrecvpct"
#define ARG_NETRNR_LONG                  "netrnr"
#define ARG_NUMNETBENCHCLIENTS_LONG      "numclients" // internal (not set by user)
#define ARG_NUMNETBENCHSERVERS_LONG      "numservers" // internal (not set by user)
#define ARG_RECVSIZE_LONG                "recvsize"
#define ARG_SENDSIZE_LONG                "sendsize"
#define ARG_SERVERS_LONG                 "servers"
#define ARG_SERVERSFILE_LONG             "serversfile"
#define ARG_TCLIENTS_LONG                "tclients"
#define ARG_TSERVERS_LONG                "tservers"

#define NETPROV_LIST_ARG                 "list" // "--netprov list" lists the available providers
#define NETPROV_RDMA_ARG                 "rdma" // "--netprov rdma" takes the first RDMA provider
#define NETENGINE_DEFAULT                "tcp" // default of "--netengine"
#define NETMEMMAX_DEFAULT                "1G" // default of "--netmemmax"
#define NETRECVPCT_DEFAULT               200 // default of "--netrecvpct"
#define NETRNR_DEFAULT                   1 // default of "--netrnr": 10 us, the shortest timer
#define NETRNR_MAX                       31 // InfiniBand RNR NAK timer codes are 5 bits


/**
 * The netbench options of ProgArgs: definition, defaults, checks, help and service transfer.
 * ProgArgs owns one instance and calls the methods below at the matching points of its own
 * initialization sequence.
 *
 * The coordinator derives the service roles from the hosts list (servers first, then clients) and
 * sends each service its index in that list, so a service knows whether it is a server or a
 * client, and clients know which servers to connect to. Before a run, the coordinator asks every
 * service for its current interfaces and sends each service its part of the resulting stream
 * placement (NetBenchPlacement).
 */
class NetBenchProgArgs
{
    public:
        void defineArgs(bpo::options_description& generic, bpo::options_description& hidden);
        void defineDefaults();
        void initImplicitValues(ProgArgs& progArgs);
        void convertUnitStrings();
        void parseHosts(ProgArgs& progArgs);
        void parseNetDevs(bool runAsService);
        void checkArgs(ProgArgs& progArgs);
        void prepareRun(const ProgArgs& progArgs);
        void getIfacesAsPropertyTree(const std::string& ownHost, bool devSubnets, bpt::ptree& outTree) const;
        void printHelp();
        void printBuildInfo(std::ostringstream& includedStream,
            std::ostringstream& notIncludedStream);
        void setFromPropertyTree(const bpt::ptree& tree);
        void getAsPropertyTree(const ProgArgs& progArgs, bpt::ptree& outTree,
            size_t serviceRank) const;
        void reset();
        void printDryRunInfo(const ProgArgs& progArgs) const;
        uint64_t getNumRounds(const ProgArgs& progArgs) const;
        uint64_t getExpectedBytesPerClientThread(const ProgArgs& progArgs) const;
        uint64_t getExpectedBytesPerClient(const ProgArgs& progArgs) const;
        size_t getNumRecvSlots(size_t ioDepth, bool bidirectional) const;

        static std::string getLibfabricVersionStr();
        static std::string getBuiltInEnginesStr();

    private:
        // config options in alphabetic order...

        std::string clientsFilePath; // path to file with service hosts to use as clients
        std::string clientsStr; // service hosts to use as clients
        bool allowCrossSubnet; // place streams across IP subnets
        std::string netDevsStr; // service-local: interfaces to listen on (server) or bind (client)
        StringVec netDevsVec; // netDevsStr broken down into individual elements (unresolved)
        bool netDevSubnets; // placement subnets from the services' "--netdevs" subnet entries
        std::string netEngineStr; // transport engine name (see NetBenchEngineType)
        NetBenchEngineType netEngine; // parsed netEngineStr
        unsigned short netPort; // service-local: fixed base listen port (0 means OS-chosen)
        std::string netProv; // libfabric provider (empty for tcp, NETPROV_RDMA_ARG for any RDMA)
        std::string netMemMaxOrigStr; // original netMemMax str from user with unit
        uint64_t netMemMax; // max send+receive buffer memory per service (0 means unlimited)
        unsigned netRecvPct; // receive buffers of one-directional streams in percent of iodepth
        unsigned netRnrTimer; // receiver-not-ready retry timer code (ibverbs, rdmacm)
        unsigned numClients; // number of clients after the servers in the service hosts list
        unsigned numServers; // number of servers at the beginning of the service hosts list
        std::string recvSizeOrigStr; // original recvSize str from user with unit
        size_t recvSize; // server response length for each round of a stream
        std::string runID; // coordinator-generated ID of this benchmark run (set in checkArgs)
        std::string sendSizeOrigStr; // alias for block size in netbench mode (empty if not given)
        std::string serversFilePath; // path to file with service hosts to use as servers
        std::string serversStr; // service hosts to use as servers
        StringVec serversVec; // services only: resolved "host:port" of all servers
        size_t serviceIndex; // services only: own position in the coordinator's hosts list
        size_t threadsPerClient; // active netbench threads per client service (at most "-t")
        size_t threadsPerServer; // active netbench threads per server service (at most "-t")
        bool useDefaultFileSize; // "-s" not given, so it gets computed in checkArgs()
        bool useNetBench; // run network benchmarking

        NetBenchPlacement placement; // coordinator: the stream placement of the run
        StringVec runIfaces; // services: own interface addresses of the run (servers)
        NetBenchStreamPlaceVec runStreams; // services: own streams of the run

        void setDefaultFileSize(ProgArgs& progArgs) const;
        void checkNetMemMax(const ProgArgs& progArgs, uint64_t maxStreamsPerServer,
            uint64_t maxEndpointsPerServer) const;

        static void resolveNetDevs(const StringVec& entries, int family, NetBenchIfaceVec& out,
            StringVec& outUnresolved, bool cidrSubnets);

    // inliners
    public:
        // getters in alphabetic order...

        bool getAllowCrossSubnet() const { return allowCrossSubnet; }
        const StringVec& getNetDevsVec() const { return netDevsVec; }
        bool getNetDevSubnets() const { return netDevSubnets; }
        NetBenchEngineType getNetEngine() const { return netEngine; }
        unsigned short getNetPort() const { return netPort; }
        std::string getNetProv() const { return netProv; }
        uint64_t getNetMemMax() const { return netMemMax; }
        unsigned getNetRecvPct() const { return netRecvPct; }
        unsigned getNetRnrTimer() const { return netRnrTimer; }
        unsigned getNumClients() const { return numClients; }
        unsigned getNumServers() const { return numServers; }
        size_t getRecvSize() const { return recvSize; }
        std::string getRunID() const { return runID; }
        const StringVec& getRunIfaces() const { return runIfaces; }
        const NetBenchStreamPlaceVec& getRunStreams() const { return runStreams; }
        const StringVec& getServersVec() const { return serversVec; }
        size_t getServiceIndex() const { return serviceIndex; }
        size_t getThreadsPerClient() const { return threadsPerClient; }
        size_t getThreadsPerServer() const { return threadsPerServer; }
        bool getUseNetBench() const { return useNetBench; }

        bool isServer() const { return serviceIndex < numServers; }

        // coordinator: role of a host in the hosts list (servers first), both false outside netbench
        bool isServerService(size_t hostIdx) const { return useNetBench && (hostIdx < numServers); }
        bool isClientService(size_t hostIdx) const { return useNetBench && (hostIdx >= numServers); }
        const char* getServiceRoleSuffix(size_t hostIdx) const
            { return !useNetBench ? "" : isServerService(hostIdx) ? " [server]" : " [client]"; }
};

#endif /* MODES_NETBENCH_NETBENCHPROGARGS_H_ */
