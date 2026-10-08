// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "cuobjclient.h"

#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

#include "HexTk.h"
#include "HipObjToken.h"
#include "HttpLib.h"
#include "Log.h"
#include "RcDevice.h"

/*
 * The shim behind the cuObject client API. cuObject's protocol is one object request with a
 * token that the server uses to RDMA-read or -write the client buffer right away. With Reliable
 * Connections that needs a queue pair that is already connected to the server's, so minting a
 * token connects first: POST /.s3rdma-rc/connect at the S3 endpoint exchanges queue pair numbers,
 * GIDs and sequence numbers (in hipObject's token format), and the minted token names that
 * connection: "rc:<connection id>:<address>:<size>:<rkey>". Releasing the token disconnects.
 *
 * The RDMA device and the buffer registrations are shared by all instances of a process, like
 * NVIDIA's library shares them (see SharedDevice). Configuration comes from the environment, since
 * cuObject's API has no place for it:
 *   S3RDMA_RC_ENDPOINT   http://host:port of the S3 server (required)
 *   S3RDMA_RC_ADDR       IPv4 address of the local RDMA NIC to use (required)
 *   S3RDMA_RC_GID_INDEX  GID table index to use (optional, default: RoCE v2 GID of the address)
 */

namespace
{
    const char* ENV_ENDPOINT = "S3RDMA_RC_ENDPOINT";
    const char* ENV_ADDR = "S3RDMA_RC_ADDR";
    const char* ENV_GID_INDEX = "S3RDMA_RC_GID_INDEX";
    const char* CONNECT_PATH = "/.s3rdma-rc/connect";
    const char* DISCONNECT_PATH = "/.s3rdma-rc/disconnect";
    const int HTTP_TIMEOUT_SECS = 10;

    struct Registration
    {
        ibv_mr* mr = nullptr;
        size_t size = 0;
    };

    struct Connection
    {
        RcDevice::Queue queue;
        std::string id; // the server's connection id
    };

    /**
     * The RDMA device and the buffer registrations, shared by all cuObjClient instances of the
     * process. NVIDIA's library registers buffers process-wide as well (through cuFile), and e.g.
     * the Cloudian SDK fork registers a buffer through one instance and mints the tokens for it
     * through another. One device also means one protection domain, which a buffer registration
     * and the queue pair that uses it have to share.
     */
    struct SharedDevice
    {
        std::unique_ptr<RcDevice> device;
        std::string endpoint; // "http://host:port"
        std::mutex mutex;
        std::map<char*, Registration> registrations; // by buffer start

        ~SharedDevice();

        static std::shared_ptr<SharedDevice> get();
        Registration* findRegistration(char* ptr, size_t size);
    };
}

struct cuObjClient::Impl
{
    CUObjOps_t ops;
    std::shared_ptr<SharedDevice> shared;
    bool connected = false;
    std::map<std::string, Connection> connections; // by token string

    bool connect(Connection& conn, uint32_t ourPsn);
    void disconnect(Connection& conn);
};

SharedDevice::~SharedDevice()
{
    if(!device)
        return;

    for(auto& ptrAndReg : registrations)
        device->deregisterBuffer(ptrAndReg.second.mr);
}

/**
 * The process-wide instance: opened from the environment by the first cuObjClient, released with
 * the last one. Returns nullptr if the configuration is missing or the device cannot be opened.
 */
std::shared_ptr<SharedDevice> SharedDevice::get()
{
    static std::mutex getMutex;
    static std::weak_ptr<SharedDevice> instance;

    std::lock_guard<std::mutex> lock(getMutex);

    std::shared_ptr<SharedDevice> shared = instance.lock();
    if(shared)
        return shared;

    const char* endpointEnv = getenv(ENV_ENDPOINT);
    const char* addrEnv = getenv(ENV_ADDR);
    const char* gidIndexEnv = getenv(ENV_GID_INDEX);

    if(!endpointEnv || !addrEnv)
    {
        Log::error(std::string("cuObject RC shim: ") + ENV_ENDPOINT + " and " + ENV_ADDR +
            " must be set in the environment.");
        return nullptr;
    }

    try
    {
        shared = std::make_shared<SharedDevice>();
        shared->endpoint = endpointEnv;
        shared->device = std::make_unique<RcDevice>(addrEnv,
            gidIndexEnv ? atoi(gidIndexEnv) : -1);
    }
    catch(const std::exception& e)
    {
        Log::error(std::string("cuObject RC shim: ") + e.what() );
        return nullptr;
    }

    instance = shared;

    return shared;
}

Registration* SharedDevice::findRegistration(char* ptr, size_t size)
{
    // the registration that starts at or before ptr and covers the range
    auto iter = registrations.upper_bound(ptr);
    if(iter == registrations.begin() )
        return nullptr;

    iter--;

    return (ptr + size <= iter->first + iter->second.size) ? &iter->second : nullptr;
}

