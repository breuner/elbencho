// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <algorithm>
#include <cstdint>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <iostream>
#include <net/if.h>

#include "Logger.h"
#include "modes/netbench/NetBenchEngineLibfabric.h"
#include "modes/netbench/NetBenchProgArgs.h"
#include "modes/netbench/NetBenchProtocol.h"
#include "ProgArgs.h"
#include "ProgException.h"
#include "toolkits/TerminalTk.h"
#include "toolkits/TranslatorTk.h"
#include "toolkits/UnitTk.h"

#define HOSTLIST_DELIMITERS         ", \n\r" // delimiters for hosts string (same as ProgArgs)
#define NETDEV_DELIMITERS           ", \n\r" // delimiters for net dev list string

#define ENDL                        << std::endl << // just to make help text print lines shorter

#define FEATURE_NAME_LIBFABRIC      "libfabric"
#define FEATURE_NAME_IBVERBS        "ibverbs"
#define FEATURE_NAME_RDMACM         "rdmacm"


/**
 * Define the netbench options. Called by ProgArgs::defineAllowedArgs() at the alphabetical
 * position of "--netbench", so the netbench options appear as one block in the options list.
 */
void NetBenchProgArgs::defineArgs(bpo::options_description& generic,
    bpo::options_description& hidden)
{
    generic.add_options()
        (ARG_CLIENTS_LONG, bpo::value(&this->clientsStr),
            "Comma-separated list of service hosts to use as clients in netbench mode. "
            "(Format: hostname[:port])")
        (ARG_CLIENTSFILE_LONG, bpo::value(&this->clientsFilePath),
            "Path to file containing line-separated service hosts to use as clients in netbench "
            "mode. (Format: hostname[:port])")
        (ARG_NETBENCH_LONG, bpo::bool_switch(&this->useNetBench),
            "Run network benchmarking between service instances. Each client thread runs one "
            "stream to one of the servers (round-robin): per round it sends \"--" ARG_SENDSIZE_LONG
            "\" bytes and receives a response of \"--" ARG_RECVSIZE_LONG "\" bytes, until "
            "\"-" ARG_FILESIZE_SHORT "\" bytes have been sent (or received for a receive-only "
            "stream). Send size larger than receive size simulates writes from clients to "
            "servers, the other way around simulates reads. Either size may be 0 for a "
            "one-directional stream. See \"--" ARG_HELPNETBENCH_LONG "\" for details.")
        (ARG_NETCROSSSUBNET_LONG, bpo::bool_switch(&this->allowCrossSubnet),
            "Allow netbench streams between interfaces in different IP subnets. By default, a "
            "client interface is only paired with a server interface in the same subnet, "
            "because separate subnets are usually separate networks.")
        (ARG_NETDEVS_LONG, bpo::value(&this->netDevsStr),
            "Comma-separated list of network interfaces for netbench mode, given as interface "
            "name, IP address, IP subnet in CIDR notation (e.g. \"172.200.0.0/16\" for all "
            "interfaces with an address in that subnet) or libfabric domain name. Service option: "
            "a server listens on each of them, a client binds its threads to them. The "
            "coordinator pairs client and server interfaces of the same subnet and balances the "
            "streams over servers and interfaces; the interfaces are checked when a run starts. "
            "(Default: the interface matching the service address in \"--" ARG_SERVERS_LONG "\" "
            "for servers, one interface per subnet for clients)")
        (ARG_NETDEVSUBNETS_LONG, bpo::bool_switch(&this->netDevSubnets),
            "Netbench mode: take the subnets for the stream placement from the IP subnets given "
            "in the services' \"--" ARG_NETDEVS_LONG "\" lists instead of from the interface "
            "netmasks, e.g. for interfaces with host addresses (\"/32\") in a routed network. "
            "Interfaces given by name or address keep the subnet of their netmask.")
        (ARG_NETENGINE_LONG, bpo::value(&this->netEngineStr),
            "Transport engine for netbench mode: \"tcp\" for plain TCP sockets, \"ibverbs\" "
            "for InfiniBand/RoCE reliable connections via libibverbs, \"rdmacm\" for the same "
            "connections set up by RDMA CM (librdmacm), which lets the adapters negotiate ECE, "
            "e.g. for Zero Touch RoCE, \"libfabric\" for the libfabric provider selected via "
            "\"--" ARG_NETPROV_LONG "\". All services of a run use the engine given to the "
            "coordinator. (Default: " NETENGINE_DEFAULT ")")
        (ARG_NETPORT_LONG, bpo::value(&this->netPort),
            "Netbench mode service option: fixed base port for the server data endpoints instead "
            "of ports chosen by the OS. Endpoint number N listens on this port plus N. (Default: "
            "0 for OS-chosen ports)")
        (ARG_NETPROV_LONG, bpo::value(&this->netProv),
            "libfabric provider for netbench mode, e.g. \"tcp\" or \"verbs;ofi_rxm\". Implies "
            "\"--" ARG_NETENGINE_LONG " libfabric\". The special value \"" NETPROV_RDMA_ARG "\" "
            "takes the first available RDMA provider (verbs, efa, cxi, ...), \"" NETPROV_LIST_ARG
            "\" lists the providers and interfaces available on this host. (Default: tcp)")
        (ARG_NETMEMMAX_LONG, bpo::value(&this->netMemMaxOrigStr),
            "Netbench mode limit for the send and receive buffer memory of a service, for all "
            "engines. A client needs \"--" ARG_IODEPTH_LONG "\" x \"--" ARG_SENDSIZE_LONG "\" plus "
            "its receive buffers x \"--" ARG_RECVSIZE_LONG "\" per thread, a server one receive "
            "buffer for every block its clients can have in flight (see \"--" ARG_NETRECVPCT_LONG
            "\") plus one response buffer per endpoint; the "
            "RDMA engines pin this memory. A run that would need more than this on a client or "
            "on a server is rejected with the amounts it needs. (Default: " NETMEMMAX_DEFAULT
            "; supports base2 suffixes, e.g. \"4G\"; 0 for no limit)")
        (ARG_NETRECVPCT_LONG, bpo::value(&this->netRecvPct),
            "Netbench mode receive buffers of a one-directional stream, in percent of the "
            "sender's \"--" ARG_IODEPTH_LONG "\": on the server for send-only streams (\"--"
            ARG_RECVSIZE_LONG " 0\"), on the client for receive-only streams (\"--" ARG_SENDSIZE_LONG
            " 0\"). Values above 100 give the receiver a margin before the sender has to wait, "
            "values below 100 provoke receiver-not-ready retries (see \"--" ARG_NETRNR_LONG "\"). "
            "Ping-pong streams always have one receive buffer per round in flight. (Default: "
            STRINGIZE(NETRECVPCT_DEFAULT) ")")
        (ARG_NETRNR_LONG, bpo::value(&this->netRnrTimer),
            "Netbench mode receiver-not-ready retry timer of the ibverbs and rdmacm engines, as "
            "InfiniBand timer code 0-" STRINGIZE(NETRNR_MAX) " (1 = 10 us, 12 = 0.64 ms, 0 = 655 "
            "ms): how long a peer waits before it resends a block that arrived while the receiver "
            "had no buffer posted (a server reposts a consumed buffer only after it saw the "
            "completion). (Default: " STRINGIZE(NETRNR_DEFAULT) ")")
        (ARG_RECVSIZE_LONG, bpo::value(&this->recvSizeOrigStr),
            "Netbench mode response size in bytes. Servers send this amount of data to a client "
            "for each received block, or as a continuous stream of blocks if the send size is 0. "
            "(Default: 1; supports base2 suffixes, e.g. \"2M\")")
        (ARG_SENDSIZE_LONG, bpo::value(&this->sendSizeOrigStr),
            "Netbench mode block size in bytes that each client thread sends per round. This is "
            "the netbench name for \"-" ARG_BLOCK_SHORT "\" and may be 0 for a receive-only "
            "stream. (Default: 1M; supports base2 suffixes)")
        (ARG_SERVERS_LONG, bpo::value(&this->serversStr),
            "Comma-separated list of service hosts to use as servers in netbench mode. "
            "(Format: hostname[:port])")
        (ARG_SERVERSFILE_LONG, bpo::value(&this->serversFilePath),
            "Path to file containing line-separated service hosts to use as servers in netbench "
            "mode. (Format: hostname[:port])")
        (ARG_TCLIENTS_LONG, bpo::value(&this->threadsPerClient),
            "Number of netbench threads per client service, if fewer than \"-" ARG_NUMTHREADS_SHORT
            "\" should run streams. The remaining threads of a client stay idle. Without \"-"
            ARG_NUMTHREADS_SHORT "\", the thread count per service is the larger of \"--"
            ARG_TSERVERS_LONG "\" and \"--" ARG_TCLIENTS_LONG "\". (Default: \"-"
            ARG_NUMTHREADS_SHORT "\")")
        (ARG_TSERVERS_LONG, bpo::value(&this->threadsPerServer),
            "Number of netbench threads per server service, if fewer than \"-" ARG_NUMTHREADS_SHORT
            "\" should serve streams. The remaining threads of a server stay idle. (Default: \"-"
            ARG_NUMTHREADS_SHORT "\")")
    ;
}

