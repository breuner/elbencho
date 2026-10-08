// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef RDMATRANSPORT_H_
#define RDMATRANSPORT_H_

#include <functional>
#include <string>
#include <sys/types.h>

#include "RdmaToken.h"

/**
 * Moves object data between a client buffer (described by an RdmaToken) and this server over
 * RDMA. The data passes through server-side buffers in chunks, so the object size is not limited
 * by the amount of registered memory. Implementations are thread-safe.
 *
 * This is the seam for the RDMA library in use: CuObjTransport implements it with NVIDIA's
 * libcuobjserver; a different library (e.g. a future server-side hipObject) is a second
 * implementation.
 */
class RdmaTransport
{
    public:
        typedef std::function<bool(const char* buf, size_t len)> Sink; // takes data from client
        typedef std::function<bool(char* buf, size_t len)> Source; // fills buffer for client

        virtual ~RdmaTransport() = default;

        /**
         * Fetch len bytes from the client buffer (RDMA READ, i.e. an S3 PUT) and pass them to
         * sink, chunk by chunk.
         *
         * @return number of bytes transferred, or a negative errno on failure.
         */
        virtual ssize_t readFromClient(const std::string& key, const RdmaToken& token, size_t len,
            const Sink& sink) = 0;

        /**
         * Push len bytes into the client buffer (RDMA WRITE, i.e. an S3 GET), chunk by chunk
         * filled by source.
         *
         * @return number of bytes transferred, or a negative errno on failure.
         */
        virtual ssize_t writeToClient(const std::string& key, const RdmaToken& token, size_t len,
            const Source& source) = 0;
};

#endif // RDMATRANSPORT_H_
