// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "Common.h"
#include "modes/mpiio/MpiIoLauncher.h"
#include "modes/mpiio/MpiIoProgArgs.h"
#include "modes/netbench/NetBenchEngine.h" // (the NetBenchIP address helpers)
#include "ProgArgs.h"

#ifdef MPI_SUPPORT

#define MPIIO_ADDR_MAXLEN       256 // fixed length of a "host:port" in the address gather
#define MPIIO_COORDADDRS_MAXLEN 4096 // fixed length of the coordinator's address list

namespace
{
    bool active = false; // MPI initialized by us
    bool coordinator = false; // rank 0 of a multi-rank run without "--service"
    bool autoService = false; // this rank was turned into a service by init()
    bool addrsGathered = false; // the address gather of this rank is done
    bool barrierTaken = false; // the final barrier of this rank is done or a thread waits in it
    std::vector<std::string> argStrs; // storage for the modified argv
    std::vector<char*> argPtrs;
    std::thread waiterThread; // auto service: waits in the final barrier, then stops the server
    std::vector<std::string> coordAddrs; // auto service: the coordinator rank's IPv4 addresses
    MPI_Comm ioComm = MPI_COMM_NULL;

    bool isLoopbackStr(const std::string& addr)
    {
        return !addr.rfind("127.", 0) || (addr == "::1");
    }

    /**
     * @return the IPv4 addresses of this host without loopback, comma-separated.
     */
    std::string ownIPv4Addrs()
    {
        NetBenchIfaceVec ifaces;
        NetBenchIP::expandSubnet("0.0.0.0/0", true, ifaces);

        std::string result;

        for(const NetBenchIface& iface : ifaces)
            if(!isLoopbackStr(iface.addr) )
                result += (result.empty() ? "" : ",") + iface.addr;

        return result;
    }

    /**
     * The address that a service announces to the coordinator rank: the local address that the
     * kernel uses to reach one of the coordinator's addresses, preferring one in the same subnet
     * on another host (a same-host match can be a private subnet that exists on every host, like
     * docker's), then any same-subnet match (all ranks on one host), then any route. Host names
     * are the last resort, because they often do not resolve across cluster nodes.
     */
    std::string pickServiceAddr()
    {
        if(coordAddrs.empty() )
            return "127.0.0.1"; // (no global address on rank 0, so all ranks are on its host)

        for(int pass = 0; pass < 3; pass++)
        {
            for(const std::string& coordAddr : coordAddrs)
            {
                sockaddr_storage peer, src;

                if(!NetBenchIP::parseIP(coordAddr, peer) )
                    continue;

                if(!NetBenchIP::routeSourceAddr( (const sockaddr*)&peer,
                    NetBenchIP::sockAddrLen( (const sockaddr*)&peer), src) )
                    continue;

                const std::string srcStr = NetBenchAddr::fromSockAddr( (const sockaddr*)&src,
                    NetBenchIP::sockAddrLen( (const sockaddr*)&src) ).ipToStr();

                if(isLoopbackStr(srcStr) )
                    continue;

                const bool sameHost = NetBenchIP::sameIP( (const sockaddr*)&peer,
                    (const sockaddr*)&src);
                const bool sameSubnet = NetBenchIP::subnetContains(
                    NetBenchIP::ifaceSubnet( (const sockaddr*)&src, 0), (const sockaddr*)&peer);

                if( ( (pass == 0) && sameSubnet && !sameHost) || ( (pass == 1) && sameSubnet) ||
                    (pass == 2) )
                    return srcStr;
            }
        }

        char hostname[MPIIO_ADDR_MAXLEN] = {};
        gethostname(hostname, sizeof(hostname) - 1);

        return hostname;
    }

    /**
     * Collective over MPI_COMM_WORLD: every rank contributes its own address (empty for rank 0
     * or for a failed service), rank 0 receives them all.
     */
    void gatherAddrs(const std::string& ownAddr, std::vector<std::string>* outAddrs)
    {
        char sendBuf[MPIIO_ADDR_MAXLEN] = {};
        strncpy(sendBuf, ownAddr.c_str(), MPIIO_ADDR_MAXLEN - 1);

        int worldSize;
        MPI_Comm_size(MPI_COMM_WORLD, &worldSize);

        std::vector<char> recvBuf(outAddrs ? (worldSize * MPIIO_ADDR_MAXLEN) : 0);

        MPI_Gather(sendBuf, MPIIO_ADDR_MAXLEN, MPI_CHAR, recvBuf.data(), MPIIO_ADDR_MAXLEN,
            MPI_CHAR, 0, MPI_COMM_WORLD);

        addrsGathered = true;

        if(!outAddrs)
            return;

        for(int i=0; i < worldSize; i++)
            outAddrs->push_back(std::string(&recvBuf[i * MPIIO_ADDR_MAXLEN] ) );
    }

