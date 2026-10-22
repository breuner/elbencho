// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <algorithm>
#include <client_http.hpp>
#include <numeric>
#include <sstream>
#include <tuple>

#include "Logger.h"
#include "modes/netbench/NetBenchPlacement.h"
#include "modes/netbench/NetBenchProgArgs.h"
#include "modes/netbench/NetBenchProtocol.h"
#include "ProgArgs.h"
#include "ProgException.h"

#define PLACEMENT_ITEM_DELIMITER        ','
#define PLACEMENT_FIELD_DELIMITER       '/' // (not part of IP addresses or device names)


namespace
{
    std::string subnetsStr(const NetBenchIfaceVec& ifaces)
    {
        std::string result;

        for(const NetBenchIface& iface : ifaces)
            result += (result.empty() ? "" : ", ") + iface.addr +
                (iface.subnet.empty() ? "" : " (" + iface.subnet + ")");

        return result.empty() ? "none" : result;
    }

    void readIfaces(const bpt::ptree& tree, const char* key, NetBenchIfaceVec& out)
    {
        const auto listIter = tree.find(key);
        if(listIter == tree.not_found() )
            return;

        for(const auto& item : listIter->second)
            out.push_back( { item.second.get<std::string>(NETBENCH_JSON_ADDR),
                item.second.get<std::string>(NETBENCH_JSON_SUBNET, "") } );
    }
} // anonymous namespace


/**
 * Ask every service for its current interfaces: servers for their server role, clients for
 * their client role.
 *
 * @throw ProgException if a service is unreachable or too old.
 */
void NetBenchPlacement::query(const ProgArgs& progArgs)
{
    const StringVec& hosts = progArgs.getHostsVec();
    const NetBenchProgArgs& netBenchArgs = progArgs.getNetBenchArgs();

    hostIfaces.assign(hosts.size(), NetBenchIfaceVec() );

    for(size_t hostIdx = 0; hostIdx < hosts.size(); hostIdx++)
    {
        std::string host, port;
        NetBenchIP::splitHostPort(hosts[hostIdx], host, port);

        const std::string requestPath = HTTPCLIENTPATH_NETBENCHIFACES "?"
            XFER_PREP_PROTCOLVERSION "=" HTTP_PROTOCOLVERSION "&"
            XFER_PREP_AUTHORIZATION "=" + progArgs.getSvcPasswordHash() + "&"
            NETBENCH_QUERY_HOST "=" + host + "&"
            NETBENCH_QUERY_DEVSUBNETS "=" + (netBenchArgs.getNetDevSubnets() ? "1" : "0");

        try
        {
            SimpleWeb::Client<SimpleWeb::HTTP> httpClient(hosts[hostIdx] );
            httpClient.config.timeout = NETBENCH_QUERY_TIMEOUT_SEC;
            httpClient.config.timeout_connect = NETBENCH_QUERY_TIMEOUT_SEC;

            auto response = httpClient.request("GET", requestPath);
            const std::string statusCode = response->status_code;

            if(!statusCode.rfind("404", 0) )
                throw ProgException("The service does not support the netbench interface query. "
                    "Service version too old?");

            if(statusCode.rfind("200", 0) )
                throw ProgException(response->content.string() );

            bpt::ptree tree;
            bpt::read_json(response->content, tree);

            readIfaces(tree, netBenchArgs.isServerService(hostIdx) ?
                NETBENCH_JSON_SERVERIFACES : NETBENCH_JSON_IFACES, hostIfaces[hostIdx] );
        }
        catch(std::exception& e)
        {
            throw ProgException("Netbench interface query failed. Service: " + hosts[hostIdx] +
                "; Error: " + e.what() );
        }

        LOGGER(Log_VERBOSE, "Netbench interfaces of " << hosts[hostIdx] <<
            netBenchArgs.getServiceRoleSuffix(hostIdx) << ": " <<
            subnetsStr(hostIfaces[hostIdx] ) << std::endl);
    }
}

/**
 * Place the client streams (call after query() ). Clients with fewer reachable servers are placed
 * first, so that their constraints do not leave a server short of streams that the other clients
 * could have filled. Every stream takes the least loaded candidate: server first, then same
 * subnet (only relevant with cross-subnet streams allowed), then the server's interface, then
 * the client's interface; ties go to the lowest index.
 *
 * @throw ProgException if a client cannot reach any server or a server cannot be reached by any
 *      client.
 */
