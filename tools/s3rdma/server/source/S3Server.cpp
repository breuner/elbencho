// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "S3Server.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "HipObjToken.h"
#include "HipObjV2Server.h"
#include "HttpLib.h"
#include "RcTokenTransport.h"
#include "Log.h"
#include "S3Xml.h"

using httplib::ContentReader;
using httplib::Request;
using httplib::Response;
typedef ObjectStore::Result Result;

namespace
{
    // S3-over-RDMA protocol headers (NVIDIA cuObject convention)
    const char* HDR_RDMA_TOKEN = "x-amz-rdma-token";
    const char* HDR_RDMA_REPLY = "x-amz-rdma-reply";
    const char* HDR_RDMA_BYTES = "x-amz-rdma-bytes-transferred";
    const int RDMA_REPLY_DECLINED = 501;

    const char* CONTENT_TYPE_XML = "application/xml";
    const char* CONTENT_TYPE_BINARY = "application/octet-stream";

    // "/<bucket>" or "/<bucket>/<key>" (bucket names cannot start with a dot, so the control
    // endpoints below "/.hipobj-rc/" never match)
    const char* PATH_PATTERN = R"(^/([^/.][^/]*)(?:/(.*))?$)";
    const char* HIPOBJ_PATH_PREFIX = "/.hipobj-rc/";
    const char* RC_PATH_PREFIX = "/.s3rdma-rc/";
}

S3Server::S3Server(const Config& config, ObjectStore& store, RdmaTransport* cuObj,
    RcTokenTransport* rcToken, HipObjV2Server* hipObj) :
    config(config), store(store), cuObj(cuObj), rcToken(rcToken), hipObj(hipObj),
    svr(std::make_unique<httplib::Server>() )
{
    svr->new_task_queue = [numThreads = config.numThreads]()
        { return new httplib::ThreadPool(numThreads); };
    svr->set_keep_alive_max_count(1000000);
    svr->set_keep_alive_timeout(60);

#ifdef RC_SUPPORT
    if(rcToken)
    {
        svr->Post(std::string(RC_PATH_PREFIX) + "connect",
            [rcToken](const Request& req, Response& res) { rcToken->handleConnect(req, res); } );
        svr->Post(std::string(RC_PATH_PREFIX) + "disconnect",
            [rcToken](const Request& req, Response& res)
            { rcToken->handleDisconnect(req, res); } );
    }

    if(hipObj)
    {
        svr->Post(std::string(HIPOBJ_PATH_PREFIX) + "prepare",
            [hipObj](const Request& req, Response& res) { hipObj->handlePrepare(req, res); } );
        svr->Post(std::string(HIPOBJ_PATH_PREFIX) + "ready",
            [hipObj](const Request& req, Response& res) { hipObj->handleReady(req, res); } );
        svr->Post(std::string(HIPOBJ_PATH_PREFIX) + "cancel",
            [hipObj](const Request& req, Response& res) { hipObj->handleCancel(req, res); } );
    }
    else
#endif
    svr->Post(std::string(HIPOBJ_PATH_PREFIX) + "(prepare|ready|cancel)",
        [](const Request& req, Response& res)
        {
            // the protocol's marker for "not supported here", so that the client falls back
            res.status = 501;
            res.set_header("X-Amz-Rdma-Protocol-Status", "unsupported");
        } );

    svr->Get("/", [this](const Request& req, Response& res)
        { res.set_content(S3Xml::listBuckets(this->store.listBuckets() ), CONTENT_TYPE_XML); } );
    svr->Get(PATH_PATTERN, [this](const Request& req, Response& res)
        { handleGet(req, res); } ); // (also handles HEAD)
    svr->Put(PATH_PATTERN, [this](const Request& req, Response& res, const ContentReader& reader)
        { handlePut(req, res, reader); } );
    svr->Post(PATH_PATTERN, [this](const Request& req, Response& res, const ContentReader& reader)
        { handlePost(req, res, reader); } );
    svr->Delete(PATH_PATTERN, [this](const Request& req, Response& res)
        { handleDelete(req, res); } );

    svr->set_exception_handler([](const Request& req, Response& res, std::exception_ptr exPtr)
    {
        try { std::rethrow_exception(exPtr); }
        catch(const std::exception& e)
        {
            Log::error(std::string("Request failed with exception: ") + e.what() );
            sendError(res, 500, "InternalError", e.what(), req.path);
        }
    } );

    svr->set_logger([](const Request& req, const Response& res)
    {
        if(Log::verbose)
            Log::debug("HTTP " + req.method + " " + req.target + " -> " +
                std::to_string(res.status) );
    } );
}

