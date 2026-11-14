// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "cuobjclient.h"

#include <cerrno>
#include <chrono>
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
 * token for a buffer connects first: POST /.s3rdma-rc/connect at the S3 endpoint exchanges queue
 * pair numbers, GIDs and sequence numbers (in hipObject's token format), and the minted token
 * names that connection: "rc:<connection id>:<address>:<size>:<rkey>". The connection belongs to
 * the registered buffer and serves all later tokens for it, so a token is just a string like
 * with NVIDIA's library and may be released before the transfer. The connection goes with the
 * registration, or earlier when a transfer failed (error overload of cuMemObjPutRDMAToken, the
 * server drops its side then too) or when it was idle for long (the server reaps those).
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
    const std::chrono::seconds RECONNECT_AFTER(120); // (the server drops idle connections at 300s)

    struct Connection
    {
        RcDevice::Queue queue;
        std::string id; // the server's connection id, empty while not connected
    };

    struct Registration
    {
        ibv_mr* mr = nullptr;
        size_t size = 0;
        Connection conn; // connected on the first token for this buffer
        std::chrono::steady_clock::time_point lastToken;
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
        Registration* findRegistrationByConnection(const std::string& connId);
        bool connect(Connection& conn);
        void disconnect(Connection& conn);
    };
}

struct cuObjClient::Impl
{
    CUObjOps_t ops;
    std::shared_ptr<SharedDevice> shared;
    bool connected = false;
};

SharedDevice::~SharedDevice()
{
    if(!device)
        return;

    for(auto& ptrAndReg : registrations)
    {
        disconnect(ptrAndReg.second.conn);
        device->deregisterBuffer(ptrAndReg.second.mr);
    }
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

Registration* SharedDevice::findRegistrationByConnection(const std::string& connId)
{
    for(auto& ptrAndReg : registrations)
        if(ptrAndReg.second.conn.id == connId)
            return &ptrAndReg.second;

    return nullptr;
}

/**
 * Create a queue pair, exchange its details with the server and connect it to the server's.
 * Leaves the connection disconnected on failure.
 */
bool SharedDevice::connect(Connection& conn)
{
    try
    {
        conn.queue = device->createQueue();
    }
    catch(const std::exception& e)
    {
        Log::error(std::string("cuObject RC shim: ") + e.what() );
        return false;
    }

    const uint32_t ourPsn = HexTk::randomPsn();

    HipObjToken ourToken;
    ourToken.qpNum = conn.queue.qp->qp_num;
    memcpy(ourToken.gid, device->getLocalGid().raw, sizeof(ourToken.gid) );
    ourToken.portNum = device->getPortNum();

    httplib::Client http(endpoint);
    http.set_connection_timeout(HTTP_TIMEOUT_SECS);
    http.set_read_timeout(HTTP_TIMEOUT_SECS);

    httplib::Result res = http.Post(CONNECT_PATH,
        httplib::Headers{ {"x-amz-rdma-token", ourToken.encode() },
            {"x-amz-rdma-psn", HexTk::toHex(ourPsn, 6) } }, "", "application/octet-stream");

    if(!res || res->status != 200)
    {
        Log::error("cuObject RC shim: connect request to " + endpoint + CONNECT_PATH +
            " failed: " + (res ? "HTTP " + std::to_string(res->status) :
                httplib::to_string(res.error() ) ) );
        disconnect(conn);
        return false;
    }

    const std::string reply = res->get_header_value("x-amz-rdma-reply");
    HipObjToken serverToken;
    uint64_t serverPsn = 0;

    if(reply.compare(0, 4, "200:") || !HipObjToken::decode(reply.substr(4), serverToken) ||
        !HexTk::parseHex(res->get_header_value("x-amz-rdma-psn"), 6, serverPsn) )
    {
        Log::error("cuObject RC shim: malformed connect reply from server");
        disconnect(conn);
        return false;
    }

    conn.id = res->get_header_value("x-amz-rdma-session");

    ibv_gid serverGid;
    memcpy(serverGid.raw, serverToken.gid, sizeof(serverGid.raw) );

    if(!device->connectQueue(conn.queue, serverToken.qpNum, serverGid,
        device->getDefaultGidIndex(), serverPsn, ourPsn) )
    {
        disconnect(conn);
        return false;
    }

    return true;
}

/**
 * Drop the connection on both sides. No-op for a connection that is not connected.
 */
void SharedDevice::disconnect(Connection& conn)
{
    if(!conn.id.empty() )
    {
        httplib::Client http(endpoint);
        http.set_connection_timeout(HTTP_TIMEOUT_SECS);
        http.set_read_timeout(HTTP_TIMEOUT_SECS);
        http.Post(DISCONNECT_PATH, httplib::Headers{ {"x-amz-rdma-session", conn.id} }, "",
            "application/octet-stream"); // (best effort, the server also expires connections)
        conn.id.clear();
    }

    if(conn.queue.qp)
    {
        device->destroyQueue(conn.queue);
        conn.queue = RcDevice::Queue();
    }
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

    Registration& reg = _impl->shared->registrations[static_cast<char*>(ptr)];
    reg.mr = mr;
    reg.size = size;

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

    _impl->shared->disconnect(iter->second.conn);
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
 * Mint a token for a transfer of size bytes at ptr + buffer_offset. The token names the
 * registered buffer's connection, which is set up here on the first token for the buffer (or
 * again when the last token is so old that the server may have dropped the connection).
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

    const auto now = std::chrono::steady_clock::now();

    if(!reg->conn.id.empty() && (now - reg->lastToken) > RECONNECT_AFTER)
        _impl->shared->disconnect(reg->conn);

    if(reg->conn.id.empty() && !_impl->shared->connect(reg->conn) )
        return CU_OBJ_FAIL;

    reg->lastToken = now;

    const std::string token = "rc:" + reg->conn.id + ":" + HexTk::toHex( (uintptr_t)start, 16) +
        ":" + HexTk::toHex(size, 8) + ":" + HexTk::toHex(reg->mr->rkey, 8);

    *desc_str_out = strdup(token.c_str() );

    return CU_OBJ_SUCCESS;
}

cuObjErr_t cuObjClient::cuMemObjPutRDMAToken(char* desc_str)
{
    if(!_impl->connected || !desc_str)
        return CU_OBJ_INVALID_VALUE;

    free(desc_str); // (the connection stays with the buffer)

    return CU_OBJ_SUCCESS;
}

/**
 * Release a token after a failed transfer: a failed work request leaves the queue pairs in error
 * state and the server drops its side, so drop the buffer's connection; the next token for the
 * buffer connects again.
 */
cuObjErr_t cuObjClient::cuMemObjPutRDMAToken(char* desc_str, int err_code)
{
    if(!_impl->connected || !desc_str)
        return CU_OBJ_INVALID_VALUE;

    if(err_code)
    {
        std::lock_guard<std::mutex> lock(_impl->shared->mutex);

        const std::string token(desc_str);
        const size_t idEnd = token.find(':', 3); // "rc:<connection id>:..."

        Registration* reg = (token.compare(0, 3, "rc:") || idEnd == std::string::npos) ?
            nullptr : _impl->shared->findRegistrationByConnection(token.substr(3, idEnd - 3) );

        if(reg)
            _impl->shared->disconnect(reg->conn);
    }

    free(desc_str);

    return CU_OBJ_SUCCESS;
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

    cuMemObjPutRDMAToken(token, (res < 0) ? EIO : 0);

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

    cuMemObjPutRDMAToken(token, (res < 0) ? EIO : 0);

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
