// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <cctype>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <getopt.h>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

#include "Config.h"
#include "CuObjTransport.h"
#include "HipObjV2Server.h"
#include "Log.h"
#include "ObjectStore.h"
#include "RcDevice.h"
#include "RcTokenTransport.h"
#include "S3Server.h"

namespace
{
    S3Server* globalServer = nullptr; // for the signal handler

    void onSignal(int)
    {
        if(globalServer)
            globalServer->stop();
    }

    void printUsage()
    {
        std::cout <<
            EXE_NAME " - Minimal S3 server with S3-over-RDMA support for testing elbencho.\n"
            "\n"
            "Objects are stored as plain files below the given directory. Object data moves\n"
            "over RDMA when the client speaks the S3-over-RDMA protocol of NVIDIA cuObject\n"
            "(x-amz-rdma-token header; DC transport, or RC transport via the client shim in\n"
            "tools/s3rdma/cuobjclient-shim) or of AMD hipObject (hipobj-rc-v2 control requests, RC\n"
            "transport), otherwise in the HTTP body. Authentication is not checked.\n"
            "\n"
            "Usage:\n"
            "  " EXE_NAME " --dir DIR --rdma-addr ADDR [OPTIONS]\n"
            "  " EXE_NAME " --dir DIR --no-rdma [OPTIONS]\n"
            "  " EXE_NAME " --rdma-addr ADDR --rdma-check\n"
            "\n"
            "Options:\n"
            "  -d, --dir DIR         Directory to store the objects in. Gets created if it does\n"
            "                        not exist. (Required)\n"
            "  -a, --addr ADDR       HTTP listen address. (Default: 127.0.0.1)\n"
            "  -p, --port PORT       HTTP port. (Default: 9000)\n"
            "  -r, --rdma-addr ADDR  IPv4 address of the RDMA NIC for the data transfers.\n"
            "                        (Required unless --no-rdma is given)\n"
            "  -P, --rdma-port PORT  cuObject server port. (Default: 18515)\n"
            "  -g, --rdma-gid-index N  GID table index of the NIC for the RC transport.\n"
            "                        (Default: the RoCE v2 entry of --rdma-addr)\n"
            "  -b, --bufsize SIZE    Size of each RDMA staging buffer, one per thread. Larger\n"
            "                        transfers are done in chunks of this size. (Default: 16M)\n"
            "  -t, --threads NUM     Number of HTTP worker threads and RDMA staging buffers.\n"
            "                        (Default: 8)\n"
            "  -n, --no-rdma         Plain HTTP S3 server. Requests carrying an RDMA token are\n"
            "                        declined with x-amz-rdma-reply: 501.\n"
            "      --no-cuobj        Disable the cuObject (DC) transport only.\n"
            "      --no-rc           Disable the RC transport (client shim, hipobj-rc-v2) only.\n"
            "  -c, --rdma-check      Only check whether the enabled RDMA transports can be set\n"
            "                        up for the given --rdma-addr, then exit. Exit code 0 means\n"
            "                        success. Used by the elbencho test suite to skip tests on\n"
            "                        hosts without suitable RDMA hardware.\n"
            "  -v, --verbose         Log every request and every RDMA transfer.\n"
            "  -h, --help            Print this help.\n";
    }

    // "16M" => 16 MiB. Suffixes k/m/g (binary), case-insensitive.
    size_t parseSize(const std::string& str)
    {
        size_t numParsed = 0;
        const unsigned long long number = std::stoull(str, &numParsed);
        unsigned shift = 0;

        if(numParsed < str.size() )
        {
            if(numParsed + 1 != str.size() )
                throw std::invalid_argument("invalid size: " + str);

            switch(tolower(str[numParsed] ) )
            {
                case 'k': shift = 10; break;
                case 'm': shift = 20; break;
                case 'g': shift = 30; break;
                default: throw std::invalid_argument("invalid size suffix: " + str);
            }
        }

        return number << shift;
    }

