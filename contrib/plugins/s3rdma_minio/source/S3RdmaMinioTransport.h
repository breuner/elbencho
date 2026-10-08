// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef S3RDMA_MINIO_S3RDMAMINIOTRANSPORT_H_
#define S3RDMA_MINIO_S3RDMAMINIOTRANSPORT_H_

#include <memory>

#include "modes/s3/S3Transport.h"
#include "S3RdmaTk.h"

/**
 * S3 transport of the s3rdma_minio plugin: while the plugin is active, object PUT/GET and
 * multipart part uploads move the object data over RDMA (see S3RdmaTk), while the multipart
 * create/complete/abort requests stay on HTTP; otherwise this is the default HTTP transport.
 * The async calls are inherited unchanged, S3RdmaMinioPlugin::checkArgs() rules them out while
 * the plugin is active.
 */
class S3RdmaMinioTransport : public S3DefaultTransport
{
    public:
        void init(S3Client& client, const BufferVec& hostBufs, const BufferVec& gpuBufs,
            const ProgArgs& progArgs, size_t workerRank);
        void uninit();

        bool isGpuDirect() const { return gpuDirect; }

        PutObjectOutcome putObject(S3Client& client, PutObjectRequest& request, char* hostBuf,
            char* gpuBuf, size_t len, AtomicLiveOps& liveOps);
        UploadPartOutcome uploadPart(S3Client& client, UploadPartRequest& request, char* hostBuf,
            char* gpuBuf, size_t len, AtomicLiveOps& liveOps);
        GetObjectOutcome getObject(S3Client& client, GetObjectRequest& request, char* hostBuf,
            char* gpuBuf, uint64_t offset, size_t len, AtomicLiveOps& liveOps);

    private:
        bool active{false}; // plugin selected via "--plugins" for this run
        bool gpuDirect{false}; // active and GPUs given: RDMA directly from/to the GPU buffers
        SharedCuObjClient* rdmaClient{NULL}; // process-wide singleton, not owned
        std::unique_ptr<S3RdmaControlPlane> controlPlane; // per-worker signed control requests
        BufferVec registeredBufs; // buffers registered with cuObject, for deregistration

        static S3ErrorType makeError(const char* op, ssize_t rdmaRes);
};

typedef S3RdmaMinioTransport S3Transport;

#endif // S3RDMA_MINIO_S3RDMAMINIOTRANSPORT_H_