void NetBenchPlacement::compute(const StringVec& hosts, unsigned numServers,
    size_t threadsPerClient, size_t threadsPerServer, bool allowCrossSubnet)
{
    this->numServers = numServers;
    this->threadsPerClient = threadsPerClient;

    const size_t numClients = hosts.size() - numServers;

    streams.assign(numClients * threadsPerClient, NetBenchStreamPlace() );
    streamsPerServer.assign(numServers, 0);

    std::vector<std::vector<size_t>> streamsPerServerIface(numServers);
    std::vector<std::vector<size_t>> streamsPerClientIface(numClients);
    std::vector<bool> serverReachable(numServers, false);

    // interfaces with a partner (so an unused interface without one is not an imbalance)
    std::vector<std::vector<bool>> ifaceUsable(hosts.size() );

    for(size_t hostIdx = 0; hostIdx < hosts.size(); hostIdx++)
        ifaceUsable[hostIdx].assign(hostIfaces[hostIdx].size(), false);

    for(size_t s = 0; s < numServers; s++)
        streamsPerServerIface[s].assign(hostIfaces[s].size(), 0);

    struct Candidate
    {
        size_t s, si, ci;
        bool sameSubnet;
    };

    std::vector<std::vector<Candidate>> candidates(numClients);
    std::vector<size_t> numReachable(numClients, 0);

    NetBenchIfaceVec allServerIfaces; // for error messages
    for(size_t s = 0; s < numServers; s++)
        allServerIfaces.insert(allServerIfaces.end(), hostIfaces[s].begin(), hostIfaces[s].end() );

    for(size_t c = 0; c < numClients; c++)
    {
        const NetBenchIfaceVec& clientIfaces = hostIfaces[numServers + c];
        streamsPerClientIface[c].assign(clientIfaces.size(), 0);

        for(size_t s = 0; s < numServers; s++)
        {
            bool reachable = false;

            for(size_t si = 0; si < hostIfaces[s].size(); si++)
                for(size_t ci = 0; ci < clientIfaces.size(); ci++)
                {
                    const bool sameSubnet = NetBenchIP::ifaceSubnetMatch(clientIfaces[ci],
                        hostIfaces[s][si] );

                    if(!NetBenchIP::ifaceFamilyMatch(clientIfaces[ci], hostIfaces[s][si] ) ||
                        (!sameSubnet && !allowCrossSubnet) )
                        continue;

                    candidates[c].push_back( { s, si, ci, sameSubnet } );
                    reachable = true;
                    ifaceUsable[s][si] = true;
                    ifaceUsable[numServers + c][ci] = true;
                }

            numReachable[c] += reachable;
            serverReachable[s] = serverReachable[s] || reachable;
        }

        if(candidates[c].empty() && threadsPerClient)
            throw ProgException("Client " + hosts[numServers + c] + " has no interface in a "
                "subnet of any server. Client interfaces: " + subnetsStr(clientIfaces) + "; "
                "server interfaces: " + subnetsStr(allServerIfaces) + ". Use \"--"
                ARG_NETCROSSSUBNET_LONG "\" to allow streams across subnets (an address family "
                "mismatch cannot be allowed).");
    }

    for(size_t s = 0; s < numServers; s++)
        if(!serverReachable[s] && numClients && threadsPerClient)
            throw ProgException("Server " + hosts[s] + " shares no subnet with any client. Server "
                "interfaces: " + subnetsStr(hostIfaces[s] ) + ". Use \"--" ARG_NETCROSSSUBNET_LONG
                "\" to allow streams across subnets.");

    std::vector<size_t> clientOrder(numClients);
    std::iota(clientOrder.begin(), clientOrder.end(), 0);
    std::stable_sort(clientOrder.begin(), clientOrder.end(),
        [&](size_t a, size_t b) { return numReachable[a] < numReachable[b]; } );

    for(size_t c : clientOrder)
        for(size_t localRank = 0; localRank < threadsPerClient; localRank++)
        {
            const Candidate* best = NULL;
            auto loadOf = [&](const Candidate& cand)
            {
                return std::make_tuple(streamsPerServer[cand.s], cand.sameSubnet ? 0 : 1,
                    streamsPerServerIface[cand.s][cand.si], streamsPerClientIface[c][cand.ci],
                    cand.s, cand.si, cand.ci);
            };

            for(const Candidate& cand : candidates[c] )
                if(!best || (loadOf(cand) < loadOf(*best) ) )
                    best = &cand;

            NetBenchStreamPlace& place = streams[(c * threadsPerClient) + localRank];
            place.clientRank = (c * threadsPerClient) + localRank;
            place.serverIdx = best->s;
            place.serverIfaceIdx = best->si;
            place.serverWorkerIdx = streamsPerServer[best->s] % threadsPerServer;
            place.serverIfaceAddr = hostIfaces[best->s][best->si].addr;
            place.clientAddr = hostIfaces[numServers + c][best->ci].addr;

            streamsPerServer[best->s]++;
            streamsPerServerIface[best->s][best->si]++;
            streamsPerClientIface[c][best->ci]++;

            LOGGER(Log_DEBUG, "Netbench stream placement: rank " << place.clientRank << ": " <<
                hosts[numServers + c] << " via " << place.clientAddr << " -> " <<
                hosts[best->s] << " via " << place.serverIfaceAddr << " worker " <<
                place.serverWorkerIdx << std::endl);
        }

    logUneven(hosts, ifaceUsable, streamsPerServerIface, streamsPerClientIface);
}

