// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifdef RC_SUPPORT

#include "HipObjV2Server.h"

#include <arpa/inet.h>
#include <cstdlib>
#include <fstream>

#include "HexTk.h"
#include "HipObjToken.h"
#include "HttpLib.h"
#include "Log.h"

using httplib::Request;
using httplib::Response;
typedef ObjectStore::Result Result;

namespace
{
    const char* PROTOCOL_NAME = "hipobj-rc-v2";
    const uint64_t MAX_TRANSFER_SIZE = 0x7fffffff; // protocol limit (2^31-1)
    const size_t MAX_SESSIONS = 64;
    const std::chrono::seconds PREPARE_TIMEOUT(10); // for the READY to arrive
    const std::chrono::seconds TRANSFER_TIMEOUT(30); // for the RDMA transfer to complete

    // The client's identity token can be all zeros: a same-device loopback marker.
    const std::string ZERO_TOKEN(HipObjToken::NUM_HEX_CHARS, '0');

    // "/bucket/key[?query]" => bucket and key
    bool parseTarget(const std::string& target, std::string& outBucket, std::string& outKey)
    {
        const std::string path = target.substr(0, target.find('?') );
        const size_t slash = path.find('/', 1);

        if(path.empty() || path[0] != '/' || slash == std::string::npos ||
            slash + 1 >= path.size() )
            return false;

        outBucket = path.substr(1, slash - 1);
        outKey = path.substr(slash + 1);

        return true;
    }

    void setProtocolHeader(Response& res)
    {
        res.set_header("X-Amz-Rdma-Protocol", PROTOCOL_NAME);
    }

    // The response for a request that does not speak this protocol.
    void sendUnsupported(Response& res)
    {
        res.status = 501;
        res.set_header("X-Amz-Rdma-Protocol-Status", "unsupported");
    }
}

HipObjV2Server::HipObjV2Server(RcDevice& device, ObjectStore& store) :
    device(device), store(store)
{
}

HipObjV2Server::~HipObjV2Server()
{
    for(auto& idAndSession : sessions)
        destroySession(*idAndSession.second);
}

//////////////////////////////////// Handlers ////////////////////////////////////

void HipObjV2Server::handlePrepare(const Request& req, Response& res)
{
    if(req.get_header_value("x-amz-rdma-protocol") != PROTOCOL_NAME)
        return sendUnsupported(res);

    std::unique_ptr<Session> session = createSession(req, res);
    if(!session)
        return; // (error response is set)

    // Our identity token for the client: queue pair, GID and the staging buffer.
    HipObjToken token;
    token.qpNum = session->queue.qp->qp_num;
    memcpy(token.gid, device.getLocalGid(session->localGidIndex).raw, sizeof(token.gid) );
    token.rkey = session->stagingMr->rkey;
    token.remoteAddr = (uintptr_t)session->staging;
    token.length = session->size;
    token.portNum = device.getPortNum();

    res.status = 200;
    setProtocolHeader(res);
    res.set_header("X-Amz-Rdma-Reply", "200:" + token.encode() );
    res.set_header("X-Amz-Rdma-Session", session->id);
    res.set_header("X-Amz-Rdma-Psn", HexTk::toHex(session->serverPsn, 6) );
    res.set_header("X-Amz-Rdma-Qpn", HexTk::toHex(token.qpNum) );
    res.set_header("X-Amz-Rdma-Mr-Addr", HexTk::toHex(token.remoteAddr) );
    res.set_header("X-Amz-Rdma-Mr-Rkey", HexTk::toHex(token.rkey) );

    Log::debug("hipobj-rc-v2 PREPARE " + session->op + " " + session->bucket + "/" +
        session->key + ": " + std::to_string(session->size) + " bytes, session " + session->id +
        ", peer GID " + RcDevice::gidToStr(session->peerGid) + ", local GID index " +
        std::to_string(session->localGidIndex) );

    std::lock_guard<std::mutex> lock(mutex);

    reapExpiredSessions();

    if(sessions.size() >= MAX_SESSIONS)
    {
        destroySession(*session);
        res.headers.clear();
        res.status = 503;
        return;
    }

    sessions[session->id] = std::move(session);
}

