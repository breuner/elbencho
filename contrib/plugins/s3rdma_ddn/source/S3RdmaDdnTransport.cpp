// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "S3RdmaDdnTransport.h"
#include "S3RdmaDdnPlugin.h"
#include "Logger.h"
#include "workers/WorkerException.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

#define S3RDMA_MAX_RETRYDELAY_MS    2000

/* Process-wide request retry accounting across all workers. The elbencho core counts failed
   requests neither on abort nor with --s3ignoreerrors, so the last worker to uninit prints a
   summary. */
static std::atomic<size_t> numActiveWorkers{0};
static std::atomic<size_t> numRetries{0}; // retry attempts
static std::atomic<size_t> numRecovered{0}; // requests that succeeded after at least one retry
static std::atomic<size_t> numFailed{0}; // requests that failed (after all retries, if any)

void S3RdmaDdnTransport::init(S3Client& client, const BufferVec& hostBufs, const BufferVec& gpuBufs,
    const ProgArgs& progArgs, size_t workerRank)
{
    if (!S3RdmaDdnPlugin::getInstance().isActive())
        return;

    http_client = std::make_unique<S3ClientCurl>(&progArgs, workerRank);
    if (!http_client->isValid())
        throw WorkerException("cuObject RDMA not connected");

    gpuDirect = !progArgs.getGPUIDsVec().empty();

    numActiveWorkers++;
    initialized = true;

    for (auto buf: (gpuDirect ? gpuBufs : hostBufs))
        http_client->registerBuffer(buf, progArgs.getBlockSize());
}

void S3RdmaDdnTransport::uninit()
{
    if (http_client)
        http_client->unregisterBuffers();

    if (!initialized)
        return;

    initialized = false;

    if (--numActiveWorkers != 0)
        return;

    // last worker: print summary for this run and reset for the next run (service mode)
    LOGGER(Log_NORMAL, "s3rdma_ddn: " <<
        "Request retries: " << numRetries.exchange(0) << "; " <<
        "Recovered after retry: " << numRecovered.exchange(0) << "; " <<
        "Failed: " << numFailed.exchange(0) << std::endl);
}

/**
 * Run an RDMA request, retrying on transient failures according to the plugin args. Every
 * attempt gets a fresh context, i.e. a fresh RDMA token. The curl handle of a failed attempt is
 * discarded instead of returned to the connection cache, so a retry also uses a new connection
 * and a fresh DNS lookup.
 *
 * @prepare callable(TkCurlContext&) -> bool to prepare the request in the given context.
 * @ctx holds the context of the last attempt on return.
 * @numAttempts number of attempts on return.
 * @return true on success.
 */
template<typename PrepareFn>
bool S3RdmaDdnTransport::runWithRetries(const char* op, const std::string& objPath,
    PrepareFn prepare, std::optional<TkCurlContext>& ctx, size_t& numAttempts)
{
    const S3RdmaDdnPlugin& plugin = S3RdmaDdnPlugin::getInstance();
    const size_t maxRetries = plugin.getNumRetries();
    const size_t baseDelayMS = plugin.getRetryDelayMS();

    for (numAttempts = 1; ; numAttempts++)
    {
        ctx.reset(); // returns handle of previous attempt to cache (if still valid)
        ctx.emplace(http_client.get());

        bool success = prepare(*ctx);
        if (success)
            success = http_client->runSync(*ctx);

        if (success)
        {
            if (numAttempts > 1)
                numRecovered++;

            return true;
        }

        /* the failure may be tied to the connection's network path (e.g. a server port that
           went down), so don't reuse it */
        ctx->discardConnection();

        if ( (numAttempts > maxRetries) || !ctx->isRetryable() )
        {
            numFailed++;

            LOGGER(Log_VERBOSE, "s3rdma_ddn: Giving up on " << op << " " << objPath << " " <<
                "after " << numAttempts << " attempt(s): " << ctx->errDescr() << std::endl);

            return false;
        }

        const size_t delayMS = std::min<size_t>(
            baseDelayMS << std::min<size_t>(numAttempts - 1, 16), S3RDMA_MAX_RETRYDELAY_MS);

        LOGGER(Log_VERBOSE, "s3rdma_ddn: Retrying " << op << " " << objPath << " " <<
            "(attempt " << numAttempts << " of " << (maxRetries + 1) << " failed: " <<
            ctx->errDescr() << "). Delay: " << delayMS << "ms" << std::endl);

        numRetries++;

        if (delayMS)
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMS) );
    }
}

S3DefaultTransport::PutObjectOutcome S3RdmaDdnTransport::putObject(S3Client& client, PutObjectRequest& request, char* hostBuf,
    char* gpuBuf, size_t len, AtomicLiveOps& liveOps)
{
    if(!active() || !len)
        return S3DefaultTransport::putObject(client, request, hostBuf, gpuBuf, len, liveOps);

    std::optional<TkCurlContext> ctx;
    size_t numAttempts;

    bool success = runWithRetries("PUT", request.GetBucket() + "/" + request.GetKey(),
        [&](TkCurlContext& attemptCtx)
        {
            return http_client->putObjectRDMAPrepare(attemptCtx, request.GetBucket(),
                request.GetKey(), isGpuDirect() ? gpuBuf : hostBuf, len);
        },
        ctx, numAttempts);

    if(!success)
        return PutObjectOutcome(makeError("PUT", *ctx, numAttempts) );

    liveOps.numBytesDone += len;

    S3::PutObjectResult result;
    result.SetETag(ctx->etag);

    return PutObjectOutcome(std::move(result));
}