/**
 * Print a NOTE for every host group whose stream counts differ by more than one (which a plain
 * round-robin never does), so that lower throughput of a server or interface is not mistaken for
 * a network problem.
 */
void NetBenchPlacement::logUneven(const StringVec& hosts,
    const std::vector<std::vector<bool>>& ifaceUsable,
    const std::vector<std::vector<size_t>>& streamsPerServerIface,
    const std::vector<std::vector<size_t>>& streamsPerClientIface) const
{
    auto noteIfUneven = [](const std::string& what, const StringVec& names,
        const std::vector<size_t>& counts)
    {
        if(counts.empty() )
            return;

        const auto minMax = std::minmax_element(counts.begin(), counts.end() );
        if( (*minMax.second - *minMax.first) <= 1)
            return;

        std::string countsStr;
        for(size_t i = 0; i < counts.size(); i++)
            countsStr += (i ? ", " : "") + names[i] + "=" + std::to_string(counts[i] );

        LOGGER(Log_NORMAL, "NOTE: Uneven netbench stream placement due to subnet constraints. "
            "Streams per " << what << ": " << countsStr << std::endl);
    };

    noteIfUneven("server", StringVec(hosts.begin(), hosts.begin() + numServers),
        streamsPerServer);

    for(size_t hostIdx = 0; hostIdx < hosts.size(); hostIdx++)
    {
        const std::vector<size_t>& allCounts = (hostIdx < numServers) ?
            streamsPerServerIface[hostIdx] : streamsPerClientIface[hostIdx - numServers];
        StringVec addrs;
        std::vector<size_t> counts;

        for(size_t i = 0; i < allCounts.size(); i++)
            if(ifaceUsable[hostIdx][i] )
            {
                addrs.push_back(hostIfaces[hostIdx][i].addr);
                counts.push_back(allCounts[i] );
            }

        noteIfUneven("interface of " + hosts[hostIdx], addrs, counts);
    }
}

/**
 * @return comma-separated interface addresses of a host, in the order the stream places refer to.
 */
std::string NetBenchPlacement::getIfacesStr(size_t hostIdx) const
{
    std::string result;

    if(hostIdx < hostIfaces.size() )
        for(const NetBenchIface& iface : hostIfaces[hostIdx] )
            result += (result.empty() ? "" : ",") + iface.addr;

    return result;
}

/**
 * @return the streams of a host (a server's streams or a client's threads in rank order) in the
 *      format of parseStreams().
 */
std::string NetBenchPlacement::getStreamsStr(size_t hostIdx) const
{
    std::string result;

    for(const NetBenchStreamPlace& place : streams)
    {
        const bool isOwn = (hostIdx < numServers) ? (place.serverIdx == hostIdx) :
            ( (place.clientRank / threadsPerClient) == (hostIdx - numServers) );

        if(!isOwn)
            continue;

        std::ostringstream item;
        item << place.clientRank << PLACEMENT_FIELD_DELIMITER << place.serverIdx <<
            PLACEMENT_FIELD_DELIMITER << place.serverIfaceIdx << PLACEMENT_FIELD_DELIMITER <<
            place.serverWorkerIdx << PLACEMENT_FIELD_DELIMITER << place.serverIfaceAddr <<
            PLACEMENT_FIELD_DELIMITER << place.clientAddr;

        result += (result.empty() ? "" : ",") + item.str();
    }

    return result;
}

size_t NetBenchPlacement::getMaxStreamsPerServer() const
{
    return streamsPerServer.empty() ? 0 :
        *std::max_element(streamsPerServer.begin(), streamsPerServer.end() );
}

size_t NetBenchPlacement::getMaxServerIfaces() const
{
    size_t maxIfaces = 0;

    for(size_t s = 0; (s < numServers) && (s < hostIfaces.size() ); s++)
        maxIfaces = std::max(maxIfaces, hostIfaces[s].size() );

    return maxIfaces;
}

/**
 * Parse the result of getStreamsStr() (on the services).
 *
 * @throw ProgException on malformed input.
 */
void NetBenchPlacement::parseStreams(const std::string& str, NetBenchStreamPlaceVec& out)
{
    std::stringstream itemsStream(str);
    std::string item;

    out.clear();

    while(std::getline(itemsStream, item, PLACEMENT_ITEM_DELIMITER) )
    {
        StringVec fields;
        std::stringstream itemStream(item);
        std::string field;

        while(std::getline(itemStream, field, PLACEMENT_FIELD_DELIMITER) )
            fields.push_back(field);

        if(fields.size() != 6)
            throw ProgException("Malformed netbench stream placement: " + item);

        NetBenchStreamPlace place;
        place.clientRank = std::stoul(fields[0] );
        place.serverIdx = std::stoul(fields[1] );
        place.serverIfaceIdx = std::stoul(fields[2] );
        place.serverWorkerIdx = std::stoul(fields[3] );
        place.serverIfaceAddr = fields[4];
        place.clientAddr = fields[5];

        out.push_back(place);
    }
}