void HipObjV2Server::handleReady(const Request& req, Response& res)
{
    if(req.get_header_value("x-amz-rdma-protocol") != PROTOCOL_NAME)
        return sendUnsupported(res);

    uint64_t cookie = 0, clientAddr = 0, clientKey = 0, clientQpNum = 0;
    const std::string id = req.get_header_value("x-amz-rdma-session");

    if(id.size() != 32 || !HexTk::parseHex(req.get_header_value("x-amz-rdma-cookie"), 8, cookie) ||
        (req.has_header("x-amz-rdma-mr-addr") &&
            !HexTk::parseHex(req.get_header_value("x-amz-rdma-mr-addr"), 16, clientAddr) ) ||
        (req.has_header("x-amz-rdma-mr-rkey") &&
            !HexTk::parseHex(req.get_header_value("x-amz-rdma-mr-rkey"), 8, clientKey) ) ||
        (req.has_header("x-amz-rdma-qpn") &&
            !HexTk::parseHex(req.get_header_value("x-amz-rdma-qpn"), 8, clientQpNum) ) )
    {
        res.status = 400;
        return;
    }

    std::unique_ptr<Session> session = takeSession(id);
    if(!session)
    {
        res.status = 409; // unknown, expired or already in transfer
        return;
    }

    if(session->cookie != cookie)
    {
        res.status = 403;
        destroySession(*session);
        return;
    }

    if(transfer(*session, clientAddr, clientKey, clientQpNum, res) )
    {
        res.status = 200;
        setProtocolHeader(res);
        res.set_header("X-Amz-Rdma-Cookie", HexTk::toHex(session->cookie, 8) );
        res.set_header("X-Amz-Rdma-Bytes-Transferred", std::to_string(session->size) );
    }

    destroySession(*session);
}

void HipObjV2Server::handleCancel(const Request& req, Response& res)
{
    if(req.get_header_value("x-amz-rdma-protocol") != PROTOCOL_NAME)
        return sendUnsupported(res);

    std::unique_ptr<Session> session = takeSession(req.get_header_value("x-amz-rdma-session") );
    if(session)
        destroySession(*session);

    res.status = 204; // idempotent: also for an unknown session
}

//////////////////////////////////// Sessions ////////////////////////////////////

/**
 * Parse the PREPARE headers and set up the session with its queue pair and staging buffer. For a
 * GET, the staging buffer is filled with the object data right away.
 *
 * @return nullptr and an error status in res on failure.
 */
std::unique_ptr<HipObjV2Server::Session> HipObjV2Server::createSession(const Request& req,
    Response& res)
{
    auto session = std::make_unique<Session>();
    uint64_t psn = 0, cookie = 0;
    const std::string tokenHeader = req.get_header_value("x-amz-rdma-token");
    const std::string tokenStr = tokenHeader.substr(0, HipObjToken::NUM_HEX_CHARS);
    HipObjToken token;

    session->op = req.get_header_value("x-amz-rdma-op");

    if( (session->op != "GET" && session->op != "PUT") ||
        !HipObjToken::isTokenHeader(tokenHeader) ||
        !HexTk::parseHex(req.get_header_value("x-amz-rdma-psn"), 6, psn) || !psn ||
        !HexTk::parseHex(req.get_header_value("x-amz-rdma-cookie"), 8, cookie) ||
        !parseTarget(req.get_header_value("x-amz-rdma-target"), session->bucket, session->key) ||
        !HexTk::parseDec(req.get_header_value("x-amz-rdma-size"), session->size) || !session->size ||
        (req.has_header("x-amz-rdma-offset") &&
            !HexTk::parseDec(req.get_header_value("x-amz-rdma-offset"), session->offset) ) )
    {
        res.status = 400;
        return nullptr;
    }

    if(session->size > MAX_TRANSFER_SIZE)
    {
        res.status = 413;
        return nullptr;
    }

    // An all-zero token means "same device loopback": the client is reachable via our own GID.
    if(tokenStr == ZERO_TOKEN)
        session->peerGid = device.getLocalGid();
    else
    if(HipObjToken::decode(tokenStr, token) && token.transport == HipObjToken::TRANSPORT_RC)
        memcpy(session->peerGid.raw, token.gid, sizeof(token.gid) );
    else
    {
        res.status = 400;
        return nullptr;
    }

    session->localGidIndex = device.chooseGidIndex(session->peerGid);
    session->clientPsn = psn;
    session->cookie = cookie;

    // for a GET, the object must exist and cover the requested range
    if(session->op == "GET")
    {
        ObjectInfo info;
        const Result statRes = store.statObject(session->bucket, session->key, info);

        if(statRes != Result::OK)
        {
            res.status = (statRes == Result::NoSuchBucket || statRes == Result::NoSuchKey) ?
                404 : 500;
            return nullptr;
        }

        if(session->offset + session->size > info.size)
        {
            res.status = 416;
            return nullptr;
        }
    }

    void* staging = nullptr;
    if(posix_memalign(&staging, 4096, session->size) )
    {
        res.status = 500;
        return nullptr;
    }

    session->staging = static_cast<char*>(staging);

    try
    {
        session->stagingMr = device.registerBuffer(session->staging, session->size);
        if(!session->stagingMr)
            throw std::runtime_error("Registration of the staging buffer failed.");

        session->queue = device.createQueue();
    }
    catch(const std::exception& e)
    {
        Log::error(std::string("hipobj-rc-v2 PREPARE failed: ") + e.what() );
        destroySession(*session);
        res.status = 500;
        return nullptr;
    }

    if(session->op == "GET")
    {
        std::ifstream stream;
        ObjectInfo info;

        store.openObjectForRead(session->bucket, session->key, stream, info);
        stream.seekg(session->offset);
        stream.read(session->staging, session->size);

        if( (uint64_t)stream.gcount() != session->size)
        {
            destroySession(*session);
            res.status = 500;
            return nullptr;
        }
    }

    session->id = HexTk::randomHex(16);
    session->serverPsn = HexTk::randomPsn();
    session->deadline = std::chrono::steady_clock::now() + PREPARE_TIMEOUT;

    return session;
}