void NetBenchProgArgs::defineDefaults()
{
    this->allowCrossSubnet = false;
    this->netEngine = NetBenchEngineType::Tcp;
    this->netEngineStr = NETENGINE_DEFAULT;
    this->netPort = 0;
    this->netMemMax = 0;
    this->netMemMaxOrigStr = NETMEMMAX_DEFAULT;
    this->netDevSubnets = false;
    this->netRecvPct = NETRECVPCT_DEFAULT;
    this->netRnrTimer = NETRNR_DEFAULT;
    this->numClients = 0;
    this->numServers = 0;
    this->recvSize = 1;
    this->recvSizeOrigStr = "1";
    this->serviceIndex = 0;
    this->threadsPerClient = 0;
    this->threadsPerServer = 0;
    this->useDefaultFileSize = false;
    this->useNetBench = false;
}

/**
 * Netbench mode implications for the core options: write phase only, no block variance unless
 * requested, "--sendsize" as block size. The default file size is computed in checkArgs().
 */
void NetBenchProgArgs::initImplicitValues(ProgArgs& progArgs)
{
    if(!useNetBench)
        return;

    progArgs.runCreateFilesPhase = true;

    // "-t" is the thread count of every service, servers and clients may use fewer of them
    if(!progArgs.argsVariablesMap.count(ARG_NUMTHREADS_LONG) &&
        (threadsPerServer || threadsPerClient) )
        progArgs.numThreads = std::max(threadsPerServer, threadsPerClient);

    if(!threadsPerServer)
        threadsPerServer = progArgs.numThreads;

    if(!threadsPerClient)
        threadsPerClient = progArgs.numThreads;

    if(!progArgs.argsVariablesMap.count(ARG_BLOCKVARIANCE_LONG) )
        progArgs.blockVariancePercent = 0;

    if(!sendSizeOrigStr.empty() )
    {
        if(progArgs.argsVariablesMap.count(ARG_BLOCK_LONG) &&
            (progArgs.blockSizeOrigStr != sendSizeOrigStr) )
            throw ProgException("\"--" ARG_SENDSIZE_LONG "\" and \"-" ARG_BLOCK_SHORT "\" are "
                "the same option in netbench mode, but were given with different values.");

        progArgs.blockSizeOrigStr = sendSizeOrigStr;
    }

    useDefaultFileSize = !progArgs.argsVariablesMap.count(ARG_FILESIZE_LONG);

    // netbench uses dir mode. transfer size is 1x filesize per thread. this is for live stats.
    progArgs.numDirsOrigStr = "0";
    progArgs.numDirs = 0;
    progArgs.numFilesOrigStr = "1";
    progArgs.numFiles = 1;
}