S3Server::~S3Server() = default;

/**
 * Bind, print the ready message and serve requests until stop() is called.
 *
 * @return false if binding failed.
 */
bool S3Server::run()
{
    if(!svr->bind_to_port(config.listenAddr, config.port) )
    {
        Log::error("Unable to listen on " + config.listenAddr + ":" + std::to_string(config.port) );
        return false;
    }

    Log::info("Listening on http://" + config.listenAddr + ":" + std::to_string(config.port) +
        " (data dir: " + config.dataDir + "; cuObject DC RDMA: " +
        (cuObj ? config.rdmaAddr + ":" + std::to_string(config.rdmaPort) : "disabled") +
        "; RC RDMA (shim, hipobj-rc-v2): " + (rcToken ? config.rdmaAddr : "disabled") + ")");

    return svr->listen_after_bind();
}

void S3Server::stop()
{
    svr->stop();
}

///////////////////////////////// Dispatching /////////////////////////////////

void S3Server::handleGet(const Request& req, Response& res)
{
    const std::string bucket = req.matches[1];
    const std::string key = req.matches[2];

    if(key.empty() )
    {
        if(!store.bucketExists(bucket) )
            return sendError(res, 404, "NoSuchBucket", "The specified bucket does not exist.",
                bucket);

        if(req.method == "HEAD")
            return; // HeadBucket

        if(hasAnyParam(req, {"acl", "tagging", "versioning", "object-lock", "policy", "location",
            "uploads", "cors", "lifecycle"} ) )
            return sendError(res, 501, "NotImplemented", "Bucket subresource not supported.",
                bucket);

        return listObjects(req, res, bucket);
    }

    if(hasAnyParam(req, {"acl", "tagging", "uploadId", "attributes"} ) )
        return sendError(res, 501, "NotImplemented", "Object subresource not supported.",
            bucket + "/" + key);

    if(req.has_header(HDR_RDMA_TOKEN) )
        rdmaGet(req, res, bucket, key);
    else
        httpGet(res, bucket, key); // (also HeadObject)
}

void S3Server::handlePut(const Request& req, Response& res, const ContentReader& reader)
{
    const std::string bucket = req.matches[1];
    const std::string key = req.matches[2];

    if(key.empty() )
    {
        if(hasAnyParam(req, {"acl", "tagging", "versioning", "object-lock", "policy", "cors",
            "lifecycle"} ) )
            return sendError(res, 501, "NotImplemented", "Bucket subresource not supported.",
                bucket);

        const Result createRes = store.createBucket(bucket); // (body is ignored)
        if(createRes != Result::OK)
            return sendStoreError(res, createRes, bucket);

        res.set_header("Location", "/" + bucket);
        return;
    }

    if(hasAnyParam(req, {"acl", "tagging"} ) )
        return sendError(res, 501, "NotImplemented", "Object subresource not supported.",
            bucket + "/" + key);

    // UploadPart or PutObject
    const std::string uploadID = req.get_param_value("uploadId");
    unsigned partNumber = 0;

    if(!uploadID.empty() )
    {
        partNumber = std::atoi(req.get_param_value("partNumber").c_str() );
        if(partNumber < 1 || partNumber > 10000)
            return sendError(res, 400, "InvalidArgument", "Part number must be in [1, 10000].",
                bucket + "/" + key);
    }

    if(req.has_header(HDR_RDMA_TOKEN) )
        rdmaPut(req, res, bucket, key, uploadID, partNumber);
    else
        httpPut(res, reader, bucket, key, uploadID, partNumber);
}