/**
 * Remove the session from the table and hand it to the caller, so that only one request ever
 * works with it.
 */
std::unique_ptr<HipObjV2Server::Session> HipObjV2Server::takeSession(const std::string& id)
{
    std::lock_guard<std::mutex> lock(mutex);

    reapExpiredSessions();

    auto iter = sessions.find(id);
    if(iter == sessions.end() )
        return nullptr;

    std::unique_ptr<Session> session = std::move(iter->second);
    sessions.erase(iter);

    return session;
}

void HipObjV2Server::destroySession(Session& session)
{
    device.destroyQueue(session.queue);
    device.deregisterBuffer(session.stagingMr);
    free(session.staging);

    session.stagingMr = nullptr;
    session.staging = nullptr;
}

// Drop sessions whose READY never came. Caller holds the mutex.
void HipObjV2Server::reapExpiredSessions()
{
    const auto now = std::chrono::steady_clock::now();

    for(auto iter = sessions.begin(); iter != sessions.end(); )
    {
        if(iter->second->deadline < now)
        {
            Log::debug("hipobj-rc-v2 session expired: " + iter->first);
            destroySession(*iter->second);
            iter = sessions.erase(iter);
        }
        else
            iter++;
    }
}

/**
 * The data phase of READY: connect to the client's queue pair, then receive (PUT) or send (GET)
 * the data. A PUT is stored afterwards.
 *
 * @return false with the error status set in res.
 */
bool HipObjV2Server::transfer(Session& session, uint64_t clientAddr, uint32_t clientKey,
    uint32_t clientQpNum, Response& res)
{
    const std::string resource = session.bucket + "/" + session.key;
    const auto deadline = std::chrono::steady_clock::now() + TRANSFER_TIMEOUT;
    ibv_wc wc = {};

    if(!clientQpNum || (session.op == "GET" && (!clientAddr || !clientKey) ) )
    {
        res.status = 400; // nothing to transfer to/from
        return false;
    }

    if(!device.connectQueue(session.queue, clientQpNum, session.peerGid, session.localGidIndex,
        session.clientPsn, session.serverPsn) )
    {
        res.status = 500;
        return false;
    }

    // PUT: the client writes into our staging buffer; GET: we write into the client's buffer.
    const bool posted = (session.op == "PUT") ?
        device.postRecv(session.queue, session.stagingMr, session.size) :
        device.postWriteWithImm(session.queue, session.stagingMr, session.size, clientAddr,
            clientKey, session.cookie);

    if(!posted)
    {
        Log::error("hipobj-rc-v2 " + session.op + " " + resource + ": posting the work request "
            "failed");
        res.status = 500;
        return false;
    }

    if(!device.waitForCompletion(session.queue, deadline, wc) )
    {
        Log::error("hipobj-rc-v2 " + session.op + " " + resource + ": transfer timed out");
        res.status = 408;
        return false;
    }

    const bool completionOk = (wc.status == IBV_WC_SUCCESS) &&
        ( (session.op == "PUT") ?
            (wc.opcode == IBV_WC_RECV_RDMA_WITH_IMM && (wc.wc_flags & IBV_WC_WITH_IMM) &&
                ntohl(wc.imm_data) == session.cookie && wc.byte_len == session.size) :
            (wc.opcode == IBV_WC_RDMA_WRITE) );

    if(!completionOk)
    {
        Log::error("hipobj-rc-v2 " + session.op + " " + resource + ": transfer failed. "
            "Completion status: " + std::string(ibv_wc_status_str(wc.status) ) +
            "; opcode: " + std::to_string(wc.opcode) + "; bytes: " + std::to_string(wc.byte_len) );
        res.status = 500;
        return false;
    }

    if(session.op == "PUT")
    {
        std::ofstream stream;
        ObjectInfo info;

        if(store.openObjectForWrite(session.bucket, session.key, stream) != Result::OK ||
            !stream.write(session.staging, session.size) )
        {
            res.status = 500;
            return false;
        }

        stream.close();

        if(store.statObject(session.bucket, session.key, info) == Result::OK)
            res.set_header("X-Amz-Rdma-Etag", info.etag);
    }

    Log::debug("RDMA " + session.op + " " + resource + ": " + std::to_string(session.size) +
        " bytes (hipobj-rc-v2" + (session.offset ? ", offset " + std::to_string(session.offset) :
        "") + ")");

    return true;
}

#endif // RC_SUPPORT