void NetBenchProgArgs::convertUnitStrings()
{
    recvSize = UnitTk::numHumanToBytesBinary(recvSizeOrigStr, false);
    netMemMax = UnitTk::numHumanToBytesBinary(netMemMaxOrigStr, false);
}

/**
 * Parse the servers and clients lists into the hosts list of ProgArgs, servers first.
 */
void NetBenchProgArgs::parseHosts(ProgArgs& progArgs)
{
    progArgs.parseHostsList(serversStr, serversFilePath, progArgs.hostsVec);
    numServers = progArgs.hostsVec.size();
    progArgs.parseHostsList(clientsStr, clientsFilePath, progArgs.hostsVec);
    numClients = progArgs.hostsVec.size() - numServers;
}

/**
 * Split the "--netdevs" entries. A service also checks them once at startup: it is an error if
 * none of them exists, a warning if some do not (they are resolved again at every run, so an
 * interface that comes up later is picked up).
 *
 * @throw ProgException if the list is empty or no entry exists.
 */
void NetBenchProgArgs::parseNetDevs(bool runAsService)
{
    if(netDevsStr.empty() )
        return;

    netDevsVec.clear();

    TranslatorTk::splitAndExpandStr(netDevsStr, NETDEV_DELIMITERS, netDevsVec);
    TranslatorTk::eraseEmptyStringsFromVec(netDevsVec);

    if(netDevsVec.empty() )
        throw ProgException("Network interfaces defined, but parsing resulted in an empty list: " +
            netDevsStr);

    if(!runAsService)
        return;

    NetBenchIfaceVec ifaces;
    StringVec unresolved;

    resolveNetDevs(netDevsVec, AF_UNSPEC, ifaces, unresolved, false);

    if(ifaces.empty() )
        throw ProgException("None of the network interfaces given with \"--" ARG_NETDEVS_LONG
            "\" exists: " + netDevsStr);

    for(const std::string& entry : unresolved)
        LOGGER(Log_NORMAL, "WARNING: No network interface currently matches \"--"
            ARG_NETDEVS_LONG "\" entry \"" << entry << "\" (re-checked at each benchmark run)." <<
            std::endl);
}

/**
 * Resolve "--netdevs" entries to interface addresses with their subnets, as of now: a subnet
 * gives every interface address in it, an interface name its addresses (one per subnet), an IP
 * address itself. Anything else is a device or domain name for the engine and passes through.
 *
 * @param family address family for interface names (AF_UNSPEC for any; used as fallback).
 * @param outUnresolved entries that gave no address.
 */
void NetBenchProgArgs::resolveNetDevs(const StringVec& entries, int family, NetBenchIfaceVec& out,
    StringVec& outUnresolved, bool cidrSubnets)
{
    NetBenchIfaceVec resolved;

    for(const std::string& entry : entries)
    {
        const size_t numBefore = resolved.size();
        sockaddr_storage ipAddr;

        if(entry.find('/') != std::string::npos)
            NetBenchIP::expandSubnet(entry, false, resolved, cidrSubnets);
        else
        if(NetBenchIP::parseIP(entry, ipAddr) )
            resolved.push_back( { entry, NetBenchIP::ifaceSubnet( (const sockaddr*)&ipAddr, 0) } );
        else
        if(if_nametoindex(entry.c_str() ) )
        {
            NetBenchIP::ifaceAddrs(entry, family, resolved);

            if(resolved.size() == numBefore)
                NetBenchIP::ifaceAddrs(entry, AF_UNSPEC, resolved);
        }
        else
            resolved.push_back( { entry, "" } );

        if(resolved.size() == numBefore)
            outUnresolved.push_back(entry);
    }

    // (two entries may select the same interface, e.g. "eth0,192.168.0.0/24")
    for(const NetBenchIface& iface : resolved)
        if(std::none_of(out.begin(), out.end(),
            [&](const NetBenchIface& o) { return o.addr == iface.addr; } ) )
            out.push_back(iface);
}

/**
 * Service: the current interfaces for the placement, for the client role (NETBENCH_JSON_IFACES)
 * and for the server role (NETBENCH_JSON_SERVERIFACES). Without "--netdevs", a client may use
 * one interface per subnet and a server the one of its service address.
 *
 * @param ownHost this service's host as the coordinator addresses it (decides the address family
 *      of named interfaces).
 */
void NetBenchProgArgs::getIfacesAsPropertyTree(const std::string& ownHost, bool devSubnets,
    bpt::ptree& outTree) const
{
    const std::vector<NetBenchAddr> ownAddrs = NetBenchIP::resolveHost(ownHost);
    const int family = ownAddrs.empty() ? AF_UNSPEC : ownAddrs[0].getFamily();

    NetBenchIfaceVec clientIfaces;
    NetBenchIfaceVec serverIfaces;
    StringVec unresolved;

    if(!netDevsVec.empty() )
    {
        resolveNetDevs(netDevsVec, family, clientIfaces, unresolved, devSubnets);
        serverIfaces = clientIfaces;
    }
    else
    {
        // (one interface per subnet, as the kernel routes through the first one of a subnet anyway)
        NetBenchIfaceVec allIfaces;
        NetBenchIP::expandSubnet("0.0.0.0/0", true, allIfaces);
        NetBenchIP::expandSubnet("::/0", true, allIfaces);

        for(const NetBenchIface& iface : allIfaces)
            if(std::none_of(clientIfaces.begin(), clientIfaces.end(),
                [&](const NetBenchIface& o) { return o.subnet == iface.subnet; } ) )
                clientIfaces.push_back(iface);

        if(!ownAddrs.empty() )
            serverIfaces.push_back( { ownAddrs[0].ipToStr(),
                NetBenchIP::ifaceSubnet(ownAddrs[0].getSockAddr(), 0) } );
    }

    for(const std::string& entry : unresolved)
        LOGGER(Log_VERBOSE, "No network interface currently matches \"--" ARG_NETDEVS_LONG
            "\" entry: " << entry << std::endl);

    auto putIfaces = [&](const char* key, const NetBenchIfaceVec& ifaces)
    {
        bpt::ptree list;

        for(const NetBenchIface& iface : ifaces)
        {
            bpt::ptree item;
            item.put(NETBENCH_JSON_ADDR, iface.addr);
            item.put(NETBENCH_JSON_SUBNET, iface.subnet);
            list.push_back(std::make_pair("", item) );
        }

        outTree.put_child(key, list);
    };

    putIfaces(NETBENCH_JSON_IFACES, clientIfaces);
    putIfaces(NETBENCH_JSON_SERVERIFACES, serverIfaces);
}