void S3Server::handlePost(const Request& req, Response& res, const ContentReader& reader)
{
    const std::string bucket = req.matches[1];
    const std::string key = req.matches[2];

    if(key.empty() )
    {
        if(req.has_param("delete") )
            return deleteObjects(res, reader, bucket);

        return sendError(res, 501, "NotImplemented", "Bucket operation not supported.", bucket);
    }

    if(req.has_param("uploads") ) // CreateMultipartUpload
    {
        std::string uploadID;
        const Result createRes = store.createMultipartUpload(bucket, key, uploadID);
        if(createRes != Result::OK)
            return sendStoreError(res, createRes, bucket + "/" + key);

        return res.set_content(S3Xml::initiateMultipartUpload(bucket, key, uploadID),
            CONTENT_TYPE_XML);
    }

    if(req.has_param("uploadId") ) // CompleteMultipartUpload (the part list body is ignored)
    {
        ObjectInfo info;
        const Result completeRes = store.completeMultipartUpload(bucket,
            req.get_param_value("uploadId"), info);
        if(completeRes != Result::OK)
            return sendStoreError(res, completeRes, bucket + "/" + key);

        res.set_header("ETag", info.etag);
        return res.set_content(S3Xml::completeMultipartUpload(bucket, key, info.etag),
            CONTENT_TYPE_XML);
    }

    sendError(res, 501, "NotImplemented", "Object operation not supported.", bucket + "/" + key);
}

void S3Server::handleDelete(const Request& req, Response& res)
{
    const std::string bucket = req.matches[1];
    const std::string key = req.matches[2];
    Result deleteRes;

    if(key.empty() )
        deleteRes = store.deleteBucket(bucket);
    else
    if(req.has_param("uploadId") )
        deleteRes = store.abortMultipartUpload(bucket, req.get_param_value("uploadId") );
    else
        deleteRes = store.deleteObject(bucket, key);

    if(deleteRes != Result::OK)
        return sendStoreError(res, deleteRes, bucket + "/" + key);

    res.status = 204;
}

//////////////////////////////// Operations /////////////////////////////////

void S3Server::listObjects(const Request& req, Response& res, const std::string& bucket)
{
    std::vector<ObjectInfo> objects;
    const std::string prefix = req.get_param_value("prefix");

    const Result listRes = store.listObjects(bucket, prefix, objects);
    if(listRes != Result::OK)
        return sendStoreError(res, listRes, bucket);

    res.set_content(S3Xml::listObjects(bucket, prefix, objects), CONTENT_TYPE_XML);
}

/**
 * GetObject/HeadObject with the data in the HTTP body. Ranges are applied by cpp-httplib.
 */
void S3Server::httpGet(Response& res, const std::string& bucket, const std::string& key)
{
    auto stream = std::make_shared<std::ifstream>();
    ObjectInfo info;

    const Result openRes = store.openObjectForRead(bucket, key, *stream, info);
    if(openRes != Result::OK)
        return sendStoreError(res, openRes, bucket + "/" + key);

    setObjectHeaders(res, info);

    if(!info.size)
        return res.set_content("", CONTENT_TYPE_BINARY);

    res.set_content_provider(info.size, CONTENT_TYPE_BINARY,
        [stream](size_t offset, size_t length, httplib::DataSink& sink)
        {
            static thread_local std::vector<char> buf;
            buf.resize(std::min<size_t>(length, 1024 * 1024) );

            stream->seekg(offset);
            stream->read(buf.data(), buf.size() );

            return stream->gcount() > 0 && sink.write(buf.data(), stream->gcount() );
        } );
}

/**
 * GetObject via RDMA: the requested bytes are RDMA-written into the client buffer described by
 * the token, the response has no body. The "range" header selects a slice of the object as usual.
 */