    // @return false if the server should not start.
    bool parseArgs(int argc, char** argv, Config& config)
    {
        const struct option longOpts[] =
        {
            {"dir", required_argument, nullptr, 'd'},
            {"addr", required_argument, nullptr, 'a'},
            {"port", required_argument, nullptr, 'p'},
            {"rdma-addr", required_argument, nullptr, 'r'},
            {"rdma-port", required_argument, nullptr, 'P'},
            {"bufsize", required_argument, nullptr, 'b'},
            {"threads", required_argument, nullptr, 't'},
            {"rdma-gid-index", required_argument, nullptr, 'g'},
            {"no-rdma", no_argument, nullptr, 'n'},
            {"no-cuobj", no_argument, nullptr, 'C'},
            {"no-rc", no_argument, nullptr, 'R'},
            {"rdma-check", no_argument, nullptr, 'c'},
            {"verbose", no_argument, nullptr, 'v'},
            {"help", no_argument, nullptr, 'h'},
            {nullptr, 0, nullptr, 0}
        };

        int opt;
        while( (opt = getopt_long(argc, argv, "d:a:p:r:P:g:b:t:ncvh", longOpts, nullptr) ) != -1)
        {
            switch(opt)
            {
                case 'd': config.dataDir = optarg; break;
                case 'a': config.listenAddr = optarg; break;
                case 'p': config.port = std::stoi(optarg); break;
                case 'r': config.rdmaAddr = optarg; break;
                case 'P': config.rdmaPort = std::stoi(optarg); break;
                case 'b': config.bufSize = parseSize(optarg); break;
                case 't': config.numThreads = std::stoul(optarg); break;
                case 'g': config.rdmaGidIndex = std::stoi(optarg); break;
                case 'n': config.cuObjEnabled = config.rcEnabled = false; break;
                case 'C': config.cuObjEnabled = false; break;
                case 'R': config.rcEnabled = false; break;
                case 'c': config.rdmaCheckOnly = true; break;
                case 'v': config.verbose = true; break;
                case 'h': printUsage(); return false;
                default: printUsage(); throw std::invalid_argument("invalid arguments");
            }
        }

        if(optind < argc)
            throw std::invalid_argument(std::string("unexpected argument: ") + argv[optind] );

        if(config.rdmaCheckOnly)
        {
            if(!config.isRdmaEnabled() )
                throw std::invalid_argument("--rdma-check needs at least one RDMA transport");
        }
        else
        if(config.dataDir.empty() )
            throw std::invalid_argument("--dir is required");

        if(config.isRdmaEnabled() && config.rdmaAddr.empty() )
            throw std::invalid_argument("--rdma-addr is required (or use --no-rdma)");

        if(!config.numThreads || !config.bufSize)
            throw std::invalid_argument("--threads and --bufsize must not be zero");

        return true;
    }
}

int main(int argc, char** argv)
{
    Config config;

    try
    {
        if(!parseArgs(argc, argv, config) )
            return EXIT_SUCCESS;
    }
    catch(const std::exception& e)
    {
        std::cerr << "ERROR: " << e.what() << " (see --help)" << std::endl;
        return EXIT_FAILURE;
    }

    Log::verbose = config.verbose;

    try
    {
        std::unique_ptr<RdmaTransport> cuObj;
        RcTokenTransport* rcToken = nullptr;
        HipObjV2Server* hipObj = nullptr;
#ifdef RC_SUPPORT
        std::unique_ptr<RcDevice> rcDevice;
        std::unique_ptr<RcTokenTransport> rcTokenTransport;
        std::unique_ptr<HipObjV2Server> hipObjServer;
#endif

        if(config.cuObjEnabled)
        {
#ifdef CUOBJ_SUPPORT
            cuObj = std::make_unique<CuObjTransport>(config.rdmaAddr, config.rdmaPort,
                config.numThreads, config.bufSize);
#else
            throw std::runtime_error("This executable was built without cuObject support "
                "(CUOBJ_SUPPORT=0). Use --no-cuobj or --no-rdma.");
#endif
        }

        if(config.rcEnabled)
        {
#ifdef RC_SUPPORT
            rcDevice = std::make_unique<RcDevice>(config.rdmaAddr, config.rdmaGidIndex);
#else
            throw std::runtime_error("This executable was built without libibverbs support "
                "(RC_SUPPORT=0). Use --no-rc or --no-rdma.");
#endif
        }

        if(config.rdmaCheckOnly)
            return EXIT_SUCCESS; // (a failure would have thrown above)

        ObjectStore store(config.dataDir);

#ifdef RC_SUPPORT
        if(rcDevice)
        {
            rcTokenTransport = std::make_unique<RcTokenTransport>(*rcDevice, config.numThreads,
                config.bufSize);
            rcToken = rcTokenTransport.get();
            hipObjServer = std::make_unique<HipObjV2Server>(*rcDevice, store);
            hipObj = hipObjServer.get();
        }
#endif

        S3Server server(config, store, cuObj.get(), rcToken, hipObj);

        globalServer = &server;
        signal(SIGINT, onSignal);
        signal(SIGTERM, onSignal);
        signal(SIGPIPE, SIG_IGN);

        const bool runRes = server.run();

        globalServer = nullptr;
        Log::info("Server stopped.");

        return runRes ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    catch(const std::exception& e)
    {
        Log::error(e.what() );
        return EXIT_FAILURE;
    }
}
