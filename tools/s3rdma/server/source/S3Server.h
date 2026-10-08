// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef S3SERVER_H_
#define S3SERVER_H_

#include <memory>
#include <string>

#include "Config.h"
#include "ObjectStore.h"
#include "RdmaTransport.h"

class HipObjV2Server;
class RcTokenTransport;

namespace httplib
{
    class Server;
    struct Request;
    struct Response;
    class ContentReader;
}

/**
 * The HTTP side: a small subset of the S3 API (buckets, objects, multipart uploads, listing) on
 * top of ObjectStore. Object data travels in the HTTP body as usual, unless the request carries
 * an "x-amz-rdma-token" header: then it moves via RdmaTransport and the HTTP request/response is
 * the body-less control message of the S3-over-RDMA protocol (NVIDIA cuObject). The token
 * selects the transport: cuObject's DC descriptor goes to the cuObject transport, the RC client
 * shim's token to RcTokenTransport. The control requests of AMD hipObject's protocol are
 * forwarded to HipObjV2Server.
 *
 * Authentication is not checked (this is a test server) and only path-style addressing is
 * supported.
 */
class S3Server
{
    public:
        S3Server(const Config& config, ObjectStore& store, RdmaTransport* cuObj,
            RcTokenTransport* rcToken, HipObjV2Server* hipObj);
        ~S3Server();

        bool run();
        void stop();

    private:
        const Config& config;
        ObjectStore& store;
        RdmaTransport* cuObj; // nullptr => cuObject RDMA requests get declined
        RcTokenTransport* rcToken; // nullptr => RC shim requests get declined
        HipObjV2Server* hipObj; // nullptr => hipobj-rc-v2 requests get declined
        std::unique_ptr<httplib::Server> svr;

        void handleGet(const httplib::Request& req, httplib::Response& res);
        void handlePut(const httplib::Request& req, httplib::Response& res,
            const httplib::ContentReader& reader);
        void handlePost(const httplib::Request& req, httplib::Response& res,
            const httplib::ContentReader& reader);
        void handleDelete(const httplib::Request& req, httplib::Response& res);

        void listObjects(const httplib::Request& req, httplib::Response& res,
            const std::string& bucket);
        void httpGet(httplib::Response& res, const std::string& bucket, const std::string& key);
        void rdmaGet(const httplib::Request& req, httplib::Response& res,
            const std::string& bucket, const std::string& key);
        void httpPut(httplib::Response& res, const httplib::ContentReader& reader,
            const std::string& bucket, const std::string& key, const std::string& uploadID,
            unsigned partNumber);
        void rdmaPut(const httplib::Request& req, httplib::Response& res,
            const std::string& bucket, const std::string& key, const std::string& uploadID,
            unsigned partNumber);
        void deleteObjects(httplib::Response& res, const httplib::ContentReader& reader,
            const std::string& bucket);

        ObjectStore::Result openForWrite(const std::string& bucket, const std::string& key,
            const std::string& uploadID, unsigned partNumber, std::ofstream& outStream);
        ObjectStore::Result statWritten(const std::string& bucket, const std::string& key,
            const std::string& uploadID, unsigned partNumber, ObjectInfo& outInfo);

        static void sendError(httplib::Response& res, int status, const std::string& code,
            const std::string& message, const std::string& resource);
        static bool declineHipObjToken(const httplib::Request& req, httplib::Response& res,
            const std::string& resource);
        RdmaTransport* selectTransport(const httplib::Request& req, httplib::Response& res,
            const std::string& resource, RdmaToken& outToken);
        static void sendStoreError(httplib::Response& res, ObjectStore::Result result,
            const std::string& resource);
        static void setObjectHeaders(httplib::Response& res, const ObjectInfo& info);
        static void setEmptyBody(httplib::Response& res);
        static bool hasAnyParam(const httplib::Request& req,
            std::initializer_list<const char*> names);
};

#endif // S3SERVER_H_