void S3Server::rdmaGet(const Request& req, Response& res, const std::string& bucket,
    const std::string& key)
{
    const std::string resource = bucket + "/" + key;

    RdmaToken token;
    RdmaTransport* rdma = selectTransport(req, res, resource, token);
    if(!rdma)
        return;

    std::ifstream stream;
    ObjectInfo info;

    const Result openRes = store.openObjectForRead(bucket, key, stream, info);
    if(openRes != Result::OK)
    {
        res.set_header(HDR_RDMA_REPLY, "404");
        return sendStoreError(res, openRes, resource);
    }

    // resolve the byte range ("bytes=first-last", either end may be open)
    const bool isRanged = !req.ranges.empty();
    uint64_t start = 0;
    uint64_t end = info.size ? (info.size - 1) : 0;

    if(isRanged)
    {
        const auto [first, last] = req.ranges[0];

        if(first < 0) // suffix range: the last <last> bytes
            start = ( (uint64_t)last < info.size) ? (info.size - last) : 0;
        else
        {
            start = first;
            if(last >= 0 && (uint64_t)last < info.size)
                end = last;
        }

        if(start >= info.size)
        {
            Log::debug("RDMA GET " + resource + ": range " + std::to_string(first) + "-" +
                std::to_string(last) + " not satisfiable for object size " +
                std::to_string(info.size) );
            res.set_header(HDR_RDMA_REPLY, "416");
            return sendError(res, 416, "InvalidRange", "The requested range is not satisfiable.",
                resource);
        }
    }

    // never transfer more than the client buffer can take
    const size_t len = std::min<uint64_t>(info.size ? (end - start + 1) : 0, token.size);

    stream.seekg(start);

    const ssize_t transferRes = rdma->writeToClient(key, token, len,
        [&stream](char* buf, size_t chunkLen)
        {
            stream.read(buf, chunkLen);
            return (size_t)stream.gcount() == chunkLen;
        } );

    if(transferRes < 0)
    {
        Log::error("RDMA GET " + resource + " failed: " + strerror(-transferRes) );
        res.set_header(HDR_RDMA_REPLY, "500");
        return sendError(res, 500, "InternalError", std::string("RDMA transfer failed: ") +
            strerror(-transferRes), resource);
    }

    Log::debug("RDMA GET " + resource + ": " + std::to_string(transferRes) + " bytes" +
        (isRanged ? " (range " + std::to_string(start) + "-" + std::to_string(end) + ")" : "") +
        (token.isRc() ? " (rc)" : " (cuobj)") );

    setObjectHeaders(res, info);
    res.set_header(HDR_RDMA_REPLY, isRanged ? "206" : "200");
    res.set_header(HDR_RDMA_BYTES, std::to_string(transferRes) );

    if(isRanged)
        res.set_header("Content-Range", "bytes " + std::to_string(start) + "-" +
            std::to_string(start + transferRes - 1) + "/" + std::to_string(info.size) );

    setEmptyBody(res);
}

/**
 * PutObject/UploadPart with the data in the HTTP body, streamed into the file.
 */
void S3Server::httpPut(Response& res, const ContentReader& reader, const std::string& bucket,
    const std::string& key, const std::string& uploadID, unsigned partNumber)
{
    std::ofstream stream;

    const Result openRes = openForWrite(bucket, key, uploadID, partNumber, stream);
    if(openRes != Result::OK)
        return sendStoreError(res, openRes, bucket + "/" + key);

    const bool readerRes = reader([&stream](const char* data, size_t len)
    {
        stream.write(data, len);
        return stream.good();
    } );

    stream.close();

    if(!readerRes || !stream)
        return sendError(res, 500, "InternalError", "Storing the object data failed.",
            bucket + "/" + key);

    ObjectInfo info;
    if(statWritten(bucket, key, uploadID, partNumber, info) != Result::OK)
        return sendError(res, 500, "InternalError", "Stat of stored object failed.",
            bucket + "/" + key);

    res.set_header("ETag", info.etag);
}

/**
 * PutObject/UploadPart via RDMA: the request has no body, the data gets RDMA-read from the
 * client buffer described by the token. The token also carries the transfer size.
 */