/**
 * Coordinator: query the services' interfaces and compute the stream placement. Call when the
 * services are reachable and before the preparation phase; no-op outside netbench mode.
 *
 * @throw ProgException if a service cannot be queried or the streams cannot be placed.
 */
void NetBenchProgArgs::prepareRun(const ProgArgs& progArgs)
{
    if(!useNetBench || progArgs.getInterruptServices() || progArgs.getQuitServices() )
        return;

    placement.query(progArgs);
    placement.compute(progArgs.getHostsVec(), numServers, threadsPerClient, threadsPerServer,
        allowCrossSubnet);

    checkNetMemMax(progArgs, placement.getMaxStreamsPerServer(),
        threadsPerServer * placement.getMaxServerIfaces() );
}

/**
 * Check the netbench options on the coordinator and set the default file size. Also handles the
 * provider listing request.
 *
 * @throw ProgException on invalid combinations.
 */
void NetBenchProgArgs::checkArgs(ProgArgs& progArgs)
{
    if(!useNetBench)
        return;

    // engine: "--netprov" belongs to libfabric and implies it unless another engine is explicit
    if(!netBenchEngineFromName(netEngineStr, netEngine) )
        throw ProgException("Unknown netbench engine: \"" + netEngineStr + "\". Valid values for "
            "\"--" ARG_NETENGINE_LONG "\": " + netBenchEngineNamesStr() );

    if(!netProv.empty() && !progArgs.argsVariablesMap.count(ARG_NETENGINE_LONG) )
        netEngine = NetBenchEngineType::Libfabric;

    if(!netProv.empty() && (netEngine != NetBenchEngineType::Libfabric) )
        throw ProgException("\"--" ARG_NETPROV_LONG "\" selects a libfabric provider and cannot be "
            "combined with \"--" ARG_NETENGINE_LONG " " + netEngineStr + "\". Use \"--"
            ARG_NETENGINE_LONG " ibverbs\" for RDMA without libfabric.");

    if(!netBenchEngineIsBuiltIn(netEngine) )
        throw ProgException(std::string("This executable was built without support for the "
            "netbench engine \"") + netBenchEngineName(netEngine) + "\". Available: " +
            getBuiltInEnginesStr() + ".");

    if(!netRecvPct)
        throw ProgException("\"--" ARG_NETRECVPCT_LONG "\" must be at least 1.");

    if(netRnrTimer > NETRNR_MAX)
        throw ProgException("\"--" ARG_NETRNR_LONG "\" must be between 0 and "
            STRINGIZE(NETRNR_MAX) ".");

    if(progArgs.argsVariablesMap.count(ARG_NETRNR_LONG) &&
        (netEngine != NetBenchEngineType::Ibverbs) && (netEngine != NetBenchEngineType::Rdmacm) )
        throw ProgException("\"--" ARG_NETRNR_LONG "\" is only supported by the ibverbs and rdmacm "
            "engines (the libfabric verbs provider takes FI_VERBS_MIN_RNR_TIMER from the "
            "environment of the services).");

#ifdef LIBFABRIC_SUPPORT
    if(netProv == NETPROV_LIST_ARG)
    {
        LibfabricEndpoint::printProviders(std::cout);
        throw ProgException("Select a provider via \"--" ARG_NETPROV_LONG "\" or use "
            "\"--" ARG_NETPROV_LONG " " NETPROV_RDMA_ARG "\" for the first RDMA provider.");
    }
#endif // LIBFABRIC_SUPPORT

    if(progArgs.getHostsVec().empty() )
        throw ProgException("Missing servers & clients definition for netbench mode.");

    if(!numServers)
        throw ProgException("At least one server needs to be defined for netbench mode.");

    if(numServers >= progArgs.getHostsVec().size() )
        throw ProgException("At least one client needs to be defined for netbench mode.");

    if(!progArgs.getBlockSize() && !recvSize)
        throw ProgException("Send size and receive size must not both be zero in netbench mode.");

    if(useDefaultFileSize)
        setDefaultFileSize(progArgs);

    if(!progArgs.getFileSize() )
        throw ProgException("File size must not be zero in netbench mode.");

    if(!progArgs.getIODepth() )
        throw ProgException("IO depth must not be zero in netbench mode.");

    if( (threadsPerServer > progArgs.getNumThreads() ) ||
        (threadsPerClient > progArgs.getNumThreads() ) )
        throw ProgException("\"--" ARG_TSERVERS_LONG "\" and \"--" ARG_TCLIENTS_LONG "\" may not "
            "exceed the number of threads per service (\"-" ARG_NUMTHREADS_SHORT "\").");

    // (the placement may give a server more streams and interfaces than this estimate, so
    // prepareRun() checks again with the real numbers)
    checkNetMemMax(progArgs,
        ( (uint64_t)numClients * threadsPerClient + numServers - 1) / numServers, threadsPerServer);

    if(progArgs.getRunCreateDirsPhase() || progArgs.getRunDeleteDirsPhase() ||
        progArgs.getRunStatFilesPhase() || progArgs.getRunReadPhase() ||
        progArgs.getRunDeleteFilesPhase() )
        throw ProgException("Netbench mode only run in write phase.");

    if(progArgs.getFileOffset() )
        throw ProgException("Netbench mode does not support \"--" ARG_FILEOFFSET_LONG "\".");

    if(!progArgs.getIsServicePathShared() )
        throw ProgException("Netbench mode does not support \"--" ARG_NOSVCPATHSHARE_LONG "\".");

    if(progArgs.getRankOffset() )
        throw ProgException("Netbench mode does not support \"--" ARG_RANKOFFSET_LONG "\".");

    if(!netDevsStr.empty() )
        LOGGER(Log_NORMAL, "NOTE: \"--" ARG_NETDEVS_LONG "\" is a service option and has no "
            "effect on the coordinator." << std::endl);

    // services recognize stale endpoints of an earlier run by this ID
    runID = boost::uuids::to_string(boost::uuids::random_generator()() );
}

