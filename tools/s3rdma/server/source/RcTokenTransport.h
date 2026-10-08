// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef RCTOKENTRANSPORT_H_
#define RCTOKENTRANSPORT_H_

#ifdef RC_SUPPORT

#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "RcDevice.h"
#include "RdmaTransport.h"

namespace httplib
{
    struct Request;
    struct Response;
}

/**
 * RdmaTransport over Reliable Connections for the cuObject-style protocol, the server side of the
 * RC client shim (../cuobjclient-shim). cuObject's single request protocol expects the server to
 * move the data while it handles the object request, which an RC queue pair can only do when it
 * is connected to the client's queue pair already. So the client connects beforehand:
 *
 *   POST /.s3rdma-rc/connect with its hipObject-style RC token (queue pair, GID) and sequence
 *   number; the server creates a queue pair, connects it to the client's and answers with a
 *   connection id, its own token and sequence number. The client's token for object requests
 *   then names that connection: "rc:<connection id>:<address>:<size>:<rkey>", and the server
 *   RDMA-reads (PUT) or RDMA-writes (GET) through it. POST /.s3rdma-rc/disconnect drops it.
 *
 * Transfers pass through a pool of registered staging buffers in chunks, like CuObjTransport.
 */
class RcTokenTransport : public RdmaTransport
{
    public:
        RcTokenTransport(RcDevice& device, unsigned numBuffers, size_t bufSize);
        virtual ~RcTokenTransport();

        void handleConnect(const httplib::Request& req, httplib::Response& res);
        void handleDisconnect(const httplib::Request& req, httplib::Response& res);

        virtual ssize_t readFromClient(const std::string& key, const RdmaToken& token, size_t len,
            const Sink& sink) override;
        virtual ssize_t writeToClient(const std::string& key, const RdmaToken& token, size_t len,
            const Source& source) override;

    private:
        struct Connection
        {
            RcDevice::Queue queue;
            std::chrono::steady_clock::time_point lastUse;
            bool busy = false; // a transfer is running on it
        };

        struct Buffer
        {
            char* ptr = nullptr;
            ibv_mr* mr = nullptr;
        };

        RcDevice& device;
        size_t bufSize;
        std::vector<Buffer> buffers; // the staging buffers
        std::vector<Buffer*> freeBuffers; // protected by mutex
        std::map<std::string, std::unique_ptr<Connection> > connections; // by id, protected by mutex
        std::mutex mutex;
        std::condition_variable freeBuffersCond;

        Buffer* acquireBuffer();
        void releaseBuffer(Buffer* buffer);
        Connection* acquireConnection(const std::string& id);
        void releaseConnection(Connection* connection);
        void reapIdleConnections();
        ssize_t transfer(const RdmaToken& token, size_t len, bool isRead,
            const std::function<bool(char* buf, size_t len)>& chunkFn);
};

#endif // RC_SUPPORT

#endif // RCTOKENTRANSPORT_H_
