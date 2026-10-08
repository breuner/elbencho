// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifdef RC_SUPPORT

#include "RcTokenTransport.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include "HexTk.h"
#include "HipObjToken.h"
#include "HttpLib.h"
#include "Log.h"

using httplib::Request;
using httplib::Response;

namespace
{
    const std::chrono::seconds TRANSFER_TIMEOUT(30); // per chunk
    const std::chrono::seconds IDLE_TIMEOUT(300); // connections without transfers get dropped
    const size_t MAX_CONNECTIONS = 1024;
}

/**
 * @throw std::runtime_error if the staging buffers cannot be set up.
 */
RcTokenTransport::RcTokenTransport(RcDevice& device, unsigned numBuffers, size_t bufSize) :
    device(device), bufSize(bufSize)
{
    buffers.resize(numBuffers);

    for(Buffer& buffer : buffers)
    {
        void* ptr = nullptr;
        if(posix_memalign(&ptr, 4096, bufSize) )
            throw std::runtime_error("Allocation of RDMA staging buffer failed. Size: " +
                std::to_string(bufSize) );

        buffer.ptr = static_cast<char*>(ptr);
        buffer.mr = device.registerBuffer(ptr, bufSize);
        if(!buffer.mr)
            throw std::runtime_error("Registration of RDMA staging buffer failed. Size: " +
                std::to_string(bufSize) + ". (Check the memlock limit, \"ulimit -l\".)");

        freeBuffers.push_back(&buffer);
    }

    Log::info("RDMA: RC token transport ready with " + std::to_string(numBuffers) +
        " staging buffers of " + std::to_string(bufSize) + " bytes each");
}

RcTokenTransport::~RcTokenTransport()
{
    for(auto& idAndConn : connections)
        device.destroyQueue(idAndConn.second->queue);

    for(Buffer& buffer : buffers)
    {
        device.deregisterBuffer(buffer.mr);
        free(buffer.ptr);
    }
}

//////////////////////////////// Connections ////////////////////////////////

/**
 * Connect a server queue pair to the client's queue pair described by its token.
 */
void RcTokenTransport::handleConnect(const Request& req, Response& res)
{
    HipObjToken clientToken;
    uint64_t clientPsn = 0;

    if(!HipObjToken::decode(req.get_header_value("x-amz-rdma-token"), clientToken) ||
        clientToken.transport != HipObjToken::TRANSPORT_RC ||
        !HexTk::parseHex(req.get_header_value("x-amz-rdma-psn"), 6, clientPsn) || !clientPsn)
    {
        res.status = 400;
        return;
    }

    ibv_gid peerGid;
    memcpy(peerGid.raw, clientToken.gid, sizeof(peerGid.raw) );

    const int gidIndex = device.chooseGidIndex(peerGid);
    const uint32_t serverPsn = HexTk::randomPsn();
    auto conn = std::make_unique<Connection>();

    try
    {
        conn->queue = device.createQueue();
    }
    catch(const std::exception& e)
    {
        Log::error(std::string("RC connect failed: ") + e.what() );
        res.status = 500;
        return;
    }

    if(!device.connectQueue(conn->queue, clientToken.qpNum, peerGid, gidIndex, clientPsn,
        serverPsn) )
    {
        device.destroyQueue(conn->queue);
        res.status = 500;
        return;
    }

    // our side of the handshake: queue pair and GID for the client to connect to
    HipObjToken serverToken;
    serverToken.qpNum = conn->queue.qp->qp_num;
    memcpy(serverToken.gid, device.getLocalGid(gidIndex).raw, sizeof(serverToken.gid) );
    serverToken.portNum = device.getPortNum();

    const std::string id = HexTk::randomHex(16);
    conn->lastUse = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(mutex);

        reapIdleConnections();

        if(connections.size() >= MAX_CONNECTIONS)
        {
            device.destroyQueue(conn->queue);
            res.status = 503;
            return;
        }

        connections[id] = std::move(conn);
    }

    Log::debug("RC connect: connection " + id + " to client queue pair " +
        std::to_string(clientToken.qpNum) );

    res.status = 200;
    res.set_header("x-amz-rdma-session", id);
    res.set_header("x-amz-rdma-reply", "200:" + serverToken.encode() );
    res.set_header("x-amz-rdma-psn", HexTk::toHex(serverPsn, 6) );
}

void RcTokenTransport::handleDisconnect(const Request& req, Response& res)
{
    std::unique_ptr<Connection> conn;

    {
        std::lock_guard<std::mutex> lock(mutex);

        auto iter = connections.find(req.get_header_value("x-amz-rdma-session") );
        if(iter != connections.end() && !iter->second->busy)
        {
            conn = std::move(iter->second);
            connections.erase(iter);
        }
    }

    if(conn)
    {
        device.destroyQueue(conn->queue);
        Log::debug("RC disconnect: connection " + req.get_header_value("x-amz-rdma-session") );
    }

    res.status = 204; // (also for an unknown connection)
}