void S3Server::rdmaPut(const Request& req, Response& res, const std::string& bucket,
    const std::string& key, const std::string& uploadID, unsigned partNumber)
{
    const std::string resource = bucket + "/" + key;

    RdmaToken token;
    RdmaTransport* rdma = selectTransport(req, res, resource, token);
    if(!rdma)
        return;

    if(req.get_header_value("Content-Length", "0") != "0" || req.has_header("Transfer-Encoding") )
    {
        res.set_header(HDR_RDMA_REPLY, "400");
        return sendError(res, 400, "InvalidArgument", "An RDMA request must not carry a body.",
            resource);
    }

    std::ofstream stream;

    const Result openRes = openForWrite(bucket, key, uploadID, partNumber, stream);
    if(openRes != Result::OK)
    {
        res.set_header(HDR_RDMA_REPLY, "404");
        return sendStoreError(res, openRes, resource);
    }

    const ssize_t transferRes = rdma->readFromClient(key, token, token.size,
        [&stream](const char* data, size_t len)
        {
            stream.write(data, len);
            return stream.good();
        } );

    stream.close();

    if(transferRes < 0 || !stream)
    {
        const std::string reason = (transferRes < 0) ? strerror(-transferRes) : "write error";
        Log::error("RDMA PUT " + resource + " failed: " + reason);

        if(uploadID.empty() )
            store.deleteObject(bucket, key); // don't leave a truncated object behind

        res.set_header(HDR_RDMA_REPLY, "500");
        return sendError(res, 500, "InternalError", "RDMA transfer failed: " + reason, resource);
    }

    ObjectInfo info;
    if(statWritten(bucket, key, uploadID, partNumber, info) != Result::OK)
        return sendError(res, 500, "InternalError", "Stat of stored object failed.", resource);

    Log::debug("RDMA PUT " + resource + (uploadID.empty() ? "" :
        " part " + std::to_string(partNumber) ) + ": " + std::to_string(transferRes) + " bytes" +
        (token.isRc() ? " (rc)" : " (cuobj)") );

    res.set_header("ETag", info.etag);
    res.set_header(HDR_RDMA_REPLY, "200");
    res.set_header(HDR_RDMA_BYTES, std::to_string(transferRes) );
}

/**
 * DeleteObjects: the keys come in the XML body.
 */
void S3Server::deleteObjects(Response& res, const ContentReader& reader,
    const std::string& bucket)
{
    std::string body;

    reader([&body](const char* data, size_t len)
    {
        body.append(data, len);
        return body.size() <= 16 * 1024 * 1024;
    } );

    const std::vector<std::string> keys = S3Xml::parseDeleteKeys(body);

    for(const std::string& key : keys)
    {
        const Result deleteRes = store.deleteObject(bucket, key);
        if(deleteRes != Result::OK)
            return sendStoreError(res, deleteRes, bucket + "/" + key);
    }

    res.set_content(S3Xml::deleteObjects(keys), CONTENT_TYPE_XML);
}

////////////////////////////////// Helpers //////////////////////////////////

// Open the object, or the part if uploadID is set, for writing.
Result S3Server::openForWrite(const std::string& bucket, const std::string& key,
    const std::string& uploadID, unsigned partNumber, std::ofstream& outStream)
{
    return uploadID.empty() ?
        store.openObjectForWrite(bucket, key, outStream) :
        store.openPartForWrite(bucket, uploadID, partNumber, outStream);
}

Result S3Server::statWritten(const std::string& bucket, const std::string& key,
    const std::string& uploadID, unsigned partNumber, ObjectInfo& outInfo)
{
    return uploadID.empty() ?
        store.statObject(bucket, key, outInfo) :
        store.statPart(bucket, uploadID, partNumber, outInfo);
}

void S3Server::sendError(Response& res, int status, const std::string& code,
    const std::string& message, const std::string& resource)
{
    res.status = status;
    res.set_content(S3Xml::error(code, message, "/" + resource), CONTENT_TYPE_XML);
}

/**
 * An object request with a hipObject RC token in the x-amz-rdma-token header is the single round
 * trip variant of that protocol, which has no working RC handshake; the two round trip variant
 * (hipobj-rc-v2) is served by HipObjV2Server instead. Decline it with the protocol's markers so
 * that the client falls back to a plain HTTP transfer.
 *
 * @return true if the request was declined.
 */
