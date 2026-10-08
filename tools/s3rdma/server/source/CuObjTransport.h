// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef CUOBJTRANSPORT_H_
#define CUOBJTRANSPORT_H_

#ifdef CUOBJ_SUPPORT

#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

#include "RdmaTransport.h"

class cuObjServer;
struct rdma_buffer;

/**
 * RdmaTransport based on NVIDIA's libcuobjserver. Each channel owns one registered host buffer;
 * a transfer leases a channel for its duration, because cuObject channels must not be used by
 * more than one thread at a time.
 */
class CuObjTransport : public RdmaTransport
{
    public:
        CuObjTransport(const std::string& addr, unsigned short port, unsigned numChannels,
            size_t bufSize);
        virtual ~CuObjTransport();

        virtual ssize_t readFromClient(const std::string& key, const RdmaToken& token, size_t len,
            const Sink& sink) override;
        virtual ssize_t writeToClient(const std::string& key, const RdmaToken& token, size_t len,
            const Source& source) override;

    private:
        struct Channel
        {
            uint16_t id = 0; // cuObject channel id
            char* buf = nullptr; // registered host buffer
            struct rdma_buffer* rdmaBuf = nullptr; // registration handle of buf
        };

        std::unique_ptr<cuObjServer> server;
        size_t bufSize;
        std::vector<Channel> channels;
        std::vector<Channel*> freeChannels; // protected by mutex
        std::mutex mutex;
        std::condition_variable freeChannelsCond;

        Channel* acquireChannel();
        void releaseChannel(Channel* channel);
};

#endif // CUOBJ_SUPPORT

#endif // CUOBJTRANSPORT_H_
