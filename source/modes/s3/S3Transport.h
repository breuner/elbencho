// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef MODES_S3_S3TRANSPORT_H_
#define MODES_S3_S3TRANSPORT_H_

#include <future>
#include <memory>

#include "Common.h"
#include "LiveOps.h"
#include "ProgArgs.h"
#include "ProgException.h"
#include "modes/s3/toolkits/S3Tk.h"

#ifdef S3_SUPPORT
    #include INCLUDE_AWS_S3(model/GetObjectRequest.h)
    #include INCLUDE_AWS_S3(model/PutObjectRequest.h)
    #include INCLUDE_AWS_S3(model/UploadPartRequest.h)
#endif // S3_SUPPORT


#ifdef S3_SUPPORT

/**
 * How object data moves between the I/O buffers and the S3 server: the default is the HTTP body
 * of the AWS SDK requests. A plugin can replace this at compile time by defining
 * ELB_S3_TRANSPORT_HEADER (see below), e.g. to move the data over RDMA instead. S3Mode owns one
 * instance per worker thread and calls the methods directly, so there is no runtime dispatch.
 *
 * hostBuf/gpuBuf are the worker's buffers for this I/O (NULL hostBuf means "--s3fastget", i.e.
 * discard received data). liveOps receives the number of transferred bytes.
 */
class S3DefaultTransport
{
    public:
        using PutObjectRequest = S3::PutObjectRequest;
        using PutObjectOutcome = S3::PutObjectOutcome;
        using UploadPartRequest = S3::UploadPartRequest;
        using UploadPartOutcome = S3::UploadPartOutcome;
        using GetObjectRequest = S3::GetObjectRequest;
        using GetObjectOutcome = S3::GetObjectOutcome;

        void init(S3Client& client, const BufferVec& hostBufs, const BufferVec& gpuBufs,
            const ProgArgs& progArgs, size_t workerRank) {}
        void uninit() {}

        /**
         * A transport that moves the object data directly from/to the GPU buffers returns true
         * here (valid after init() ), so that LocalWorker skips the host<->GPU copies like it does
         * for cuFile and only copies for data verification.
         */
        bool isGpuDirect() const { return false; }

        /**
         * Check args for constraints of this transport.
         *
         * @throw ProgException if a problem is found.
         */
        static void checkArgs(const ProgArgs& progArgs)
        {
            if(progArgs.getUseCuFile() && (progArgs.getBenchMode() == BenchMode_S3) )
                throw ProgException("cuFile API cannot be used with S3");
        }

        PutObjectOutcome putObject(S3Client& client, PutObjectRequest& request, char* hostBuf,
            char* gpuBuf, size_t len, AtomicLiveOps& liveOps)
        {
            setUploadBody(request, hostBuf, len, liveOps);

            return client.PutObject(request);
        }

        UploadPartOutcome uploadPart(S3Client& client, UploadPartRequest& request, char* hostBuf,
            char* gpuBuf, size_t len, AtomicLiveOps& liveOps)
        {
            setUploadBody(request, hostBuf, len, liveOps);

            return client.UploadPart(request);
        }

        void uploadPartAsync(S3Client& client, UploadPartRequest& request, char* hostBuf,
            char* gpuBuf, size_t len, AtomicLiveOps& liveOps,
            std::promise<UploadPartOutcome>& promise)
        {
            setUploadBody(request, hostBuf, len, liveOps);

            client.UploadPartAsync(request,
                [&promise](const S3Client*, const UploadPartRequest&, UploadPartOutcome outcome,
                    const std::shared_ptr<const Aws::Client::AsyncCallerContext>&)
                { promise.set_value(std::move(outcome) ); } );
        }

        GetObjectOutcome getObject(S3Client& client, GetObjectRequest& request, char* hostBuf,
            char* gpuBuf, uint64_t offset, size_t len, AtomicLiveOps& liveOps)
        {
            setDownloadTarget(request, hostBuf, len, liveOps);

            return client.GetObject(request);
        }

        void getObjectAsync(S3Client& client, GetObjectRequest& request, char* hostBuf,
            char* gpuBuf, uint64_t offset, size_t len, AtomicLiveOps& liveOps,
            std::promise<GetObjectOutcome>& promise)
        {
            setDownloadTarget(request, hostBuf, len, liveOps);

            client.GetObjectAsync(request,
                [&promise](const S3Client*, const GetObjectRequest&, GetObjectOutcome outcome,
                    const std::shared_ptr<const Aws::Client::AsyncCallerContext>&)
                { promise.set_value(std::move(outcome) ); } );
        }

        static size_t getBytesReceived(const GetObjectOutcome& outcome)
        {
            return outcome.GetResult().GetContentLength();
        }

    private:
        /**
         * Send the buffer as request body. The stream gets the exact len, because the SDK would
         * otherwise send the full stream despite a smaller content length in the request.
         */
        template <typename REQUEST>
        static void setUploadBody(REQUEST& request, char* hostBuf, size_t len,
            AtomicLiveOps& liveOps)
        {
            if(len)
                request.SetBody(std::make_shared<S3MemoryStream>( (unsigned char*)hostBuf, len) );

            request.SetDataSentEventHandler(
                [&liveOps](const Aws::Http::HttpRequest*, long long numBytes)
                { liveOps.numBytesDone += numBytes; } );
        }

        /**
         * Receive the response body into the buffer or discard it if hostBuf is NULL.
         */
        static void setDownloadTarget(GetObjectRequest& request, char* hostBuf, size_t len,
            AtomicLiveOps& liveOps)
        {
            /* note: the factory is called async and also again on retries, so it captures by value
                and resets the stream position to the beginning of the buffer. */
            if(hostBuf)
                request.SetResponseStreamFactory([hostBuf, len]()
                {
                    S3MemoryStream* memStream = new S3MemoryStream( (unsigned char*)hostBuf, len);

                    memStream->seekp(0);
                    memStream->seekg(0);

                    return memStream;
                } );
            else
                request.SetResponseStreamFactory([]()
                {
                    return new Aws::FStream("/dev/null",
                        std::ios_base::out | std::ios_base::binary);
                } );

            request.SetDataReceivedEventHandler(
                [&liveOps](const Aws::Http::HttpRequest*, Aws::Http::HttpResponse*,
                    long long numBytes)
                { liveOps.numBytesDone += numBytes; } );
        }
};

#else // !S3_SUPPORT

class S3DefaultTransport
{
    public:
        static void checkArgs(const ProgArgs& progArgs) {}
};

#endif // S3_SUPPORT


#ifdef ELB_S3_TRANSPORT_HEADER
    #include ELB_S3_TRANSPORT_HEADER // from a plugin; defines S3Transport
#else
    typedef S3DefaultTransport S3Transport;
#endif


#endif /* MODES_S3_S3TRANSPORT_H_ */
