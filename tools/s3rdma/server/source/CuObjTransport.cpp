// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifdef CUOBJ_SUPPORT

#include "CuObjTransport.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>

#include <cuobjserver.h>

#include "Log.h"

/**
 * Opens the RDMA device behind the given address and registers one buffer per channel.
 *
 * @param addr IP address or netdev name of the RDMA NIC.
 * @throw std::runtime_error if the RDMA session or the buffer registration fails.
 */
CuObjTransport::CuObjTransport(const std::string& addr, unsigned short port,
    unsigned numChannels, size_t bufSize) : bufSize(bufSize)
{
    // route the library's own messages to stderr (errors always, details only when verbose)
    cuObjServer::setupTelemetry(false, &std::cerr);
    cuObjServer::setTelemFlags(Log::verbose ?
        (CUOBJ_LOG_PATH_INFO | CUOBJ_LOG_PATH_ERROR) : CUOBJ_LOG_PATH_ERROR, 0);

    cuObjRDMATunable params;
    params.setNumDcis(std::max(numChannels, 1u) ); // one DC initiator per channel is enough

    server = std::make_unique<cuObjServer>(addr.c_str(), port, CUOBJ_PROTO_RDMA_DC_V1, params);

    if(!server->isConnected() )
        throw std::runtime_error("cuObject could not open an RDMA session for address \"" + addr +
            "\" (see the messages above). Note that cuObject's DC transport needs a NIC that "
            "supports Dynamically Connected queue pairs, e.g. ConnectX-5 or newer.");

    channels.resize(numChannels);

    for(Channel& channel : channels)
    {
        void* buf = nullptr;
        if(posix_memalign(&buf, 4096, bufSize) )
            throw std::runtime_error("Allocation of RDMA buffer failed. Size: " +
                std::to_string(bufSize) );

        channel.buf = static_cast<char*>(buf);
        channel.rdmaBuf = server->registerBuffer(buf, bufSize);
        if(!channel.rdmaBuf)
            throw std::runtime_error("Registration of RDMA buffer failed. Size: " +
                std::to_string(bufSize) + ". (Check the memlock limit, \"ulimit -l\".)");

        channel.id = server->allocateChannelId();
        if(channel.id == INVALID_CHANNEL_ID)
            throw std::runtime_error("Allocation of cuObject channel failed.");

        freeChannels.push_back(&channel);
    }

    Log::info("RDMA: cuObject server ready on " + addr + ":" + std::to_string(port) + " with " +
        std::to_string(numChannels) + " channels of " + std::to_string(bufSize) + " bytes each");
}

CuObjTransport::~CuObjTransport()
{
    for(Channel& channel : channels)
    {
        if(channel.rdmaBuf)
        {
            server->freeChannelId(channel.id);
            server->deRegisterBuffer(channel.rdmaBuf);
        }

        free(channel.buf);
    }

    server.reset();
}

CuObjTransport::Channel* CuObjTransport::acquireChannel()
{
    std::unique_lock<std::mutex> lock(mutex);

    freeChannelsCond.wait(lock, [this]() { return !freeChannels.empty(); } );

    Channel* channel = freeChannels.back();
    freeChannels.pop_back();

    return channel;
}

void CuObjTransport::releaseChannel(Channel* channel)
{
    std::lock_guard<std::mutex> lock(mutex);

    freeChannels.push_back(channel);
    freeChannelsCond.notify_one();
}

ssize_t CuObjTransport::readFromClient(const std::string& key, const RdmaToken& token, size_t len,
    const Sink& sink)
{
    // lease a channel for the duration of this transfer
    std::shared_ptr<Channel> channel(acquireChannel(),
        [this](Channel* leased) { releaseChannel(leased); } );

    size_t numDone = 0;

    while(numDone < len)
    {
        const size_t chunkLen = std::min<uint64_t>(bufSize, len - numDone);

        // synchronous RDMA READ of the next chunk from the client buffer into our buffer
        const ssize_t readRes = server->handlePutObject(key, channel->rdmaBuf,
            token.remoteAddr + numDone, chunkLen, token.descr, channel->id);

        if(readRes < 0)
            return readRes;

        if(!readRes)
            return -EIO; // nothing transferred, would loop forever

        if(!sink(channel->buf, readRes) )
            return -EIO;

        numDone += readRes;
    }

    return numDone;
}

ssize_t CuObjTransport::writeToClient(const std::string& key, const RdmaToken& token, size_t len,
    const Source& source)
{
    // lease a channel for the duration of this transfer
    std::shared_ptr<Channel> channel(acquireChannel(),
        [this](Channel* leased) { releaseChannel(leased); } );

    size_t numDone = 0;

    while(numDone < len)
    {
        const size_t chunkLen = std::min<uint64_t>(bufSize, len - numDone);

        if(!source(channel->buf, chunkLen) )
            return -EIO;

        // synchronous RDMA WRITE of the chunk from our buffer into the client buffer
        const ssize_t writeRes = server->handleGetObject(key, channel->rdmaBuf,
            token.remoteAddr + numDone, chunkLen, token.descr, channel->id);

        if(writeRes < 0)
            return writeRes;

        if(!writeRes)
            return -EIO;

        numDone += writeRes;
    }

    return numDone;
}

#endif // CUOBJ_SUPPORT