/**
 * Exchange queue pair details with the server and connect our queue pair to its.
 */
bool cuObjClient::Impl::connect(Connection& conn, uint32_t ourPsn)
{
    HipObjToken ourToken;
    ourToken.qpNum = conn.queue.qp->qp_num;
    memcpy(ourToken.gid, shared->device->getLocalGid().raw, sizeof(ourToken.gid) );
    ourToken.portNum = shared->device->getPortNum();

    httplib::Client http(shared->endpoint);
    http.set_connection_timeout(HTTP_TIMEOUT_SECS);
    http.set_read_timeout(HTTP_TIMEOUT_SECS);

    httplib::Result res = http.Post(CONNECT_PATH,
        httplib::Headers{ {"x-amz-rdma-token", ourToken.encode() },
            {"x-amz-rdma-psn", HexTk::toHex(ourPsn, 6) } }, "", "application/octet-stream");

    if(!res || res->status != 200)
    {
        Log::error("cuObject RC shim: connect request to " + shared->endpoint + CONNECT_PATH +
            " failed: " + (res ? "HTTP " + std::to_string(res->status) :
                httplib::to_string(res.error() ) ) );
        return false;
    }

    const std::string reply = res->get_header_value("x-amz-rdma-reply");
    HipObjToken serverToken;
    uint64_t serverPsn = 0;

    if(reply.compare(0, 4, "200:") || !HipObjToken::decode(reply.substr(4), serverToken) ||
        !HexTk::parseHex(res->get_header_value("x-amz-rdma-psn"), 6, serverPsn) )
    {
        Log::error("cuObject RC shim: malformed connect reply from server");
        return false;
    }

    conn.id = res->get_header_value("x-amz-rdma-session");

    ibv_gid serverGid;
    memcpy(serverGid.raw, serverToken.gid, sizeof(serverGid.raw) );

    return shared->device->connectQueue(conn.queue, serverToken.qpNum, serverGid,
        shared->device->getDefaultGidIndex(), serverPsn, ourPsn);
}

void cuObjClient::Impl::disconnect(Connection& conn)
{
    if(!conn.id.empty() )
    {
        httplib::Client http(shared->endpoint);
        http.set_connection_timeout(HTTP_TIMEOUT_SECS);
        http.Post(DISCONNECT_PATH, httplib::Headers{ {"x-amz-rdma-session", conn.id} }, "",
            "application/octet-stream"); // (best effort, the server also expires connections)
    }

    shared->device->destroyQueue(conn.queue);
}

////////////////////////////////// cuObject API //////////////////////////////////

cuObjClient::cuObjClient(CUObjOps_t& ops, cuObjProto_t proto) : _impl(new Impl() ), _reserved{}
{
    _impl->ops = ops;
    _impl->shared = SharedDevice::get();
    _impl->connected = (_impl->shared != nullptr);
}

cuObjClient::~cuObjClient()
{
    for(auto& tokenAndConn : _impl->connections)
        _impl->disconnect(tokenAndConn.second);

    delete _impl; // (releases the shared device; registrations go with its last user)
}

bool cuObjClient::isConnected()
{
    return _impl->connected;
}

cuObjErr_t cuObjClient::cuMemObjGetDescriptor(void* ptr, size_t size)
{
    if(!_impl->connected || !ptr || !size || size > CUOBJ_MAX_MEMORY_REG_SIZE)
        return CU_OBJ_INVALID_VALUE;

    std::lock_guard<std::mutex> lock(_impl->shared->mutex);

    ibv_mr* mr = _impl->shared->device->registerBuffer(ptr, size);
    if(!mr)
    {
        Log::error("cuObject RC shim: buffer registration failed. Size: " + std::to_string(size) +
            " (check the memlock limit, \"ulimit -l\")");
        return CU_OBJ_FAIL;
    }

    _impl->shared->registrations[static_cast<char*>(ptr)] = Registration{mr, size};

    return CU_OBJ_SUCCESS;
}

cuObjErr_t cuObjClient::cuMemObjPutDescriptor(void* ptr)
{
    if(!_impl->connected)
        return CU_OBJ_INVALID_VALUE;

    std::lock_guard<std::mutex> lock(_impl->shared->mutex);

    auto iter = _impl->shared->registrations.find(static_cast<char*>(ptr) );
    if(iter == _impl->shared->registrations.end() )
        return CU_OBJ_INVALID_VALUE;

    _impl->shared->device->deregisterBuffer(iter->second.mr);
    _impl->shared->registrations.erase(iter);

    return CU_OBJ_SUCCESS;
}