/**
 * Mark the connection as busy and return it, or nullptr if it is unknown or in use.
 */
RcTokenTransport::Connection* RcTokenTransport::acquireConnection(const std::string& id)
{
    std::lock_guard<std::mutex> lock(mutex);

    auto iter = connections.find(id);
    if(iter == connections.end() || iter->second->busy)
        return nullptr;

    iter->second->busy = true;
    return iter->second.get();
}

void RcTokenTransport::releaseConnection(Connection* connection)
{
    std::lock_guard<std::mutex> lock(mutex);

    connection->busy = false;
    connection->lastUse = std::chrono::steady_clock::now();
}

// Drop connections that saw no transfer for a long time (e.g. the client died). Caller holds
// the mutex.
void RcTokenTransport::reapIdleConnections()
{
    const auto cutoff = std::chrono::steady_clock::now() - IDLE_TIMEOUT;

    for(auto iter = connections.begin(); iter != connections.end(); )
    {
        if(!iter->second->busy && iter->second->lastUse < cutoff)
        {
            Log::debug("RC connection expired: " + iter->first);
            device.destroyQueue(iter->second->queue);
            iter = connections.erase(iter);
        }
        else
            iter++;
    }
}

////////////////////////////////// Transfers //////////////////////////////////

RcTokenTransport::Buffer* RcTokenTransport::acquireBuffer()
{
    std::unique_lock<std::mutex> lock(mutex);

    freeBuffersCond.wait(lock, [this]() { return !freeBuffers.empty(); } );

    Buffer* buffer = freeBuffers.back();
    freeBuffers.pop_back();

    return buffer;
}

void RcTokenTransport::releaseBuffer(Buffer* buffer)
{
    std::lock_guard<std::mutex> lock(mutex);

    freeBuffers.push_back(buffer);
    freeBuffersCond.notify_one();
}

ssize_t RcTokenTransport::readFromClient(const std::string& key, const RdmaToken& token,
    size_t len, const Sink& sink)
{
    return transfer(token, len, true, [&sink](char* buf, size_t chunkLen)
        { return sink(buf, chunkLen); } );
}

ssize_t RcTokenTransport::writeToClient(const std::string& key, const RdmaToken& token,
    size_t len, const Source& source)
{
    return transfer(token, len, false, source);
}

/**
 * Move len bytes between the client buffer named by the token and the chunk function, through
 * a staging buffer: RDMA READ then chunkFn(staging) for isRead, chunkFn(staging) then RDMA WRITE
 * otherwise.
 *
 * @return number of bytes transferred, or a negative errno.
 */
ssize_t RcTokenTransport::transfer(const RdmaToken& token, size_t len, bool isRead,
    const std::function<bool(char* buf, size_t len)>& chunkFn)
{
    // "rc:<connection id>:<address>:<size>:<rkey>"
    const size_t idEnd = token.descr.find(':', 3);
    const size_t rkeyStart = token.descr.rfind(':') + 1;
    uint64_t rkey = 0;

    if(idEnd == std::string::npos || !HexTk::parseHex(token.descr.substr(rkeyStart), 8, rkey) )
        return -EINVAL;

    const std::string id = token.descr.substr(3, idEnd - 3);

    Connection* conn = acquireConnection(id);
    if(!conn)
        return -ENOTCONN;

    Buffer* buffer = acquireBuffer();
    ssize_t result = 0;
    size_t numDone = 0;

    while(numDone < len)
    {
        const size_t chunkLen = std::min<uint64_t>(bufSize, len - numDone);
        const uint64_t remoteAddr = token.remoteAddr + numDone;
        ibv_wc wc = {};

        if(!isRead && !chunkFn(buffer->ptr, chunkLen) )
        {
            result = -EIO;
            break;
        }

        const bool posted = isRead ?
            device.postRead(conn->queue, buffer->mr, chunkLen, remoteAddr, rkey) :
            device.postWrite(conn->queue, buffer->mr, chunkLen, remoteAddr, rkey);

        if(!posted)
        {
            result = -EIO;
            break;
        }

        if(!device.waitForCompletion(conn->queue,
            std::chrono::steady_clock::now() + TRANSFER_TIMEOUT, wc) )
        {
            result = -ETIMEDOUT;
            break;
        }

        if(wc.status != IBV_WC_SUCCESS)
        {
            Log::error(std::string("RC transfer failed. Completion status: ") +
                ibv_wc_status_str(wc.status) + "; connection: " + id);
            result = -EIO;
            break;
        }

        if(isRead && !chunkFn(buffer->ptr, chunkLen) )
        {
            result = -EIO;
            break;
        }

        numDone += chunkLen;
    }

    releaseBuffer(buffer);

    if(result < 0)
    {
        // a failed work request leaves the queue pair in error state, so drop the connection
        std::lock_guard<std::mutex> lock(mutex);
        device.destroyQueue(conn->queue);
        connections.erase(id);
        return result;
    }

    releaseConnection(conn);

    return numDone;
}

#endif // RC_SUPPORT