S3DefaultTransport::UploadPartOutcome S3RdmaDdnTransport::uploadPart(S3Client& client, S3DefaultTransport::UploadPartRequest& request, char* hostBuf,
    char* gpuBuf, size_t len, AtomicLiveOps& liveOps)
{
    if(!active() || !len)
        return S3DefaultTransport::uploadPart(client, request, hostBuf, gpuBuf, len, liveOps);

    std::optional<TkCurlContext> ctx;
    size_t numAttempts;

    bool success = runWithRetries("UploadPart", request.GetBucket() + "/" + request.GetKey(),
        [&](TkCurlContext& attemptCtx)
        {
            return http_client->uploadPartRDMAPrepare(attemptCtx, request.GetBucket(),
                request.GetKey(), request.GetUploadId(), request.GetPartNumber(),
                isGpuDirect() ? gpuBuf : hostBuf, len);
        },
        ctx, numAttempts);

    if(!success)
        return UploadPartOutcome(makeError("UploadPart", *ctx, numAttempts) );

    liveOps.numBytesDone += len;

    S3::UploadPartResult result;
    result.SetETag(ctx->etag);

    return UploadPartOutcome(std::move(result));
}

void S3RdmaDdnTransport::uploadPartAsync(S3Client& client, UploadPartRequest& request, char* hostBuf,
        char* gpuBuf, size_t len, AtomicLiveOps& liveOps,
        std::promise<UploadPartOutcome>& promise)
{
    if(!active())
        return S3DefaultTransport::uploadPartAsync(client, request, hostBuf, gpuBuf, len, liveOps, promise);

    auto handler = [=, &client, &request, &liveOps, &promise, this](const S3Client*, bool, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&)
    {
        auto outcome = uploadPart(client, request, hostBuf, gpuBuf, len, liveOps);
        promise.set_value(std::move(outcome));
    };

    runAsync(client, handler);
}

S3DefaultTransport::GetObjectOutcome S3RdmaDdnTransport::getObject(S3Client& client, GetObjectRequest& request, char* hostBuf,
    char* gpuBuf, uint64_t offset, size_t len, AtomicLiveOps& liveOps)
{
    if(!active())
        return S3DefaultTransport::getObject(client, request, hostBuf, gpuBuf, offset, len, liveOps);

    std::optional<TkCurlContext> ctx;
    size_t numAttempts;

    bool success = runWithRetries("GET", request.GetBucket() + "/" + request.GetKey(),
        [&](TkCurlContext& attemptCtx)
        {
            return http_client->getObjectRDMAPrepare(attemptCtx, request.GetBucket(),
                request.GetKey(), isGpuDirect() ? gpuBuf : hostBuf, len, offset);
        },
        ctx, numAttempts);

    if(!success)
        return GetObjectOutcome(makeError("GET", *ctx, numAttempts) );

    liveOps.numBytesDone += len;

    S3::GetObjectResult result;
    result.SetContentLength(len);

    return GetObjectOutcome(std::move(result));
}

void S3RdmaDdnTransport::getObjectAsync(S3Client& client, GetObjectRequest& request, char* hostBuf,
        char* gpuBuf, uint64_t offset, size_t len, AtomicLiveOps& liveOps,
        std::promise<GetObjectOutcome>& promise)
{
    if(!active())
        return S3DefaultTransport::getObjectAsync(client, request, hostBuf, gpuBuf, offset, len, liveOps, promise);

    auto handler = [=, &client, &request, &liveOps, &promise, this](const S3Client*, bool, const std::shared_ptr<const Aws::Client::AsyncCallerContext>&)
    {
        auto outcome = getObject(client, request, hostBuf, gpuBuf, offset, len, liveOps);
        promise.set_value(std::move(outcome));
    };

    runAsync(client, handler);
}

template<typename HandlerT>
void S3RdmaDdnTransport::runAsync(S3Client& client, HandlerT &handler)
{
    /* Executor is private in S3Client and public interface is only to able run
     * S3Client methods.
     * As a hack, perform useless async call of cheap const method,
     * and do all the work in handler.  */
    client.SubmitAsync(&S3Client::MultipartUploadSupported, handler);
}

S3ErrorType S3RdmaDdnTransport::makeError(const char* op, const TkCurlContext &ctx,
    size_t numAttempts)
{
    std::string message = std::string("S3 RDMA ") + op + " failed";
    if (numAttempts > 1)
        message += " after " + std::to_string(numAttempts) + " attempts";
    message += ": " + ctx.errDescr();

    return S3ErrorType(Aws::Client::AWSError<S3Errors>(S3Errors::UNKNOWN, "S3RdmaError", message, false));
}