    bool hasArg(int argc, char** argv, const std::string& longName)
    {
        const std::string arg = "--" + longName;

        for(int i=1; i < argc; i++)
            if( (arg == argv[i]) || !strncmp(argv[i], (arg + "=").c_str(), arg.size() + 1) )
                return true;

        return false;
    }
}

#endif // MPI_SUPPORT

/**
 * Initialize MPI if "--mpiio" is on the command line and set the role of this rank. Rank 0 of a
 * multi-rank run gets the services' addresses appended as "--hosts", the other ranks get the
 * service options appended.
 *
 * @argc/argv replaced by the modified command line where applicable.
 * @throw ProgException on MPI errors, a missing MPI_THREAD_MULTIPLE or a failed service rank.
 */
void MpiIoLauncher::init(int& argc, char**& argv)
{
#ifdef MPI_SUPPORT

    if(!hasArg(argc, argv, ARG_MPIIO_LONG) )
        return;

    int provided;
    int initRes = MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    if(initRes != MPI_SUCCESS)
        throw ProgException("MPI initialization failed. (MPI_Init_thread)");

    active = true;

    if(provided < MPI_THREAD_MULTIPLE)
        throw ProgException("The MPI library does not provide MPI_THREAD_MULTIPLE, which MPI-IO "
            "mode needs for its worker threads.");

    int worldSize, worldRank;
    MPI_Comm_size(MPI_COMM_WORLD, &worldSize);
    MPI_Comm_rank(MPI_COMM_WORLD, &worldRank);

    if( (worldSize == 1) || hasArg(argc, argv, ARG_RUNASSERVICE_LONG) )
    { // standalone or persistent service: all ranks do I/O
        ioComm = MPI_COMM_WORLD;
        return;
    }

    // multi-rank run: rank 0 coordinates, all other ranks become services
    // (comm split first, then address gather, in this order on every rank)

    MPI_Comm_split(MPI_COMM_WORLD, worldRank ? 1 : MPI_UNDEFINED, worldRank, &ioComm);

    // the coordinator's addresses let the services pick the interface to announce
    char coordAddrsBuf[MPIIO_COORDADDRS_MAXLEN] = {};

    if(worldRank == 0)
        strncpy(coordAddrsBuf, ownIPv4Addrs().c_str(), sizeof(coordAddrsBuf) - 1);

    MPI_Bcast(coordAddrsBuf, MPIIO_COORDADDRS_MAXLEN, MPI_CHAR, 0, MPI_COMM_WORLD);

    for(char* tok = strtok(coordAddrsBuf, ","); tok; tok = strtok(NULL, ",") )
        coordAddrs.push_back(tok);

    argStrs.assign(argv, argv + argc);

    if(worldRank == 0)
    {
        coordinator = true;

        if(hasArg(argc, argv, ARG_HOSTS_LONG) || hasArg(argc, argv, ARG_HOSTSFILE_LONG) )
            throw ProgException("\"--" ARG_HOSTS_LONG "\" cannot be given under mpirun in MPI-IO "
                "mode, because the service ranks are assigned automatically. For persistent "
                "services, start them with \"--" ARG_RUNASSERVICE_LONG " --" ARG_MPIIO_LONG
                "\" via mpirun and run the coordinator without mpirun.");

        std::vector<std::string> addrs;
        gatherAddrs("", &addrs);

        std::string hosts;

        for(int i=1; i < worldSize; i++)
        {
            if(addrs[i].empty() )
                throw ProgException("MPI rank " + std::to_string(i) + " failed to start its "
                    "elbencho service. See its error message above.");

            hosts += ( (i > 1) ? "," : "") + addrs[i];
        }

        argStrs.push_back("--" ARG_HOSTS_LONG);
        argStrs.push_back(hosts);

        // (host and pid, so that the user can interrupt this rank for a result table, see docs)
        char hostname[MPIIO_ADDR_MAXLEN] = {};
        gethostname(hostname, sizeof(hostname) - 1);

        std::cout << "MPI-IO: coordinator rank + " << (worldSize - 1) << " service rank" <<
            ( (worldSize > 2) ? "s" : "") << " (coordinator: " << hostname << ", pid " <<
            getpid() << ")" << std::endl;
    }
    else
    {
        autoService = true;

        argStrs.push_back("--" ARG_RUNASSERVICE_LONG);
        argStrs.push_back("--" ARG_FOREGROUNDSERVICE_LONG);
        argStrs.push_back("--" ARG_SERVICEPORT_LONG);
        argStrs.push_back("0");

        /* the services print their phase results and run notes, which would appear once per
            rank in the merged output of mpirun; errors go to stderr and to the coordinator */
        if(!freopen("/dev/null", "w", stdout) )
            std::cerr << "WARNING: Failed to silence stdout of MPI service rank " << worldRank <<
                std::endl;
    }

    for(std::string& arg : argStrs)
        argPtrs.push_back(&arg[0] );

    argPtrs.push_back(nullptr);

    argc = argPtrs.size() - 1;
    argv = argPtrs.data();

#endif // MPI_SUPPORT
}