bool S3Server::declineHipObjToken(const Request& req, Response& res, const std::string& resource)
{
    if(!HipObjToken::isTokenHeader(req.get_header_value(HDR_RDMA_TOKEN) ) )
        return false;

    res.set_header(HDR_RDMA_REPLY, std::to_string(RDMA_REPLY_DECLINED) );
    res.set_header("X-Amz-Rdma-Protocol-Status", "unsupported");
    sendError(res, 501, "NotImplemented", "Single round trip RC transfers are not supported, "
        "use the hipobj-rc-v2 control protocol.", resource);

    return true;
}

/**
 * Parse the request's RDMA token and pick the transport for it. hipObject's single round trip
 * token gets declined, so do tokens of a transport that is not available.
 *
 * @return nullptr with the response set, if there is no transport for this request.
 */
RdmaTransport* S3Server::selectTransport(const Request& req, Response& res,
    const std::string& resource, RdmaToken& outToken)
{
    if(declineHipObjToken(req, res, resource) )
        return nullptr;

    if(!RdmaToken::parse(req.get_header_value(HDR_RDMA_TOKEN), outToken) )
    {
        res.set_header(HDR_RDMA_REPLY, "400");
        sendError(res, 400, "InvalidArgument", "Unsupported or malformed RDMA token.", resource);
        return nullptr;
    }

    RdmaTransport* transport = cuObj;
#ifdef RC_SUPPORT
    if(outToken.isRc() )
        transport = rcToken;
#else
    if(outToken.isRc() )
        transport = nullptr;
#endif

    if(!transport)
    {
        res.set_header(HDR_RDMA_REPLY, std::to_string(RDMA_REPLY_DECLINED) );
        sendError(res, 501, "NotImplemented", std::string("RDMA transfers via ") +
            (outToken.isRc() ? "RC" : "cuObject") + " are disabled.", resource);
    }

    return transport;
}

void S3Server::sendStoreError(Response& res, Result result, const std::string& resource)
{
    switch(result)
    {
        case Result::NoSuchBucket:
            return sendError(res, 404, "NoSuchBucket", "The specified bucket does not exist.",
                resource);
        case Result::NoSuchKey:
            return sendError(res, 404, "NoSuchKey", "The specified key does not exist.", resource);
        case Result::NoSuchUpload:
            return sendError(res, 404, "NoSuchUpload", "The specified upload does not exist.",
                resource);
        case Result::BucketNotEmpty:
            return sendError(res, 409, "BucketNotEmpty", "The bucket you tried to delete is not "
                "empty.", resource);
        case Result::InvalidName:
            return sendError(res, 400, "InvalidArgument", "Invalid bucket or key name.", resource);
        default:
            return sendError(res, 500, "InternalError", std::string("File system error: ") +
                strerror(errno), resource);
    }
}

void S3Server::setObjectHeaders(Response& res, const ObjectInfo& info)
{
    res.set_header("ETag", info.etag);
    res.set_header("Last-Modified", S3Xml::httpDate(info.mtime) );
    res.set_header("Accept-Ranges", "bytes");
}

/**
 * Empty body for the reply to a request that carried a "range" header (the RDMA GET). cpp-httplib
 * rejects an empty response to a ranged request as unsatisfiable (416) unless it comes from a
 * content provider, and it answers with status 200 then. That is fine here: the clients evaluate
 * the 200/206 distinction from the x-amz-rdma-reply header, not from the HTTP status. (A provider
 * with a length of zero does not count, hence the chunked one with an empty body.)
 */
void S3Server::setEmptyBody(Response& res)
{
    res.set_chunked_content_provider(CONTENT_TYPE_BINARY,
        [](size_t, httplib::DataSink& sink) { sink.done(); return true; } );
}

bool S3Server::hasAnyParam(const Request& req, std::initializer_list<const char*> names)
{
    return std::any_of(names.begin(), names.end(),
        [&req](const char* name) { return req.has_param(name); } );
}