/**
 * "--netmemmax": the send and receive buffers of a client (see NetBenchMode::initClient) and of
 * a server (see NetBenchMode::initServer) may not exceed the limit, so that nobody pins gigabytes
 * by accident.
 *
 * @param maxStreamsPerServer streams of the server with the most streams.
 * @param maxEndpointsPerServer threads x interfaces of the server with the most interfaces.
 * @throw ProgException with both amounts if either side would exceed the limit.
 */
void NetBenchProgArgs::checkNetMemMax(const ProgArgs& progArgs, uint64_t maxStreamsPerServer,
    uint64_t maxEndpointsPerServer) const
{
    const uint64_t ioDepth = progArgs.getIODepth();
    const uint64_t sendSize = progArgs.getBlockSize();
    const uint64_t clientSlots = recvSize ? getNumRecvSlots(ioDepth, sendSize != 0) : 0; // per thread
    const uint64_t serverSlots = sendSize ? getNumRecvSlots(ioDepth, recvSize != 0) : 0; // per stream

    const uint64_t clientBytes = threadsPerClient * ( (ioDepth * sendSize) + (clientSlots * recvSize) );
    const uint64_t serverBytes = (maxStreamsPerServer * serverSlots * sendSize) +
        (maxEndpointsPerServer * recvSize);

    if(!netMemMax || ( (clientBytes <= netMemMax) && (serverBytes <= netMemMax) ) )
        return;

    auto sizeStr = [](uint64_t bytes) { return UnitTk::numToHumanStrBase2(bytes) + "B"; };

    throw ProgException("Netbench buffers would need " + sizeStr(serverBytes) + " per server (" +
        std::to_string(maxStreamsPerServer) + " streams x " + std::to_string(serverSlots) +
        " receive buffers x send size " + sizeStr(sendSize) + " + " +
        std::to_string(maxEndpointsPerServer) + " endpoints x receive size " + sizeStr(recvSize) +
        ") and " + sizeStr(clientBytes) + " per client (" + std::to_string(threadsPerClient) +
        " threads x (IO depth " + std::to_string(ioDepth) + " x send size " + sizeStr(sendSize) +
        " + " + std::to_string(clientSlots) + " receive buffers x receive size " +
        sizeStr(recvSize) + ")), which exceeds \"--" ARG_NETMEMMAX_LONG "\" (" +
        sizeStr(netMemMax) + "). Raise the limit or reduce \"--" ARG_IODEPTH_LONG "\", the "
        "block sizes, the thread counts or \"--" ARG_NETRECVPCT_LONG "\".");
}

/**
 * Receive buffers of a stream on the receiving side. A ping-pong stream needs exactly one per
 * round in flight (the sender waits for the response before it reuses a slot), a one-directional
 * stream gets "--netrecvpct" of the sender's rounds in flight.
 */
size_t NetBenchProgArgs::getNumRecvSlots(size_t ioDepth, bool bidirectional) const
{
    if(bidirectional)
        return ioDepth;

    return std::max<size_t>(1, ( (ioDepth * netRecvPct) + 99) / 100);
}

/**
 * Default "-s": the largest size per stream whose total over all streams and both directions
 * still fits the 64 bit counters, so runs end by "--timelimit" in practice.
 */
void NetBenchProgArgs::setDefaultFileSize(ProgArgs& progArgs) const
{
    const uint64_t sendSize = progArgs.getBlockSize();
    const uint64_t unitSize = sendSize ? sendSize : recvSize;
    const uint64_t numStreams = std::max<uint64_t>(1, (uint64_t)numClients * threadsPerClient);
    const uint64_t bytesPerRound = std::max<uint64_t>(1, sendSize + recvSize);
    const uint64_t numRounds = UINT64_MAX / numStreams / bytesPerRound;

    progArgs.fileSize = numRounds * unitSize;
    progArgs.fileSizeOrigStr = std::to_string(progArgs.fileSize);
}

/**
 * @return comma-separated names of the engines in this executable.
 */
std::string NetBenchProgArgs::getBuiltInEnginesStr()
{
    std::string result;

    for(NetBenchEngineType engine : { NetBenchEngineType::Tcp, NetBenchEngineType::Ibverbs,
        NetBenchEngineType::Rdmacm, NetBenchEngineType::Libfabric } )
        if(netBenchEngineIsBuiltIn(engine) )
            result += (result.empty() ? "" : ", ") + std::string(netBenchEngineName(engine) );

    return result.empty() ? "none" : result;
}

