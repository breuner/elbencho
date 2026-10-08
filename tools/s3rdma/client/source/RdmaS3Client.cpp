// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "RdmaS3Client.h"

#include <cstdlib>

#include "HttpLib.h"
#include "Log.h"

namespace
{
    // S3-over-RDMA protocol headers (NVIDIA cuObject convention)
    const char* HDR_RDMA_TOKEN = "x-amz-rdma-token";
    const char* HDR_RDMA_REPLY = "x-amz-rdma-reply";
    const char* HDR_RDMA_BYTES = "x-amz-rdma-bytes-transferred";
    const int HTTP_TIMEOUT_SECS = 30;

    // A token for the duration of one request.
    struct Token
    {
        Token(cuObjClient& client, char* buf, size_t len, cuObjOpType_t op) : client(client)
        {
            if(client.cuMemObjGetRDMAToken(buf, len, 0, op, &str) != CU_OBJ_SUCCESS)
                str = nullptr;
        }

        ~Token()
        {
            if(str)
                client.cuMemObjPutRDMAToken(str);
        }

        cuObjClient& client;
        char* str = nullptr;
    };

    std::string describe(const httplib::Result& res)
    {
        if(!res)
            return "request failed: " + httplib::to_string(res.error() );

        return "HTTP " + std::to_string(res->status) + ", " + HDR_RDMA_REPLY + ": '" +
            res->get_header_value(HDR_RDMA_REPLY) + "'" +
            (res->body.empty() ? "" : ", body: " + res->body.substr(0, 300) );
    }
}

/**
 * @param endpoint "http://host:port" of the S3 server.
 */
RdmaS3Client::RdmaS3Client(const std::string& endpoint) :
    rdma(std::make_unique<cuObjClient>(ops, CUOBJ_PROTO_RDMA_DC_V1) ),
    http(std::make_unique<httplib::Client>(endpoint) )
{
    http->set_connection_timeout(HTTP_TIMEOUT_SECS);
    http->set_read_timeout(HTTP_TIMEOUT_SECS);
    http->set_keep_alive(true);

    connected = rdma->isConnected();

    if(!connected)
        Log::error("The RDMA client library is not connected to an RDMA device.");
}

RdmaS3Client::~RdmaS3Client() = default;

/**
 * Pin a buffer for RDMA. Required before transfers from/to it.
 */
bool RdmaS3Client::registerBuffer(char* buf, size_t len)
{
    if(rdma->cuMemObjGetDescriptor(buf, len) != CU_OBJ_SUCCESS)
    {
        Log::error("Registration of the I/O buffer for RDMA failed. Size: " + std::to_string(len) );
        return false;
    }

    return true;
}

bool RdmaS3Client::createBucket(const std::string& bucket)
{
    return simpleRequest("PUT", "/" + bucket);
}

bool RdmaS3Client::deleteBucket(const std::string& bucket)
{
    return simpleRequest("DELETE", "/" + bucket);
}

bool RdmaS3Client::deleteObject(const std::string& bucket, const std::string& key)
{
    return simpleRequest("DELETE", "/" + bucket + "/" + key);
}

/**
 * Upload the buffer as an object: the server RDMA-reads it while handling the body-less PUT.
 */
bool RdmaS3Client::putObject(const std::string& bucket, const std::string& key, char* buf,
    size_t len)
{
    Token token(*rdma, buf, len, CUOBJ_PUT);
    if(!token.str)
    {
        Log::error("Minting an RDMA token for the upload failed.");
        return false;
    }

    httplib::Result res = http->Put("/" + bucket + "/" + key,
        httplib::Headers{ {HDR_RDMA_TOKEN, token.str}, {"x-amz-content-sha256", "UNSIGNED-PAYLOAD"} },
        "", "application/octet-stream");

    // like the elbencho plugin: success is HTTP 200 with an ETag
    if(!res || res->status != 200 || !res->has_header("ETag") )
    {
        Log::error("RDMA PUT " + bucket + "/" + key + " failed: " + describe(res) );
        return false;
    }

    Log::debug("RDMA PUT " + bucket + "/" + key + ": " + std::to_string(len) + " bytes");

    return true;
}

/**
 * Download len bytes starting at offset into the buffer: the server RDMA-writes them while
 * handling the GET.
 *
 * @return number of bytes the server reports as transferred, -1 on error.
 */
ssize_t RdmaS3Client::getObject(const std::string& bucket, const std::string& key, char* buf,
    size_t len, uint64_t offset)
{
    Token token(*rdma, buf, len, CUOBJ_GET);
    if(!token.str)
    {
        Log::error("Minting an RDMA token for the download failed.");
        return -1;
    }

    httplib::Result res = http->Get("/" + bucket + "/" + key,
        httplib::Headers{ {HDR_RDMA_TOKEN, token.str}, {"x-amz-content-sha256", "UNSIGNED-PAYLOAD"},
            {"Range", "bytes=" + std::to_string(offset) + "-" + std::to_string(offset + len - 1) } } );

    // like the elbencho plugin: the x-amz-rdma-reply header decides, 200 or 206 is success
    const std::string reply = res ? res->get_header_value(HDR_RDMA_REPLY) : "";

    if(!res || (reply != "200" && reply != "206") )
    {
        Log::error("RDMA GET " + bucket + "/" + key + " failed: " + describe(res) );
        return -1;
    }

    const ssize_t numBytes = res->has_header(HDR_RDMA_BYTES) ?
        atoll(res->get_header_value(HDR_RDMA_BYTES).c_str() ) : (ssize_t)len;

    Log::debug("RDMA GET " + bucket + "/" + key + ": " + std::to_string(numBytes) + " bytes" +
        (offset ? " at offset " + std::to_string(offset) : "") );

    return numBytes;
}

bool RdmaS3Client::simpleRequest(const char* method, const std::string& path)
{
    httplib::Request req;
    req.method = method;
    req.path = path;

    httplib::Result res = http->send(req);

    if(!res || res->status >= 300)
    {
        Log::error(std::string(method) + " " + path + " failed: " + describe(res) );
        return false;
    }

    return true;
}
