// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "S3RdmaMinioTransport.h"

#include <string>

#include "S3RdmaMinioPlugin.h"
#include "workers/WorkerException.h"

/**
 * Connect to the cuObject RDMA fabric and register the I/O buffers: the GPU buffers when GPUs are
 * given (GPU-direct), otherwise the host buffers.
 *
 * @throw WorkerException if the plugin is active but RDMA cannot be initialized.
 */
void S3RdmaMinioTransport::init(S3Client& client, const BufferVec& hostBufs,
    const BufferVec& gpuBufs, const ProgArgs& progArgs, size_t workerRank)
{
    active = S3RdmaMinioPlugin::getInstance().isActive();
    gpuDirect = active && !progArgs.getGPUIDsVec().empty();

    if(!active)
        return;

    rdmaClient = SharedCuObjClient::getInstance();
    if(!rdmaClient)
        throw WorkerException("S3 RDMA requested, but the cuObject RDMA fabric is not available "
            "(cuObjClient failed to connect). See the cuFile log (cufile.log in the current dir "
            "unless CUFILE_LOGFILE_PATH is set) and check the cuObject client config "
            "(CUFILE_ENV_PATH_JSON), e.g. rdma_dev_addr_list and rdma_peer_type.");

    controlPlane = std::make_unique<S3RdmaControlPlane>(&progArgs, workerRank);
    if(!controlPlane->isValid() )
        throw WorkerException("S3 RDMA requested, but the RDMA control plane could not be "
            "initialized. Check S3 endpoint and credentials.");

    const BufferVec& rdmaBufs = gpuDirect ? gpuBufs : hostBufs;

    for(char* buf : rdmaBufs)
    {
        if(!buf)
            continue;

        if(!rdmaClient->registerBuffer(buf, progArgs.getBlockSize() ) )
            throw WorkerException("Registration of I/O buffer with cuObject for RDMA failed.");

        registeredBufs.push_back(buf);
    }
}

void S3RdmaMinioTransport::uninit()
{
    if(rdmaClient)
        for(char* buf : registeredBufs)
            rdmaClient->deregisterBuffer(buf);

    registeredBufs.clear();
    controlPlane.reset();
    rdmaClient = NULL;
    active = gpuDirect = false;
}

S3RdmaMinioTransport::PutObjectOutcome S3RdmaMinioTransport::putObject(S3Client& client,
    PutObjectRequest& request, char* hostBuf, char* gpuBuf, size_t len, AtomicLiveOps& liveOps)
{
    if(!active || !len)
        return S3DefaultTransport::putObject(client, request, hostBuf, gpuBuf, len, liveOps);

    S3RdmaClientCtx ctx;
    ctx.bucket = request.GetBucket();
    ctx.object = request.GetKey();

    const ssize_t rdmaRes = rdmaPutWithRetry(*rdmaClient, *controlPlane, ctx,
        gpuDirect ? gpuBuf : hostBuf, len);

    if(rdmaRes <= 0)
        return PutObjectOutcome(makeError("PUT", rdmaRes) );

    liveOps.numBytesDone += rdmaRes;

    S3::PutObjectResult result;
    result.SetETag(ctx.etag);

    return PutObjectOutcome(std::move(result) );
}

/**
 * One part of a multipart upload over RDMA: the control request carries the upload ID and part
 * number as query parameters; the create/complete/abort requests of the upload stay on HTTP.
 */
S3RdmaMinioTransport::UploadPartOutcome S3RdmaMinioTransport::uploadPart(S3Client& client,
    UploadPartRequest& request, char* hostBuf, char* gpuBuf, size_t len, AtomicLiveOps& liveOps)
{
    if(!active || !len)
        return S3DefaultTransport::uploadPart(client, request, hostBuf, gpuBuf, len, liveOps);

    S3RdmaClientCtx ctx;
    ctx.bucket = request.GetBucket();
    ctx.object = request.GetKey();
    ctx.uploadID = request.GetUploadId();
    ctx.partNumber = request.GetPartNumber();

    const ssize_t rdmaRes = rdmaPutWithRetry(*rdmaClient, *controlPlane, ctx,
        gpuDirect ? gpuBuf : hostBuf, len);

    if(rdmaRes <= 0)
        return UploadPartOutcome(makeError("UploadPart", rdmaRes) );

    liveOps.numBytesDone += rdmaRes;

    S3::UploadPartResult result;
    result.SetETag(ctx.etag);

    return UploadPartOutcome(std::move(result) );
}

S3RdmaMinioTransport::GetObjectOutcome S3RdmaMinioTransport::getObject(S3Client& client,
    GetObjectRequest& request, char* hostBuf, char* gpuBuf, uint64_t offset, size_t len,
    AtomicLiveOps& liveOps)
{
    if(!active)
        return S3DefaultTransport::getObject(client, request, hostBuf, gpuBuf, offset, len,
            liveOps);

    S3RdmaClientCtx ctx;
    ctx.bucket = request.GetBucket();
    ctx.object = request.GetKey();

    const ssize_t rdmaRes = rdmaGetWithRetry(*rdmaClient, *controlPlane, ctx,
        gpuDirect ? gpuBuf : hostBuf, len, offset);

    if(rdmaRes <= 0)
        return GetObjectOutcome(makeError("GET", rdmaRes) );

    liveOps.numBytesDone += rdmaRes;

    S3::GetObjectResult result;
    result.SetContentLength(rdmaRes);

    return GetObjectOutcome(std::move(result) );
}

/**
 * Turn an RDMA failure into an SDK error, so that S3Mode reports it like any other S3 error.
 */
S3ErrorType S3RdmaMinioTransport::makeError(const char* op, ssize_t rdmaRes)
{
    const std::string message = std::string("S3 RDMA ") + op +
        ( (rdmaRes == S3Rdma::RDMA_NOT_SUPPORTED) ?
            " declined by server (no HTTP fallback)." :
            " failed. RDMA result: " + std::to_string(rdmaRes) );

    return S3ErrorType(Aws::Client::AWSError<S3Errors>(S3Errors::UNKNOWN, "S3RdmaError", message,
        false) );
}