void NetBenchProgArgs::printHelp()
{
    std::cout <<
        "Network benchmarking between hosts." ENDL
        std::endl <<
        "Usage:" ENDL
        "  Start a service on each host that takes part, servers and clients alike:" ENDL
        "    $ " EXE_NAME " --" ARG_RUNASSERVICE_LONG " [SERVICE OPTIONS]" ENDL
        "  Run the benchmark from any host:" ENDL
        "    $ " EXE_NAME " --" ARG_NETBENCH_LONG " --" ARG_SERVERS_LONG " HOSTS "
            "--" ARG_CLIENTS_LONG " HOSTS [OPTIONS]" ENDL
        std::endl;

    bpo::options_description basicDescription(
        "Basic Options", TerminalTk::getTerminalLineLength(80) );

    basicDescription.add_options()
        (ARG_NETBENCH_LONG, bpo::bool_switch(),
            "Run network benchmarking between service instances instead of storage I/O.")
        (ARG_SERVERS_LONG, bpo::value<std::string>(),
            "Comma-separated service hosts that act as servers. (Format: hostname[:port])")
        (ARG_CLIENTS_LONG, bpo::value<std::string>(),
            "Comma-separated service hosts that act as clients. Each client thread runs one "
            "stream to one server, round-robin over the servers. (Format: hostname[:port])")
        (ARG_SENDSIZE_LONG, bpo::value<std::string>(),
            "Bytes that a client thread sends to its server per round (same as \"-"
            ARG_BLOCK_SHORT "\"). 0 for a receive-only stream. (Default: 1M)")
        (ARG_RECVSIZE_LONG, bpo::value<std::string>(),
            "Bytes that the server sends back per round. A large send size with a small receive "
            "size simulates writes to a storage server, the reverse simulates reads. 0 for a "
            "send-only stream. (Default: 1)")
        (ARG_NUMTHREADS_LONG "," ARG_NUMTHREADS_SHORT, bpo::value<size_t>(),
            "Number of threads per service. Each client thread runs one stream, each server "
            "thread serves streams. (Default: 1)")
        (ARG_TSERVERS_LONG, bpo::value<size_t>(),
            "Netbench threads per server, if fewer than \"-" ARG_NUMTHREADS_SHORT "\". (Default: "
            "\"-" ARG_NUMTHREADS_SHORT "\")")
        (ARG_TCLIENTS_LONG, bpo::value<size_t>(),
            "Netbench threads per client, if fewer than \"-" ARG_NUMTHREADS_SHORT "\". Without "
            "\"-" ARG_NUMTHREADS_SHORT "\", the thread count per service is the larger of \"--"
            ARG_TSERVERS_LONG "\" and \"--" ARG_TCLIENTS_LONG "\". (Default: \"-"
            ARG_NUMTHREADS_SHORT "\")")
    ;

    std::cout << basicDescription << std::endl;

    bpo::options_description frequentDescription(
        "Frequently Used Options", TerminalTk::getTerminalLineLength(80) );

    frequentDescription.add_options()
        (ARG_TIMELIMITSECS_LONG, bpo::value<size_t>(),
            "Stop the benchmark after this many seconds. (Default: 0 for no limit)")
        (ARG_IODEPTH_LONG, bpo::value<size_t>(),
            "Rounds in flight per stream. 1 means ping-pong, where the latency is the round trip "
            "time. (Default: 1)")
        (ARG_NETENGINE_LONG, bpo::value<std::string>(),
            "Transport engine: \"tcp\" (plain TCP sockets), \"ibverbs\" (InfiniBand/RoCE via "
            "libibverbs), \"rdmacm\" (the same via RDMA CM, with ECE negotiation for Zero Touch "
            "RoCE) or \"libfabric\" (provider via \"--" ARG_NETPROV_LONG "\"). (Default: "
            NETENGINE_DEFAULT ")")
        (ARG_NETPROV_LONG, bpo::value<std::string>(),
            "libfabric provider, e.g. \"tcp\", \"verbs;ofi_rxm\" or \"" NETPROV_RDMA_ARG "\" for "
            "the first RDMA provider; implies the libfabric engine. \"" NETPROV_LIST_ARG "\" "
            "lists the providers and interfaces of this host. (Default: tcp)")
        (ARG_LATENCY_LONG, bpo::bool_switch(),
            "Show minimum, average and maximum round trip latency.")
    ;

    std::cout << frequentDescription << std::endl;

    bpo::options_description serviceDescription(
        "Service Options (given to the \"--" ARG_RUNASSERVICE_LONG "\" instances)",
        TerminalTk::getTerminalLineLength(80) );

    serviceDescription.add_options()
        (ARG_NETDEVS_LONG, bpo::value<std::string>(),
            "Comma-separated network interfaces for the data endpoints, given as interface name, "
            "IP address, IP subnet (\"172.200.0.0/16\" for all interfaces in that subnet) or "
            "libfabric domain name. A server listens on each of them, a client binds its threads "
            "to them. Streams are placed between interfaces of the same subnet (see \"--"
            ARG_NETCROSSSUBNET_LONG "\"), balanced over servers and interfaces; the interfaces "
            "are checked when a run starts. (Default: the interface of the service address for "
            "servers, one interface per subnet for clients)")
        (ARG_NETPORT_LONG, bpo::value<unsigned short>(),
            "Fixed base port for the data endpoints of a server; endpoint number N listens on "
            "this port plus N. (Default: 0 for OS-chosen ports, announced via the service API)")
    ;

    std::cout << serviceDescription << std::endl;

    bpo::options_description miscDescription(
        "Miscellaneous Options", TerminalTk::getTerminalLineLength(80) );

    miscDescription.add_options()
        (ARG_FILESIZE_LONG "," ARG_FILESIZE_SHORT, bpo::value<std::string>(),
            "Bytes to send per client thread, or to receive for a receive-only stream. (Default: "
            "the largest value that fits the counters, so runs are ended by \"--"
            ARG_TIMELIMITSECS_LONG "\")")
        (ARG_NETCROSSSUBNET_LONG, bpo::bool_switch(),
            "Allow streams between interfaces in different IP subnets. By default, client and "
            "server interfaces are only paired within the same subnet.")
        (ARG_NETDEVSUBNETS_LONG, bpo::bool_switch(),
            "Take the subnets for the stream placement from the IP subnets given in the "
            "services' \"--" ARG_NETDEVS_LONG "\" lists instead of from the interface netmasks "
            "(for interfaces with host addresses, e.g. \"/32\", in a routed network).")
        (ARG_NETMEMMAX_LONG, bpo::value<std::string>(),
            "Limit for the send and receive buffer memory of a client or a server (see the "
            "Memory section of docs/netbench.md). Runs that would need more are rejected with "
            "the amounts they need. (Default: " NETMEMMAX_DEFAULT ")")
        (ARG_NETRECVPCT_LONG, bpo::value<unsigned>(),
            "Receive buffers of a one-directional stream in percent of the sender's \"--"
            ARG_IODEPTH_LONG "\" (server side of send-only streams, client side of receive-only "
            "streams). Below 100 provokes receiver-not-ready retries. (Default: "
            STRINGIZE(NETRECVPCT_DEFAULT) ")")
        (ARG_NETRNR_LONG, bpo::value<unsigned>(),
            "Receiver-not-ready retry timer of the ibverbs and rdmacm engines, as InfiniBand "
            "timer code 0-" STRINGIZE(NETRNR_MAX) " (1 = 10 us, 12 = 0.64 ms, 0 = 655 ms). "
            "(Default: " STRINGIZE(NETRNR_DEFAULT) ")")
        (ARG_SHOWSVCMINMAX_LONG, bpo::bool_switch(),
            "Show the clients and the servers with the lowest and highest IOPS and throughput "
            "(rate and host name, plus the average), to make outliers visible. Also adds these "
            "values to the json result file.")
        (ARG_SERVERSFILE_LONG, bpo::value<std::string>(),
            "Path to file with line-separated server hosts.")
        (ARG_CLIENTSFILE_LONG, bpo::value<std::string>(),
            "Path to file with line-separated client hosts.")
    ;

    std::cout << miscDescription << std::endl;

    std::cout <<
        "Examples:" ENDL
        "  Simulate writes with 1MiB blocks, 8 streams per client, for 30 seconds:" ENDL
        "    $ " EXE_NAME " --" ARG_NETBENCH_LONG " --" ARG_SERVERS_LONG " s1,s2 "
            "--" ARG_CLIENTS_LONG " c[1-4] \\" ENDL
        "        --" ARG_SENDSIZE_LONG " 1m --" ARG_RECVSIZE_LONG " 1 -" ARG_NUMTHREADS_SHORT
            " 8 --" ARG_TIMELIMITSECS_LONG " 30" ENDL
        std::endl <<
        "  Simulate 4KiB reads with 16 rounds in flight per stream:" ENDL
        "    $ " EXE_NAME " --" ARG_NETBENCH_LONG " --" ARG_SERVERS_LONG " s1 "
            "--" ARG_CLIENTS_LONG " c1 \\" ENDL
        "        --" ARG_SENDSIZE_LONG " 1 --" ARG_RECVSIZE_LONG " 4k --" ARG_IODEPTH_LONG " 16 "
            "--" ARG_TIMELIMITSECS_LONG " 30" ENDL
        std::endl <<
        "  Send 10GiB per thread over RDMA in one direction only:" ENDL
        "    $ " EXE_NAME " --" ARG_NETBENCH_LONG " --" ARG_SERVERS_LONG " s1 "
            "--" ARG_CLIENTS_LONG " c1 --" ARG_NETENGINE_LONG " ibverbs \\" ENDL
        "        --" ARG_SENDSIZE_LONG " 4m --" ARG_RECVSIZE_LONG " 0 -" ARG_FILESIZE_SHORT
            " 10g -" ARG_NUMTHREADS_SHORT " 4" ENDL
        std::endl <<
        "  One server with 8 threads, 10 clients with 1 thread each, 4KiB ping-pong:" ENDL
        "    $ " EXE_NAME " --" ARG_NETBENCH_LONG " --" ARG_SERVERS_LONG " s1 "
            "--" ARG_CLIENTS_LONG " c[1-10] \\" ENDL
        "        --" ARG_TSERVERS_LONG " 8 --" ARG_TCLIENTS_LONG " 1 --" ARG_SENDSIZE_LONG " 4k "
            "--" ARG_RECVSIZE_LONG " 4k --" ARG_TIMELIMITSECS_LONG " 30" ENDL
        std::endl <<
        "  Services with a fixed interface and port range for the data endpoints:" ENDL
        "    $ " EXE_NAME " --" ARG_RUNASSERVICE_LONG " --" ARG_NETDEVS_LONG " eth1 "
            "--" ARG_NETPORT_LONG " 20000" ENDL
        std::endl <<
        "Details on the engines, interface selection, memory and stream placement are in" ENDL
        "docs/netbench.md." <<
        std::endl;
}

