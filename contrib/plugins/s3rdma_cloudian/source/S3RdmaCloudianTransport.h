// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef S3RDMA_CLOUDIAN_S3RDMACLOUDIANTRANSPORT_H_
#define S3RDMA_CLOUDIAN_S3RDMACLOUDIANTRANSPORT_H_

#include <aws/s3/RdmaPtr.h>
#include <future>
#include <vector>

#include "modes/s3/S3Transport.h"
#include "S3RdmaCloudianPlugin.h"
#include "workers/WorkerException.h"

#include INCLUDE_AWS_S3(model/PutObjectRDMARequest.h)
#include INCLUDE_AWS_S3(model/UploadPartRDMARequest.h)

/**
 * S3 transport of the s3rdma_cloudian plugin: the RDMA request variants of Cloudian's aws-sdk-cpp
 * fork, which move the object data via an RdmaPtr instead of an HTTP body. While the plugin is
 * inactive, the fork's client falls back to TCP for these requests.
 *
 * Buffers: GPU buffers are used directly (GPU-direct) with "--cufile --gdsbufreg", otherwise the
 * host buffers. Both can be pre-registered with cuObject for the whole run ("--gdsbufreg" or
 * "--cuobjhostbufreg"), else the SDK registers per request.
 */
class S3RdmaCloudianTransport
{
    public:
        using PutObjectRequest = S3::PutObjectRDMARequest;
        using PutObjectOutcome = S3::PutObjectRDMAOutcome;
        using UploadPartRequest = S3::UploadPartRDMARequest;
        using UploadPartOutcome = S3::UploadPartRDMAOutcome;
        using GetObjectRequest = S3::GetObjectRDMARequest;
        using GetObjectOutcome = S3::GetObjectRDMAOutcome;

        void init(S3Client& client, const BufferVec& hostBufs, const BufferVec& gpuBufs,
            const ProgArgs& progArgs, size_t workerRank)
        {
            const S3RdmaCloudianPlugin& plugin = S3RdmaCloudianPlugin::getInstance();

            /* the fork disables RDMA at startup if cuObject is not available, but EnableRDMA()
                would override that, so remember the startup state before the first override */
            static const bool rdmaAvailable = S3Client::IsRDMAEnabled();

            if(plugin.isActive() && !rdmaAvailable)
                throw WorkerException("S3 RDMA requested, but RDMA is not available (cuObject "
                    "initialization failed). See the cuFile log (cufile.log in the current dir "
                    "unless CUFILE_LOGFILE_PATH is set) and check the cuObject client config "
                    "(CUFILE_ENV_PATH_JSON), e.g. rdma_dev_addr_list and rdma_peer_type.");

            S3Client::EnableRDMA(plugin.isActive() );

            useGpuBufs = progArgs.getUseCuFile() && !progArgs.getGPUIDsVec().empty() &&
                progArgs.getUseGPUBufReg();
            bufsRegistered = useGpuBufs || plugin.getUseCuObjHostBufReg();

            if(!bufsRegistered)
                return;

            // (an RdmaPtr with registered=false registers the buffer and owns the registration)
            for(char* buf : (useGpuBufs ? gpuBufs : hostBufs) )
            {
                if(!buf)
                    continue;

                registrations.emplace_back(buf, progArgs.getBlockSize() );

                if(!registrations.back() ) // (NULL data after a failed registration)
                    throw WorkerException("Registration of I/O buffer with cuObject for RDMA "
                        "failed.");
            }
        }

        void uninit()
        {
            while(!registrations.empty() ) // deregister in reverse order
                registrations.pop_back();
        }

        bool isGpuDirect() const { return useGpuBufs; }

        /**
         * cuFile (GDS) buffers can be used for S3 with this transport, but only when the plugin is
         * active.
         *
         * @throw ProgException if a problem is found.
         */
        static void checkArgs(const ProgArgs& progArgs)
        {
            if(progArgs.getUseCuFile() && (progArgs.getBenchMode() == BenchMode_S3) &&
                !S3RdmaCloudianPlugin::getInstance().isActive() )
                throw ProgException("cuFile API cannot be used with S3 unless plugin "
                    "s3rdma_cloudian is active.");
        }