/**
 * Called by the HTTP service when it listens: an auto service sends its address to rank 0 and
 * starts the thread that waits for the coordinator's final barrier to stop the server.
 *
 * @stopServer stops the HTTP server; called from another thread.
 */
void MpiIoLauncher::serviceListening(unsigned short port, std::function<void()> stopServer)
{
#ifdef MPI_SUPPORT

    if(!autoService)
        return;

    gatherAddrs(pickServiceAddr() + ":" + std::to_string(port), nullptr);

    barrierTaken = true;

    waiterThread = std::thread([stopServer]()
    {
        /* a non-blocking barrier polled at a low rate instead of MPI_Barrier: a thread that blocks
            inside MPI spins in the progress engine for the whole run, which burns a core per
            service and, with an MPI library that serializes the MPI calls of a process behind one
            lock (MPICH's ch3 device), takes that lock away from the worker threads' MPI-IO calls.
            the poll interval only delays the exit of the services after the coordinator is done */
        MPI_Request request;
        int done = 0;

        MPI_Ibarrier(MPI_COMM_WORLD, &request);

        for( ; ; )
        {
            MPI_Test(&request, &done, MPI_STATUS_IGNORE);

            if(done)
                break;

            std::this_thread::sleep_for(std::chrono::seconds(1) );
        }

        stopServer();
    } );

#endif // MPI_SUPPORT
}

/**
 * Called by the HTTP service after its server stopped, so that the stop callback has completed
 * before the server object goes away.
 */
void MpiIoLauncher::joinServiceWaiter()
{
#ifdef MPI_SUPPORT
    if(waiterThread.joinable() )
        waiterThread.join();
#endif // MPI_SUPPORT
}

/**
 * Release the other ranks and finalize MPI. Also on error paths: whatever collective this rank
 * still owes gets done here, so that no other rank waits forever.
 */
void MpiIoLauncher::finish()
{
#ifdef MPI_SUPPORT

    if(!active)
        return;

    if( (coordinator || autoService) && !addrsGathered)
    { // failed before the address exchange
        std::vector<std::string> addrs;
        gatherAddrs("", coordinator ? &addrs : nullptr);
    }

    if( (coordinator || autoService) && !barrierTaken)
    { // (non-blocking form, because only that matches the service waiter thread's MPI_Ibarrier)
        MPI_Request request;

        MPI_Ibarrier(MPI_COMM_WORLD, &request);
        MPI_Wait(&request, MPI_STATUS_IGNORE);
    }

    joinServiceWaiter();

    MPI_Finalize();

    active = false;

#endif // MPI_SUPPORT
}

bool MpiIoLauncher::isActive()
{
#ifdef MPI_SUPPORT
    return active;
#else
    return false;
#endif
}

/**
 * @return true if init() turned this rank into a service under mpirun.
 */
bool MpiIoLauncher::isAutoService()
{
#ifdef MPI_SUPPORT
    return autoService;
#else
    return false;
#endif
}

#ifdef MPI_SUPPORT
/**
 * @return the communicator of the I/O ranks (all ranks that run worker threads).
 */
MPI_Comm MpiIoLauncher::getIoComm()
{
    return ioComm;
}
#endif // MPI_SUPPORT
