// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef CONFIG_H_
#define CONFIG_H_

#include <cstddef>
#include <string>

/**
 * Command line settings of the server.
 */
struct Config
{
    std::string dataDir; // objects are stored as files below this dir
    std::string listenAddr = "127.0.0.1"; // HTTP listen address
    int port = 9000; // HTTP port
    std::string rdmaAddr; // IPv4 address of the RDMA NIC
    unsigned short rdmaPort = 18515; // cuObject server port (DC transport, nothing listens here)
    int rdmaGidIndex = -1; // GID table index for the RC transport, -1 = RoCE v2 GID of rdmaAddr
    size_t bufSize = 16 * 1024 * 1024; // size of each registered cuObject RDMA buffer
    unsigned numThreads = 8; // HTTP worker threads and cuObject RDMA channels
    bool cuObjEnabled = true; // cuObject (DC) transport for the x-amz-rdma-token protocol
    bool rcEnabled = true; // RC transport for the hipobj-rc-v2 protocol
    bool rdmaCheckOnly = false; // only try to open the RDMA transports, then exit

    bool isRdmaEnabled() const { return cuObjEnabled || rcEnabled; }
    bool verbose = false; // log every HTTP request and RDMA transfer
};

#endif // CONFIG_H_