void NetBenchProgArgs::printBuildInfo(std::ostringstream& includedStream,
    std::ostringstream& notIncludedStream)
{
#ifdef LIBFABRIC_SUPPORT
    includedStream << FEATURE_NAME_LIBFABRIC << " ";
#else
    notIncludedStream << FEATURE_NAME_LIBFABRIC << " ";
#endif

#ifdef IBVERBS_SUPPORT
    includedStream << FEATURE_NAME_IBVERBS << " ";
#else
    notIncludedStream << FEATURE_NAME_IBVERBS << " ";
#endif

#ifdef RDMACM_SUPPORT
    includedStream << FEATURE_NAME_RDMACM << " ";
#else
    notIncludedStream << FEATURE_NAME_RDMACM << " ";
#endif
}

/**
 * @return libfabric header and runtime version for "--version"; empty without netbench support.
 */
std::string NetBenchProgArgs::getLibfabricVersionStr()
{
#ifdef LIBFABRIC_SUPPORT
    return LibfabricEndpoint::getVersionStr();
#else
    return "";
#endif
}

/**
 * Sets the netbench arguments for a new benchmark in service mode. The service-local options
 * (interfaces, fixed port) are not touched.
 */
void NetBenchProgArgs::setFromPropertyTree(const bpt::ptree& tree)
{
    netEngineStr = tree.get<std::string>(ARG_NETENGINE_LONG);
    netProv = tree.get<std::string>(ARG_NETPROV_LONG);
    numClients = tree.get<unsigned>(ARG_NUMNETBENCHCLIENTS_LONG);
    numServers = tree.get<unsigned>(ARG_NUMNETBENCHSERVERS_LONG);
    netRecvPct = tree.get<unsigned>(ARG_NETRECVPCT_LONG);
    netRnrTimer = tree.get<unsigned>(ARG_NETRNR_LONG);
    recvSize = tree.get<size_t>(ARG_RECVSIZE_LONG);
    runID = tree.get<std::string>(ARG_NETBENCHRUNID_LONG);
    serviceIndex = tree.get<size_t>(ARG_NETBENCHSVCIDX_LONG);
    threadsPerClient = tree.get<size_t>(ARG_TCLIENTS_LONG);
    threadsPerServer = tree.get<size_t>(ARG_TSERVERS_LONG);
    useNetBench = tree.get<bool>(ARG_NETBENCH_LONG);

    if(!netBenchEngineFromName(netEngineStr, netEngine) )
        throw ProgException("Unknown netbench engine: \"" + netEngineStr + "\"");

    serversVec.clear();
    TranslatorTk::splitAndExpandStr(tree.get<std::string>(ARG_NETBENCHSERVERSSTR_LONG),
        HOSTLIST_DELIMITERS, serversVec);
    TranslatorTk::eraseEmptyStringsFromVec(serversVec);

    runIfaces.clear();
    TranslatorTk::splitAndExpandStr(tree.get<std::string>(ARG_NETBENCHIFACES_LONG, ""),
        NETDEV_DELIMITERS, runIfaces);
    TranslatorTk::eraseEmptyStringsFromVec(runIfaces);

    NetBenchPlacement::parseStreams(tree.get<std::string>(ARG_NETBENCHSTREAMS_LONG, ""),
        runStreams);
}

