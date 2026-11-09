// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef S3RDMA_DDN_S3RDMA_TRANSPORT_H_
#define S3RDMA_DDN_S3RDMA_TRANSPORT_H_

#include "modes/s3/S3Transport.h"
#include "S3TkCurl.h"
#include <memory>
#include <optional>

class S3RdmaDdnTransport : public S3DefaultTransport
{
    public:
        void init(S3Client& client, const BufferVec& hostBufs, const BufferVec& gpuBufs,
            const ProgArgs& progArgs, size_t workerRank);
        void uninit();

        bool isGpuDirect() const { return gpuDirect; }
        bool active() const { return http_client && http_client->isValid(); }

        PutObjectOutcome putObject(S3Client& client, PutObjectRequest& request, char* hostBuf,
            char* gpuBuf, size_t len, AtomicLiveOps& liveOps);
        UploadPartOutcome uploadPart(S3Client& client, UploadPartRequest& request, char* hostBuf,
            char* gpuBuf, size_t len, AtomicLiveOps& liveOps);
        void uploadPartAsync(S3Client& client, UploadPartRequest& request, char* hostBuf,
            char* gpuBuf, size_t len, AtomicLiveOps& liveOps,
            std::promise<UploadPartOutcome>& promise);
        GetObjectOutcome getObject(S3Client& client, GetObjectRequest& request, char* hostBuf,
            char* gpuBuf, uint64_t offset, size_t len, AtomicLiveOps& liveOps);
        void getObjectAsync(S3Client& client, GetObjectRequest& request, char* hostBuf,
            char* gpuBuf, uint64_t offset, size_t len, AtomicLiveOps& liveOps,
            std::promise<GetObjectOutcome>& promise);

    private:
        template<typename HandlerT>
        void runAsync(S3Client& client, HandlerT &handler);

        template<typename PrepareFn>
        bool runWithRetries(const char* op, const std::string& objPath, PrepareFn prepare,
            std::optional<TkCurlContext>& ctx, size_t& numAttempts);

        static S3ErrorType makeError(const char* op, const TkCurlContext &ctx,
            size_t numAttempts);

        bool initialized{false}; // true if this instance is counted in the active workers

        bool gpuDirect{false};
        std::unique_ptr<S3ClientCurl> http_client;
};

typedef S3RdmaDdnTransport S3Transport;

#endif // S3RDMA_DDN_S3RDMA_TRANSPORT_H_
