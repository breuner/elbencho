// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef HIPOBJV2SERVER_H_
#define HIPOBJV2SERVER_H_

#ifdef RC_SUPPORT

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "ObjectStore.h"
#include "RcDevice.h"

namespace httplib
{
    struct Request;
    struct Response;
}

/**
 * Server side of AMD hipObject's "hipobj-rc-v2" control protocol, the Reliable Connection
 * counterpart of cuObject's single-request protocol. A transfer takes two round trips on the
 * control endpoints below "/.hipobj-rc/", each a body-less POST with x-amz-rdma-* headers:
 *
 *   PREPARE: the client sends op, target, size, its token (queue pair, GID) and sequence number.
 *            The server creates a session with its own queue pair and a staging buffer and
 *            answers with a session id, its queue pair number, sequence number and, for a PUT,
 *            the staging buffer's address and key.
 *   READY:   both queue pairs get connected. For a PUT the client RDMA-writes the data into the
 *            staging buffer with the session cookie as immediate value; for a GET the server
 *            RDMA-writes the object data into the client buffer. The response reports the
 *            outcome ("FINAL").
 *   CANCEL:  drops a session.
 *
 * Signatures are not verified, like everywhere else in this server.
 */
class HipObjV2Server
{
    public:
        HipObjV2Server(RcDevice& device, ObjectStore& store);
        ~HipObjV2Server();

        void handlePrepare(const httplib::Request& req, httplib::Response& res);
        void handleReady(const httplib::Request& req, httplib::Response& res);
        void handleCancel(const httplib::Request& req, httplib::Response& res);

    private:
        struct Session
        {
            std::string id; // 32 hex chars
            std::string op; // "GET" or "PUT"
            std::string bucket;
            std::string key;
            uint64_t size = 0;
            uint64_t offset = 0;
            uint32_t cookie = 0;
            uint32_t clientPsn = 0;
            uint32_t serverPsn = 0;
            ibv_gid peerGid = {};
            int localGidIndex = 0; // our GID for this peer
            RcDevice::Queue queue;
            char* staging = nullptr; // holds the object data during the transfer
            ibv_mr* stagingMr = nullptr;
            std::chrono::steady_clock::time_point deadline; // for the READY to arrive
        };

        RcDevice& device;
        ObjectStore& store;
        std::mutex mutex; // for sessions
        std::map<std::string, std::unique_ptr<Session> > sessions; // by id

        std::unique_ptr<Session> createSession(const httplib::Request& req,
            httplib::Response& res);
        std::unique_ptr<Session> takeSession(const std::string& id);
        void destroySession(Session& session);
        void reapExpiredSessions();
        bool transfer(Session& session, uint64_t clientAddr, uint32_t clientKey,
            uint32_t clientQpNum, httplib::Response& res);
};

#endif // RC_SUPPORT

#endif // HIPOBJV2SERVER_H_