/**
 * Gets the netbench arguments for a new benchmark in service mode.
 *
 * @param serviceRank position of the receiving service in the hosts list (servers first).
 */
void NetBenchProgArgs::getAsPropertyTree(const ProgArgs& progArgs, bpt::ptree& outTree,
    size_t serviceRank) const
{
    StringVec serversVecTmp;
    const StringVec& hostsVec = progArgs.getHostsVec();

    for(size_t i = 0; (i < numServers) && (i < hostsVec.size() ); i++)
        serversVecTmp.push_back(hostsVec[i] );

    outTree.put(ARG_NETBENCH_LONG, useNetBench);
    outTree.put(ARG_NETBENCHRUNID_LONG, runID);
    outTree.put(ARG_NETBENCHSERVERSSTR_LONG, TranslatorTk::stringVecToString(serversVecTmp, ",") );
    outTree.put(ARG_NETBENCHSVCIDX_LONG, serviceRank);
    outTree.put(ARG_NETENGINE_LONG, netBenchEngineName(netEngine) );
    outTree.put(ARG_NETPROV_LONG, netProv);
    outTree.put(ARG_NUMNETBENCHCLIENTS_LONG, numClients);
    outTree.put(ARG_NUMNETBENCHSERVERS_LONG, numServers);
    outTree.put(ARG_NETRECVPCT_LONG, netRecvPct);
    outTree.put(ARG_NETRNR_LONG, netRnrTimer);
    outTree.put(ARG_RECVSIZE_LONG, recvSize);
    outTree.put(ARG_TCLIENTS_LONG, threadsPerClient);
    outTree.put(ARG_TSERVERS_LONG, threadsPerServer);

    outTree.put(ARG_NETBENCHIFACES_LONG, placement.getIfacesStr(serviceRank) );
    outTree.put(ARG_NETBENCHSTREAMS_LONG, placement.getStreamsStr(serviceRank) );
}

/**
 * Reset the per-run values. Called by ProgArgs::resetBenchPath().
 */
void NetBenchProgArgs::reset()
{
    runID.clear();
    serversVec.clear();
    serviceIndex = 0;

    runIfaces.clear();
    runStreams.clear();
}

/**
 * Print the dry run info ("--dryrun"): bytes per client thread, per client and in total.
 *
 * Note: keep in sync with WorkerManager::getPhaseNumEntriesAndBytes().
 */
void NetBenchProgArgs::printDryRunInfo(const ProgArgs& progArgs) const
{
    const uint64_t numRounds = getNumRounds(progArgs);
    const size_t numClients = progArgs.getHostsVec().size() - numServers;

    auto printBytes = [](const char* label, uint64_t numBytes)
    {
        std::cout << label << numBytes << " | " << (numBytes / (1024*1024) ) << " MiB | " <<
            (numBytes / (1024*1024*1024) ) << " GiB" << std::endl;
    };

    const uint64_t sendPerThread = numRounds * progArgs.getBlockSize();
    const uint64_t recvPerThread = numRounds * recvSize;

    std::cout << "* Bytes per client thread:" << std::endl;
    printBytes("  * Send:   ", sendPerThread);
    printBytes("  * Recv:   ", recvPerThread);

    std::cout << "* Bytes per client service:" << std::endl;
    printBytes("  * Send:   ", threadsPerClient * sendPerThread);
    printBytes("  * Recv:   ", threadsPerClient * recvPerThread);

    std::cout << "* Bytes total for all clients:" << std::endl;
    printBytes("  * Send:   ", numClients * threadsPerClient * sendPerThread);
    printBytes("  * Recv:   ", numClients * threadsPerClient * recvPerThread);
}

/**
 * @return number of rounds per stream: file size divided by the send size, or by the receive
 *      size for a receive-only stream.
 */
uint64_t NetBenchProgArgs::getNumRounds(const ProgArgs& progArgs) const
{
    const size_t unitSize = progArgs.getBlockSize() ? progArgs.getBlockSize() : recvSize;

    return unitSize ? (progArgs.getFileSize() / unitSize) : 0;
}

/**
 * @return bytes on the wire per client thread, counting both directions.
 */
uint64_t NetBenchProgArgs::getExpectedBytesPerClientThread(const ProgArgs& progArgs) const
{
    return getNumRounds(progArgs) * (progArgs.getBlockSize() + recvSize);
}

/**
 * @return bytes on the wire per client service, counting both directions.
 */
uint64_t NetBenchProgArgs::getExpectedBytesPerClient(const ProgArgs& progArgs) const
{
    return threadsPerClient * getExpectedBytesPerClientThread(progArgs);
}