ssize_t cuObjClient::cuMemObjGetMaxRequestCallbackSize(void* ptr)
{
    if(!_impl->connected)
        return -1;

    std::lock_guard<std::mutex> lock(_impl->shared->mutex);

    Registration* reg = _impl->shared->findRegistration(static_cast<char*>(ptr), 1);

    return reg ? (ssize_t)reg->size : -1;
}

/**
 * Mint a token for a transfer of size bytes at ptr + buffer_offset: connects a new queue pair to
 * the server and names that connection in the token.
 */
cuObjErr_t cuObjClient::cuMemObjGetRDMAToken(void* ptr, size_t size, size_t buffer_offset,
    cuObjOpType_t operation, char** desc_str_out)
{
    if(!_impl->connected || !desc_str_out || !size)
        return CU_OBJ_INVALID_VALUE;

    std::lock_guard<std::mutex> lock(_impl->shared->mutex);

    char* start = static_cast<char*>(ptr) + buffer_offset;
    Registration* reg = _impl->shared->findRegistration(start, size);
    if(!reg)
    {
        Log::error("cuObject RC shim: token requested for an unregistered buffer range");
        return CU_OBJ_INVALID_VALUE;
    }

    Connection conn;

    try
    {
        conn.queue = _impl->shared->device->createQueue();
    }
    catch(const std::exception& e)
    {
        Log::error(std::string("cuObject RC shim: ") + e.what() );
        return CU_OBJ_FAIL;
    }

    if(!_impl->connect(conn, HexTk::randomPsn() ) )
    {
        _impl->disconnect(conn);
        return CU_OBJ_FAIL;
    }

    const std::string token = "rc:" + conn.id + ":" + HexTk::toHex( (uintptr_t)start, 16) + ":" +
        HexTk::toHex(size, 8) + ":" + HexTk::toHex(reg->mr->rkey, 8);

    *desc_str_out = strdup(token.c_str() );
    _impl->connections[token] = conn;

    return CU_OBJ_SUCCESS;
}

cuObjErr_t cuObjClient::cuMemObjPutRDMAToken(char* desc_str)
{
    if(!_impl->connected || !desc_str)
        return CU_OBJ_INVALID_VALUE;

    std::lock_guard<std::mutex> lock(_impl->shared->mutex);

    auto iter = _impl->connections.find(desc_str);
    if(iter == _impl->connections.end() )
        return CU_OBJ_INVALID_VALUE;

    _impl->disconnect(iter->second);
    _impl->connections.erase(iter);
    free(desc_str);

    return CU_OBJ_SUCCESS;
}

cuObjErr_t cuObjClient::cuMemObjPutRDMAToken(char* desc_str, int err_code)
{
    return cuMemObjPutRDMAToken(desc_str); // (no queue pair reset needed, connections are per token)
}

void* cuObjClient::getCtx(const void* handle)
{
    return const_cast<void*>(handle);
}

/**
 * The callback flow of cuObject: mint a token and let the application's callback do the request.
 */
ssize_t cuObjClient::cuObjGet(void* ctx, void* ptr, size_t size, loff_t offset, loff_t buf_offset)
{
    char* token = nullptr;
    if(cuMemObjGetRDMAToken(ptr, size, buf_offset, CUOBJ_GET, &token) != CU_OBJ_SUCCESS)
        return -1;

    const cufileRDMAInfo_t info = {1, (int)strlen(token), token};
    const ssize_t res = _impl->ops.get ?
        _impl->ops.get(ctx, static_cast<char*>(ptr) + buf_offset, size, offset, &info) : -1;

    cuMemObjPutRDMAToken(token);

    return res;
}

ssize_t cuObjClient::cuObjPut(void* ctx, void* ptr, size_t size, loff_t offset, loff_t buf_offset)
{
    char* token = nullptr;
    if(cuMemObjGetRDMAToken(ptr, size, buf_offset, CUOBJ_PUT, &token) != CU_OBJ_SUCCESS)
        return -1;

    const cufileRDMAInfo_t info = {1, (int)strlen(token), token};
    const ssize_t res = _impl->ops.put ?
        _impl->ops.put(ctx, static_cast<char*>(ptr) + buf_offset, size, offset, &info) : -1;

    cuMemObjPutRDMAToken(token);

    return res;
}

cuObjMemoryType_t cuObjClient::getMemoryType(const void* ptr)
{
    return CUOBJ_MEMORY_SYSTEM; // the shim handles host memory only
}

void cuObjClient::setupTelemetry(bool use_OTEL, std::ostream* os) {}
void cuObjClient::setTelemFlags(unsigned log_flags) {}
void cuObjClient::setTelemFlags(unsigned log_flags, unsigned log_op_flags) {}
void cuObjClient::setOpFreq(unsigned freq_get, unsigned freq_put) {}
void cuObjClient::shutdownTelemetry() {}