        PutObjectOutcome putObject(S3Client& client, PutObjectRequest& request, char* hostBuf,
            char* gpuBuf, size_t len, AtomicLiveOps& liveOps)
        {
            setSentHandler(request, liveOps);

            return client.PutObjectRDMA(request, rdmaPtr(hostBuf, gpuBuf, len) );
        }

        UploadPartOutcome uploadPart(S3Client& client, UploadPartRequest& request, char* hostBuf,
            char* gpuBuf, size_t len, AtomicLiveOps& liveOps)
        {
            setSentHandler(request, liveOps);

            return client.UploadPartRDMA(request, rdmaPtr(hostBuf, gpuBuf, len) );
        }

        void uploadPartAsync(S3Client& client, UploadPartRequest& request, char* hostBuf,
            char* gpuBuf, size_t len, AtomicLiveOps& liveOps,
            std::promise<UploadPartOutcome>& promise)
        {
            setSentHandler(request, liveOps);

            client.UploadPartRDMAAsync(request, rdmaPtr(hostBuf, gpuBuf, len),
                [&promise](const S3Client*, const UploadPartRequest&, Aws::S3::RdmaPtr&& ptr,
                    UploadPartOutcome&& outcome,
                    const std::shared_ptr<const Aws::Client::AsyncCallerContext>&)
                {
                    ptr.release();
                    promise.set_value(std::move(outcome) );
                } );
        }

        GetObjectOutcome getObject(S3Client& client, GetObjectRequest& request, char* hostBuf,
            char* gpuBuf, uint64_t offset, size_t len, AtomicLiveOps& liveOps)
        {
            setReceivedHandler(request, liveOps);

            return client.GetObjectRDMA(request, rdmaPtr(hostBuf, gpuBuf, len) );
        }

        void getObjectAsync(S3Client& client, GetObjectRequest& request, char* hostBuf,
            char* gpuBuf, uint64_t offset, size_t len, AtomicLiveOps& liveOps,
            std::promise<GetObjectOutcome>& promise)
        {
            setReceivedHandler(request, liveOps);

            client.GetObjectRDMAAsync(request, rdmaPtr(hostBuf, gpuBuf, len),
                [&promise](const S3Client*, const GetObjectRequest&, Aws::S3::RdmaPtr&& ptr,
                    GetObjectOutcome&& outcome,
                    const std::shared_ptr<const Aws::Client::AsyncCallerContext>&)
                {
                    ptr.release();
                    promise.set_value(std::move(outcome) );
                } );
        }

        static size_t getBytesReceived(const GetObjectOutcome& outcome)
        {
            return outcome.GetResult().GetRDMABytesTransferred();
        }

    private:
        bool useGpuBufs{false}; // transfer directly from/to the GPU buffers
        bool bufsRegistered{false}; // buffers are pre-registered for the whole run
        std::vector<Aws::S3::RdmaPtr> registrations; // owning registrations of the I/O buffers

        Aws::S3::RdmaPtr rdmaPtr(char* hostBuf, char* gpuBuf, size_t len) const
        {
            static char emptyBuf; // the fork rejects a NULL pointer, so 0-byte objects get this

            if(!len)
                return Aws::S3::RdmaPtr(&emptyBuf, 0, true);

            return Aws::S3::RdmaPtr(useGpuBufs ? gpuBuf : hostBuf, len, bufsRegistered);
        }

        template <typename REQUEST>
        static void setSentHandler(REQUEST& request, AtomicLiveOps& liveOps)
        {
            request.SetDataSentEventHandler(
                [&liveOps](const Aws::Http::HttpRequest*, long long numBytes)
                { liveOps.numBytesDone += numBytes; } );
        }

        static void setReceivedHandler(GetObjectRequest& request, AtomicLiveOps& liveOps)
        {
            request.SetDataReceivedEventHandler(
                [&liveOps](const Aws::Http::HttpRequest*, Aws::Http::HttpResponse*,
                    long long numBytes)
                { liveOps.numBytesDone += numBytes; } );
        }
};

typedef S3RdmaCloudianTransport S3Transport;

#endif // S3RDMA_CLOUDIAN_S3RDMACLOUDIANTRANSPORT_H_
