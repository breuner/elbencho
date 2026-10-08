// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <array>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "Common.h"
#include "Logger.h"
#include "modes/s3/toolkits/S3AclTk.h"
#include "modes/s3/S3Mode.h"
#include "modes/s3/S3ProgArgs.h"
#include "modes/s3/S3Transport.h"
#include "ProgArgs.h"
#include "PathStore.h"
#include "toolkits/random/RandAlgoGoldenPrime.h"
#include "toolkits/random/RandAlgoSelectorTk.h"
#include "modes/s3/toolkits/S3Tk.h"
#include "toolkits/StringTk.h"
#include "toolkits/TranslatorTk.h"
#include "workers/LocalWorker.h"
#include "workers/WorkerException.h"
#include "workers/WorkersSharedData.h"

#ifdef S3_SUPPORT
    #include <aws/core/auth/AWSCredentialsProvider.h>
    #include <aws/core/utils/HashingUtils.h>
    #include <aws/core/utils/memory/stl/AWSString.h>
    #include <aws/core/utils/StringUtils.h>
    #include <aws/core/utils/threading/Executor.h>
    #include <aws/core/utils/UUID.h>
    #include INCLUDE_AWS_S3(model/AbortMultipartUploadRequest.h)
    #include INCLUDE_AWS_S3(model/BucketLocationConstraint.h)
    #include INCLUDE_AWS_S3(model/CompleteMultipartUploadRequest.h)
    #include INCLUDE_AWS_S3(model/CreateBucketRequest.h)
    #include INCLUDE_AWS_S3(model/CreateMultipartUploadRequest.h)
    #include INCLUDE_AWS_S3(model/DeleteBucketRequest.h)
    #include INCLUDE_AWS_S3(model/DeleteBucketTaggingRequest.h)
    #include INCLUDE_AWS_S3(model/DeleteObjectRequest.h)
    #include INCLUDE_AWS_S3(model/DeleteObjectTaggingRequest.h)
    #include INCLUDE_AWS_S3(model/DeleteObjectsRequest.h)
    #include INCLUDE_AWS_S3(model/GetBucketAclRequest.h)
    #include INCLUDE_AWS_S3(model/GetBucketTaggingRequest.h)
    #include INCLUDE_AWS_S3(model/GetObjectAclRequest.h)
    #include INCLUDE_AWS_S3(model/GetObjectRequest.h)
    #include INCLUDE_AWS_S3(model/GetObjectTaggingRequest.h)
    #include INCLUDE_AWS_S3(model/GetObjectLockConfigurationRequest.h)
    #include INCLUDE_AWS_S3(model/GetBucketVersioningRequest.h)
    #include INCLUDE_AWS_S3(model/HeadObjectRequest.h)
    #include INCLUDE_AWS_S3(model/HeadBucketRequest.h)
    #include INCLUDE_AWS_S3(model/ListObjectsV2Request.h)
    #include INCLUDE_AWS_S3(model/ListPartsRequest.h)
    #include INCLUDE_AWS_S3(model/Object.h)
    #include INCLUDE_AWS_S3(model/ObjectLockRule.h)
    #include INCLUDE_AWS_S3(model/PutBucketAclRequest.h)
    #include INCLUDE_AWS_S3(model/PutBucketTaggingRequest.h)
    #include INCLUDE_AWS_S3(model/PutObjectAclRequest.h)
    #include INCLUDE_AWS_S3(model/PutObjectRequest.h)
    #include INCLUDE_AWS_S3(model/PutObjectTaggingRequest.h)
    #include INCLUDE_AWS_S3(model/PutObjectLockConfigurationRequest.h)
    #include INCLUDE_AWS_S3(model/PutBucketVersioningRequest.h)
    #include INCLUDE_AWS_S3(model/UploadPartRequest.h)
#endif

#define TAG_CHECKSUM_LEN        2

#define TAG_KEY_MEDIUM_NAME     "medium"
#define TAG_KEY_LONG_NAME       "long"
#define TAG_VALUE_MEDIUM_LEN    100
#define TAG_VALUE_LONG_LEN      256

#define RETENTION_PERIOD_DAYS   1


/**
 * Call .get() on a std::future. Ignores result and any exceptions, so typically only good for
 * discarding results after a previous error. Calling .get() twice is not allowed, so we check
 * .valid() first here.
 */
#define SAFE_FUTURE_GET_IGNORE_ERR(future) \
    do { try { if(future.valid() ) future.get(); } catch(...) {} } while(0)


#ifdef S3_SUPPORT
    #ifdef S3_AWSCRT
        namespace S3 = Aws::S3Crt::Model;
    #else
        namespace S3 = Aws::S3::Model;
        using S3Errors = Aws::S3::S3Errors;
    #endif // S3_AWSCRT

    S3UploadStore S3Mode::s3SharedUploadStore; // singleton for shared uploads

    /**
     * Context variables for asynchronous GetObject requests.
     */
    class S3AsyncDownloadContext
    {
        public:

        /**
         * Note that this does not init all members - some need to be set later (because the
         * values are not ready yet at the time of object creation).
         */
        S3AsyncDownloadContext(uint64_t currentOffset, size_t blockSize) :
            currentOffset(currentOffset), blockSize(blockSize),
            partCompleteFuture(partCompletePromise.get_future() )
        {}

        uint64_t currentOffset;
        size_t blockSize;

        std::chrono::steady_clock::time_point ioStartT;

        S3Transport::GetObjectRequest request;
        std::promise<S3Transport::GetObjectOutcome> partCompletePromise;
        std::future<S3Transport::GetObjectOutcome> partCompleteFuture;
    };

    /**
     * Context variables for parts of a asynchronous multipart upload.
     */
    class S3AsyncUploadPartContext
    {
        public:

        /**
         * Note that this does not init all members - some need to be set later (because the
         * values are not ready yet at the time of object creation).
         */
        S3AsyncUploadPartContext(uint64_t currentOffset, size_t blockSize, size_t partNum) :
            currentOffset(currentOffset), blockSize(blockSize), partNum(partNum),
            partCompleteFuture(partCompletePromise.get_future() )
        {
            completedPart.SetPartNumber(partNum);
        }

        uint64_t currentOffset;
        size_t blockSize;
        size_t partNum;

        std::chrono::steady_clock::time_point ioStartT;

        S3Transport::UploadPartRequest uploadPartRequest;
        std::promise<S3Transport::UploadPartOutcome> partCompletePromise;
        std::future<S3Transport::UploadPartOutcome> partCompleteFuture;
        S3::CompletedPart completedPart;
    };

#endif // S3_SUPPORT


S3Mode::S3Mode(LocalWorker& worker) :
    worker(worker), progArgs(worker.progArgs), s3Args(worker.progArgs->getS3Args() ),
    workerRank(worker.workerRank), opsLog(worker.opsLog)
{
}

/**
 * Initialize AWS S3 SDK & S3 client object. Intended to be called at the start of each benchmark
 * phase. Will do nothing if not built with S3 support or no S3 endpoints defined.
 *
 * S3 endpoints get assigned round-robin to workers based on workerRank.
 *
 * @throw WorkerException on error
 */
void S3Mode::init()
{
#ifdef S3_SUPPORT

    s3SharedUploadStore.setProgArgs(progArgs, workerRank);

    if(progArgs->getBenchMode() != BenchMode_S3)
        return; // nothing to do

    if(s3Args.getUseS3ClientSingleton() )
    { // using shared singleton s3 client instead of per-worker s3 client instances
        s3Client = s3Args.getS3ClientSingleton();
        s3EndpointStr = s3Args.getS3SingletonEndpointStr();
    }
    else
    { // using per-worker s3 client instances
        s3Client = S3Tk::initS3Client(progArgs, workerRank, &worker.isInterruptionRequested,
            &s3EndpointStr);
    }

    useS3SSE = s3Args.getUseS3SSE();

    s3SSECKey = s3Args.getS3SSECKey();
    if(!s3SSECKey.empty() )
        s3SSECKeyMD5 = S3Tk::computeKeyMD5(s3SSECKey);

    s3SSEKMSKey = s3Args.getS3SSEKMSKey();

    if(s3Args.getS3ChecksumAlgo().empty() )
        s3ChecksumAlgorithm = S3ChecksumAlgorithm::NOT_SET;
    else
    {
        /* GetChecksumAlgorithmForName returns hash value for invalid algo names, so we need to
            confirm the algo by checking the valid algo enum values. */

        s3ChecksumAlgorithm = S3ChecksumAlgorithmMapper::GetChecksumAlgorithmForName(
            s3Args.getS3ChecksumAlgo() );

        switch(s3ChecksumAlgorithm)
        {
            case S3ChecksumAlgorithm::CRC32:
            case S3ChecksumAlgorithm::CRC32C:
            case S3ChecksumAlgorithm::SHA1:
            case S3ChecksumAlgorithm::SHA256:
            //case S3ChecksumAlgorithm::CRC64NVME: // crc64nvme not supported by aws sdk currently
                // because of missing uploadPartRequest.SetChecksumCRC64NVME for
                // S3Tk::addUploadPartRequestChecksum()
                break;
            default:
                throw WorkerException(std::string("Invalid S3 checksum algorithm: ") +
                    s3Args.getS3ChecksumAlgo() );
        }
    }

    transport.init(*s3Client, worker.ioBufVec, worker.gpuIOBufVec, *progArgs, workerRank);

#endif // S3_SUPPORT
}

/**
 * Free S3 client object. Intended to be called at the end of each benchmark phase.
 */
void S3Mode::uninit()
{
#ifdef S3_SUPPORT

    if(progArgs->getBenchMode() != BenchMode_S3)
        return; // nothing to do

    transport.uninit();

    // s3Client is a std::shared_ptr, so reset() will cleanup the client object
    // (note: this could also be the shared singleton s3 client from ProgArgs)
    s3Client.reset();

#endif // S3_SUPPORT
}

/**
 * Iterate over all buckets to create or remove them. Each worker processes its own subset of
 * buckets.
 *
 * @throw WorkerException on error.
 */
void S3Mode::iterateBuckets()
{
#ifndef S3_SUPPORT
    throw WorkerException(std::string(__func__) + "called, but this was built without S3 support");
#else

    const StringVec& bucketVec = progArgs->getBenchPaths();
    const size_t numBuckets = bucketVec.size();
    const size_t numDataSetThreads = progArgs->getNumDataSetThreads();

    worker.workerGotPhaseWork = false; // not all workers might get work

    for(unsigned bucketIndex = workerRank;
        bucketIndex < numBuckets;
        bucketIndex += numDataSetThreads)
    {
        worker.checkInterruptionRequest();

        worker.workerGotPhaseWork = true;
        const auto& bucketName = bucketVec[bucketIndex];

        std::chrono::steady_clock::time_point ioStartT = std::chrono::steady_clock::now();

        if(worker.benchPhase == BenchPhase_CREATEDIRS)
            createBucket(bucketName);

        if(worker.benchPhase == BenchPhase_PUT_S3_BUCKET_MD)
        {
            if (s3Args.getDoS3BucketTagging())
                createBucketTagging(bucketName);

            if (s3Args.getDoS3ObjectLockConfiguration())
                putObjectLockConfiguration(bucketName);

            if (s3Args.getDoS3BucketVersioning())
                putBucketVersioning(bucketName, false);
        }

        if(worker.benchPhase == BenchPhase_PUTBUCKETACL)
            putBucketAcl(bucketName);

        if(worker.benchPhase == BenchPhase_GETBUCKETACL)
            getBucketAcl(bucketName);

        if(worker.benchPhase == BenchPhase_STATDIRS)
            headBucket(bucketName);

        if(worker.benchPhase == BenchPhase_GET_S3_BUCKET_MD)
        {
            if (s3Args.getDoS3BucketTagging())
                getBucketTagging(bucketName);

            if (s3Args.getDoS3ObjectLockConfiguration())
                getObjectLockConfiguration(bucketName);

            if (s3Args.getDoS3BucketVersioning())
                getBucketVersioning(bucketName);
        }

        if(worker.benchPhase == BenchPhase_DEL_S3_BUCKET_MD)
        {
            if (s3Args.getDoS3BucketVersioning())
                putBucketVersioning(bucketName, true);

            // Disable lock configuration
            if (s3Args.getDoS3ObjectLockConfiguration())
                putObjectLockConfiguration(bucketName, true);

            if (s3Args.getDoS3BucketTagging())
                deleteBucketTagging(bucketName);
        }

        // delete buckets
        if(worker.benchPhase == BenchPhase_DELETEDIRS)
            deleteBucket(bucketName);

        // calc entry operations latency
        std::chrono::steady_clock::time_point ioEndT = std::chrono::steady_clock::now();
        std::chrono::microseconds ioElapsedMicroSec =
            std::chrono::duration_cast<std::chrono::microseconds>
            (ioEndT - ioStartT);

        worker.entriesLatHisto.addLatency(ioElapsedMicroSec.count() );

        worker.atomicLiveOps.numEntriesDone++;
    }

#endif // S3_SUPPORT
}

/**
 * This is for s3 mode. Iterate over all objects to create/read/remove them.
 * By default, this uses a unique "dir" (i.e. prefix with slashes inside a bucket) per worker and
 * fills up each dir before moving on to the next. If dir sharing is enabled, all workers will use
 * dirs of rank 0.
 *
 * @throw WorkerException on error.
 */
void S3Mode::iterateObjects()
{
#ifndef S3_SUPPORT
    throw WorkerException(std::string(__func__) + "called, but this was built without S3 support");
#else

    if( (worker.benchPhase == BenchPhase_READFILES) && s3Args.getUseS3RandObjSelect() )
    {
        iterateObjectsRand();
        return;
    }

    const bool haveSubdirs = (progArgs->getNumDirs() > 0);
    const size_t numDirs = haveSubdirs ? progArgs->getNumDirs() : 1; // set 1 to run dir loop once
    const size_t numFiles = progArgs->getNumFiles();
    const uint64_t fileSize = progArgs->getFileSize();
    const size_t blockSize = progArgs->getBlockSize();
    const StringVec& bucketVec = progArgs->getBenchPaths();
    std::array<char, PATH_BUF_LEN> currentPath;
    const size_t workerDirRank = progArgs->getDoDirSharing() ? 0 : workerRank; /* for dir sharing,
        all workers use the dirs of worker rank 0 */
    std::string objectPrefix = s3Args.getS3ObjectPrefix();
    const bool objectPrefixRand = s3Args.getUseS3ObjectPrefixRand();
    const BenchPhase globalBenchPhase = worker.workersSharedData->currentBenchPhase;
    const size_t localWorkerRank = workerRank - progArgs->getRankOffset();
    const bool isRWMixedReader = ( (globalBenchPhase == BenchPhase_CREATEFILES) &&
        (localWorkerRank < progArgs->getNumRWMixReadThreads() ) );

    // walk over each unique dir per worker

    for(size_t dirIndex = 0; dirIndex < numDirs; dirIndex++)
    {
        // occasional interruption check
        IF_UNLIKELY( (dirIndex % INTERRUPTION_CHECK_INTERVAL) == 0)
            worker.checkInterruptionRequest();

        // fill up this dir with all files before moving on to the next dir

        for(size_t fileIndex = 0; fileIndex < numFiles; fileIndex++)
        {
            // occasional interruption check
            IF_UNLIKELY( (fileIndex % INTERRUPTION_CHECK_INTERVAL) == 0)
                worker.checkInterruptionRequest();

            // generate current dir path
            int printRes;

            if(haveSubdirs)
                printRes = snprintf(currentPath.data(), PATH_BUF_LEN, "r%zu/d%zu/r%zu-f%zu",
                    workerDirRank, dirIndex, workerRank, fileIndex);
            else
                printRes = snprintf(currentPath.data(), PATH_BUF_LEN, "r%zu-f%zu",
                    workerRank, fileIndex);

            IF_UNLIKELY(printRes >= PATH_BUF_LEN)
                throw WorkerException("object path too long for static buffer. "
                    "Buffer size: " + std::to_string(PATH_BUF_LEN) + "; "
                    "workerRank: " + std::to_string(workerRank) + "; "
                    "dirIndex: " + std::to_string(dirIndex) + "; "
                    "fileIndex: " + std::to_string(fileIndex) );

            if(objectPrefixRand)
                objectPrefix = getRandObjectPrefix(
                    workerRank, dirIndex, fileIndex, s3Args.getS3ObjectPrefix() );

            unsigned bucketIndex = (workerRank + dirIndex) % bucketVec.size();
            std::string currentObjectPath = objectPrefix + currentPath.data();


            worker.rwOffsetGen->reset(); // reset for next file

            std::chrono::steady_clock::time_point ioStartT = std::chrono::steady_clock::now();

            if( (worker.benchPhase == BenchPhase_CREATEFILES) && !isRWMixedReader)
            {
                if(blockSize < fileSize)
                    uploadObjectMultiPart(bucketVec[bucketIndex], currentObjectPath);
                else
                    uploadObjectSinglePart(bucketVec[bucketIndex], currentObjectPath);
            }

            if (worker.benchPhase == BenchPhase_PUT_S3_OBJECT_MD)
            {
                if (s3Args.getDoS3ObjectTagging())
                    putObjectTags(bucketVec[bucketIndex], currentObjectPath);
            }

            if( (worker.benchPhase == BenchPhase_READFILES) || isRWMixedReader)
            {
                downloadObject(bucketVec[bucketIndex], currentObjectPath,
                    isRWMixedReader);
            }

            if(worker.benchPhase == BenchPhase_STATFILES)
                statObject(bucketVec[bucketIndex], currentObjectPath);

            if(worker.benchPhase == BenchPhase_GET_S3_OBJECT_MD)
            {
                if (s3Args.getDoS3ObjectTagging())
                    getObjectTags(bucketVec[bucketIndex], currentObjectPath);
            }

            if(worker.benchPhase == BenchPhase_PUTOBJACL)
                putObjectAcl(bucketVec[bucketIndex], currentObjectPath);

            if(worker.benchPhase == BenchPhase_GETOBJACL)
                getObjectAcl(bucketVec[bucketIndex], currentObjectPath);

            if(worker.benchPhase == BenchPhase_DEL_S3_OBJECT_MD)
            {
                if (s3Args.getDoS3ObjectTagging())
                    deleteObjectTags(bucketVec[bucketIndex], currentObjectPath);
            }

            if(worker.benchPhase == BenchPhase_DELETEFILES)
                deleteObject(bucketVec[bucketIndex], currentObjectPath);

            // calc entry operations latency. (for create, this includes open/rw/close.)
            std::chrono::steady_clock::time_point ioEndT = std::chrono::steady_clock::now();
            std::chrono::microseconds ioElapsedMicroSec =
                std::chrono::duration_cast<std::chrono::microseconds>
                (ioEndT - ioStartT);

            // entry lat & num done count
            if(isRWMixedReader)
            {
                worker.entriesLatHistoReadMix.addLatency(ioElapsedMicroSec.count() );
                worker.atomicLiveOpsReadMix.numEntriesDone++;
            }
            else
            {
                worker.entriesLatHisto.addLatency(ioElapsedMicroSec.count() );
                worker.atomicLiveOps.numEntriesDone++;
            }

        } // end of files for loop
    } // end of dirs for loop

#endif // S3_SUPPORT
}

/**
 * This is for s3 mode with custom tree. Iterate over all objects to create/read/remove them. Each
 * worker uses a subset of the files from the non-shared tree and parts of files from the shared
 * tree.
 *
 * Note: With a custom tree, multiple benchmark paths are not supported. This is a limitation of
 * dirModeIterateCustomFiles() and we keep the same limitation here for compatibility.
 *
 * @throw WorkerException on error.
 */
void S3Mode::iterateCustomObjects()
{
#ifndef S3_SUPPORT
    throw WorkerException(std::string(__func__) + "called, but this was built without S3 support");
#else

    const std::string bucketName = progArgs->getBenchPaths()[0];
    const size_t blockSize = progArgs->getBlockSize();
    const bool useS3MpuSharing = s3Args.getUseS3MPUSharing();
    const PathList& customTreePaths = worker.customTreeFiles.getPaths();
    const BenchPhase globalBenchPhase = worker.workersSharedData->currentBenchPhase;
    const size_t localWorkerRank = workerRank - progArgs->getRankOffset();
    const bool isRWMixedReader = ( (globalBenchPhase == BenchPhase_CREATEFILES) &&
        (localWorkerRank < progArgs->getNumRWMixReadThreads() ) );
    std::string objectPrefix = s3Args.getS3ObjectPrefix();


    // check if this worker has anything to do in this round
    IF_UNLIKELY(customTreePaths.empty() )
    {
        LOGGER(Log_DEBUG, "got no work in this round. workerRank: " << workerRank << std::endl);

        worker.workerGotPhaseWork = false;
        return;
    }


    unsigned short numFilesDone = 0; // just for occasional interruption check (so "short" is ok)

    // walk over custom tree part of this worker

    for(const PathStoreElem& currentPathElem : customTreePaths)
    {
        // occasional interruption check
        if( (numFilesDone % INTERRUPTION_CHECK_INTERVAL) == 0)
            worker.checkInterruptionRequest();

        std::chrono::steady_clock::time_point ioStartT = std::chrono::steady_clock::now();

        if( (worker.benchPhase == BenchPhase_CREATEFILES) ||
            (worker.benchPhase == BenchPhase_READFILES) )
        {
            const uint64_t fileSize = currentPathElem.totalLen;
            const uint64_t fileOffset = currentPathElem.rangeStart;
            const uint64_t rangeLen = currentPathElem.rangeLen;

            worker.rwOffsetGen->reset(rangeLen, fileOffset);

            LOGGER_DEBUG_BUILD(__func__ << ":" << __LINE__ << ": " <<
                "obj: " << objectPrefix + currentPathElem.path << "; "
                "rangeStart: " << fileOffset << "; "
                "rangeLen: " << rangeLen << "; " <<
                "fSize: " << fileSize << "; " << std::endl);

            if(worker.benchPhase == BenchPhase_CREATEFILES)
            {
                // (useS3MpuSharing is to prevent creation of new MPU IDs outside of S3UploadStore)
                if( (rangeLen < fileSize) || useS3MpuSharing)
                    uploadObjectMultiPartShared(bucketName,
                        objectPrefix + currentPathElem.path, fileSize);
                else
                { // this worker uploads the whole object
                    if(blockSize < fileSize)
                        uploadObjectMultiPart(bucketName,
                            objectPrefix + currentPathElem.path);
                    else
                        uploadObjectSinglePart(bucketName,
                            objectPrefix + currentPathElem.path);
                }
            }

            if(worker.benchPhase == BenchPhase_READFILES)
                downloadObject(bucketName, objectPrefix + currentPathElem.path, false);
        }

        if(worker.benchPhase == BenchPhase_STATFILES)
            statObject(bucketName, objectPrefix + currentPathElem.path);

        if(worker.benchPhase == BenchPhase_PUTOBJACL)
            putObjectAcl(bucketName, objectPrefix + currentPathElem.path);

        if(worker.benchPhase == BenchPhase_GETOBJACL)
            getObjectAcl(bucketName, objectPrefix + currentPathElem.path);

        if(worker.benchPhase == BenchPhase_DELETEFILES)
            deleteObject(bucketName, objectPrefix + currentPathElem.path);

        // calc entry operations latency. (for create, this includes open/rw/close.)
        std::chrono::steady_clock::time_point ioEndT = std::chrono::steady_clock::now();
        std::chrono::microseconds ioElapsedMicroSec =
            std::chrono::duration_cast<std::chrono::microseconds>
            (ioEndT - ioStartT);

        // entry lat & num done count
        if(currentPathElem.totalLen == currentPathElem.rangeLen)
        { // entry lat & done is only meaningful for fully processed entries
            if(isRWMixedReader)
            {
                worker.entriesLatHistoReadMix.addLatency(ioElapsedMicroSec.count() );
                worker.atomicLiveOpsReadMix.numEntriesDone++;
            }
            else
            {
                worker.entriesLatHisto.addLatency(ioElapsedMicroSec.count() );
                worker.atomicLiveOps.numEntriesDone++;
            }
        }

        numFilesDone++;

    } // end of tree elements for-loop

#endif // S3_SUPPORT
}

/**
 * Iterate over all MPU IDs from service MPU sharing mode and mark them as completed. Each worker
 * processes its own subset of MPU IDs.
 *
 * @throw WorkerException on error.
 */
 void S3Mode::iterateAndCompleteMpuIDs()
 {
 #ifndef S3_SUPPORT
     throw WorkerException(std::string(__func__) + "called, but this was built without S3 support");
 #else

     const StringVec& bucketVec = progArgs->getBenchPaths();
     const size_t numBuckets = bucketVec.size();
     const std::string bucketName = bucketVec.empty() ? "" : bucketVec[0];
     const size_t numDataSetThreads = progArgs->getNumDataSetThreads();
     const PathList& pathList = progArgs->getCustomTreeFilesShared().getPaths();
     const size_t pathListLen = pathList.size();
     const std::string objectPrefix = s3Args.getS3ObjectPrefix();
     const uint64_t objectSize = progArgs->getFileSize();

     worker.workerGotPhaseWork = false; // not all workers might get work

     // sanity check: should never happen
     IF_UNLIKELY(!s3Args.getUseS3MPUSharing() )
        throw WorkerException("MPU sharing completion phase started without MPU sharing being "
            "selected.");

     // sanity check: should never happen
     IF_UNLIKELY(numBuckets != 1)
        throw WorkerException("MPU sharing completion phase detected invalid number of buckets. "
            "NumBuckets: " + std::to_string(numBuckets) );

    // each worker rank completes its own subset of MPU IDs
    unsigned pathIndex = workerRank;
    for(PathListCIter pathListIter = std::next(pathList.begin(), pathIndex);
         pathIndex < pathListLen;
         pathIndex += numDataSetThreads, std::advance(pathListIter, numDataSetThreads) )
    {
        worker.checkInterruptionRequest();

        worker.workerGotPhaseWork = true;

        std::chrono::steady_clock::time_point ioStartT = std::chrono::steady_clock::now();

        const std::string objectName = objectPrefix + pathListIter->path;

        std::string uploadID = s3SharedUploadStore.getMultipartUploadID(bucketName, objectName,
            s3Client, opsLog);

        queryAndFinishMultipartUpload(bucketName, objectName, uploadID, objectSize);

        // calc entry operations latency
        std::chrono::steady_clock::time_point ioEndT = std::chrono::steady_clock::now();
        std::chrono::microseconds ioElapsedMicroSec =
            std::chrono::duration_cast<std::chrono::microseconds>
            (ioEndT - ioStartT);

        worker.entriesLatHisto.addLatency(ioElapsedMicroSec.count() );

        worker.atomicLiveOps.numEntriesDone++;
    }

 #endif // S3_SUPPORT
 }

/**
 * Service instances with a shared custom tree upload each object only among their own threads,
 * because the uploadID is not shared between service instances. In this case ProgArgs prepared a
 * tree with only this service's files, so the sublist is based on local worker ranks.
 * (Note: ProgArgs uses the same condition in prepareCustomTreePathStores().)
 *
 * @return true if worker.customTreeFiles got the shared files, false if this case does not apply.
 */
bool S3Mode::prepareCustomTreePathStores(bool throwOnSmallerThanBlockSize)
{
    const size_t numThreads = progArgs->getNumThreads();
    const size_t numDataSetThreads = progArgs->getNumDataSetThreads();

    if(!progArgs->getRunAsService() || (progArgs->getBenchMode() != BenchMode_S3) ||
        !progArgs->getRunCreateFilesPhase() || (numDataSetThreads == numThreads) ||
        s3Args.getUseS3MPUSharing() )
        return false;

    const size_t workerRankLocal = workerRank - progArgs->getRankOffset();

    progArgs->getCustomTreeFilesShared().getWorkerSublistShared(workerRankLocal,
        numThreads, throwOnSmallerThanBlockSize, worker.customTreeFiles);

    return true;
}


#ifdef S3_SUPPORT

/**
 * Throw an informative WorkerException if the s3 request outcome has the error flag set.
 *
 * @outcome s3 request outcome.
 * @failMessage human-friendly error message, e.g. "Object upload failed."
 * @objectName name of object to which this error applies, can be empty.
 * @throw WorkerException on error.
 */
template <typename R>
void S3Mode::throwOnError(
        const Aws::Utils::Outcome<R, S3ErrorType>& outcome,
        const std::string& failMessage,
        const std::string& bucketName,
        const std::string& objectName)
{
    IF_LIKELY(outcome.IsSuccess() )
        return;

    const auto s3Error = outcome.GetError();

    std::stringstream errStr;
        errStr << failMessage << std::endl <<
        "Endpoint: " << s3EndpointStr << std::endl <<
        "Bucket: " << bucketName << std::endl <<
        (objectName.empty() ? std::string("") : ("Object: " + objectName + "\n" )) <<
        "Exception: " << s3Error.GetExceptionName() << std::endl <<
        "Message: " << s3Error.GetMessage() << std::endl <<
        "HTTP Error Code: " << (int)s3Error.GetResponseCode() << " (" <<
            TranslatorTk::httpErrorCodeToHumanStr( (int)s3Error.GetResponseCode() ) << ")" <<
            std::endl <<
        "Request ID: " << s3Error.GetRequestId() << std::endl;

    throw WorkerException(errStr.str() );
}
#endif // S3_SUPPORT

/**
 * Abort unfinished shared multipart uploads e.g. after interruption or error. This does not throw
 * an exception on error, because it's intended for the cleanup after a previous error or
 * interruption.
 */
void S3Mode::abortUnfinishedSharedUploads()
{
#ifdef S3_SUPPORT

    bool useS3MpuSharing = s3Args.getUseS3MPUSharing();

    // loop until no more unfinished objects are returned
    for( ; ; )
    {
        std::string bucketName;
        std::string objectName;
        std::string uploadID;

        s3SharedUploadStore.getNextUnfinishedUpload(bucketName, objectName, uploadID);

        if(uploadID.empty() )
            return; // no unfinished uploads left to abort

        if(!s3Client)
        { // this can happen if an exception gets thrown in preparePhase() before initS3Client()
            LOGGER(Log_DEBUG, "Skipping abort attempt of unfinished S3 MPUs because S3 client not "
                "initialized");
            return;
        }

        LOGGER(Log_DEBUG, "Aborting unfinished shared multipart upload. "
            "Rank: " << workerRank << "; "
            "Endpoint: " << s3EndpointStr << "; "
            "Bucket: " << bucketName << "; "
            "Object: " << objectName << "; " << std::endl);

        bool abortSuccess = abortMultipartUpload(bucketName, objectName, uploadID);

        if(abortSuccess)
            continue;

        // aborting unfinished upload failed
        // (this is normal in svc mpu sharing mode, where all services cancel all shared mpu ids)
        ERRLOGGER(useS3MpuSharing ? Log_DEBUG : Log_NORMAL,
            "Aborting unfinished shared multipart upload failed. "
            "Rank: " << workerRank << "; "
            "Endpoint: " << s3EndpointStr << "; "
            "Bucket: " << bucketName << "; "
            "Object: " << objectName << "; "
            "UploadID: " << uploadID << "; " << std::endl);
    }

#endif // S3_SUPPORT
}

/**
 * List objects in given buckets with user-defined limit for number of entries.
 *
 * @throw WorkerException on error.
 */
void S3Mode::listObjects()
{
#ifndef S3_SUPPORT
    throw WorkerException(std::string(__func__) + "called, but this was built without S3 support");
#else

    const StringVec& bucketVec = progArgs->getBenchPaths();
    const size_t numBuckets = bucketVec.size();
    const size_t numDataSetThreads = progArgs->getNumDataSetThreads();
    std::string objectPrefix = s3Args.getS3ObjectPrefix();

    worker.workerGotPhaseWork = false; // not all workers might get work

    for(unsigned bucketIndex = workerRank;
        bucketIndex < numBuckets;
        bucketIndex += numDataSetThreads)
    {
        uint64_t numObjectsLeft = s3Args.getS3ListObjNum();
        std::string nextContinuationToken;
        bool isTruncated; // true if S3 server reports more objects left to retrieve

        worker.workerGotPhaseWork = true;

        do
        {
            worker.checkInterruptionRequest();

            std::chrono::steady_clock::time_point ioStartT = std::chrono::steady_clock::now();

            S3::ListObjectsV2Request request;
            request.SetBucket(bucketVec[bucketIndex] );
            request.SetPrefix(objectPrefix);
            request.SetMaxKeys( (numObjectsLeft > 1000) ? 1000 : numObjectsLeft); // can't be >1000

            if(!nextContinuationToken.empty() )
                request.SetContinuationToken(nextContinuationToken);

            OPLOG_PRE_OP("S3ListObjectsV2", bucketVec[bucketIndex] + "/" + objectPrefix, 0,
                request.GetMaxKeys() );

            S3::ListObjectsV2Outcome outcome = s3Client->ListObjectsV2(request);

            OPLOG_POST_OP("S3ListObjectsV2", bucketVec[bucketIndex] + "/" + objectPrefix, 0,
                outcome.GetResult().GetKeyCount(), !outcome.IsSuccess() );

            IF_UNLIKELY(!outcome.IsSuccess() )
            {
                auto s3Error = outcome.GetError();

                throw WorkerException(std::string("Object listing v2 failed. ") +
                    "Endpoint: " + s3EndpointStr + "; "
                    "Bucket: " + bucketVec[bucketIndex] + "; "
                    "Prefix: " + objectPrefix + "; "
                    "ContinuationToken: " + nextContinuationToken + "; "
                    "NumObjectsLeft: " + std::to_string(numObjectsLeft) + "; "
                    "Exception: " + s3Error.GetExceptionName() + "; " +
                    "Message: " + s3Error.GetMessage() + "; " +
                    "HTTP Error Code: " + std::to_string( (int)s3Error.GetResponseCode() ) + " (" +
                        TranslatorTk::httpErrorCodeToHumanStr( (int)s3Error.GetResponseCode() ) +
                        "); " +
                    "Request ID: " + s3Error.GetRequestId() );
            }

            // calc entry operations latency
            std::chrono::steady_clock::time_point ioEndT = std::chrono::steady_clock::now();
            std::chrono::microseconds ioElapsedMicroSec =
                std::chrono::duration_cast<std::chrono::microseconds>
                (ioEndT - ioStartT);

            worker.entriesLatHisto.addLatency(ioElapsedMicroSec.count() );

            unsigned keyCount = outcome.GetResult().GetKeyCount();

            worker.atomicLiveOps.numEntriesDone += keyCount;
            numObjectsLeft -= keyCount;

            nextContinuationToken = outcome.GetResult().GetNextContinuationToken();
            isTruncated = outcome.GetResult().GetIsTruncated();

        } while(isTruncated && numObjectsLeft); // end of while numObjectsLeft loop
    }

#endif // S3_SUPPORT
}

/**
 * This is for s3 mode parallel listing of objects. Expects a dataset created via
 * iterateObjects() and uses different prefixes per worker thread to parallelize, so that each
 * worker requests the listing of the dirs/objs that it created in iterateObjects().
 *
 * @throw WorkerException on error.
 */
void S3Mode::listObjParallel()
{
#ifndef S3_SUPPORT
    throw WorkerException(std::string(__func__) + "called, but this was built without S3 support");
#else

    const bool haveSubdirs = (progArgs->getNumDirs() > 0);
    const size_t numDirs = haveSubdirs ? progArgs->getNumDirs() : 1; // set 1 to run dir loop once
    const size_t numFiles = progArgs->getNumFiles();
    const StringVec& bucketVec = progArgs->getBenchPaths();
    std::array<char, PATH_BUF_LEN> currentPath;
    const size_t workerDirRank = progArgs->getDoDirSharing() ? 0 : workerRank; /* for dir sharing,
        all workers use the dirs of worker rank 0 */
    std::string objectPrefix = s3Args.getS3ObjectPrefix();
    const bool objectPrefixRand = s3Args.getUseS3ObjectPrefixRand();
    const bool doListObjVerify = s3Args.getDoListObjVerify();


    // walk over each unique dir per worker

    for(size_t dirIndex = 0; dirIndex < numDirs; dirIndex++)
    {
        uint64_t numObjectsLeft = numFiles;
        std::string nextContinuationToken;
        bool isTruncated; // true if S3 server reports more objects left to retrieve
        StringList receivedObjs; // for verification (if requested by user)
        StringSet expectedObjs; // for verification (if requested by user)

        // generate list prefix for current dir
        int printRes;

        if(haveSubdirs)
            printRes = snprintf(currentPath.data(), PATH_BUF_LEN, "r%zu/d%zu/r%zu-",
                        workerDirRank, dirIndex, workerRank);
        else
            printRes = snprintf(currentPath.data(), PATH_BUF_LEN, "r%zu-",
                        workerRank);

        IF_UNLIKELY(printRes >= PATH_BUF_LEN)
            throw WorkerException("object path too long for static buffer. "
                "Buffer size: " + std::to_string(PATH_BUF_LEN) + "; "
                "workerRank: " + std::to_string(workerRank) + "; "
                "dirIndex: " + std::to_string(dirIndex) );

        unsigned bucketIndex = (workerRank + dirIndex) % bucketVec.size();
        std::string currentListPrefix = objectPrefix + currentPath.data();

        // build list of expected objs in dir for verification. (std::set for alphabetic order)
        for(size_t fileIndex = 0; doListObjVerify && (fileIndex < numFiles); fileIndex++)
        {
            int printRes;

            if(haveSubdirs)
                printRes = snprintf(currentPath.data(), PATH_BUF_LEN, "r%zu/d%zu/r%zu-f%zu",
                    workerDirRank, dirIndex, workerRank, fileIndex);
            else
                printRes = snprintf(currentPath.data(), PATH_BUF_LEN, "r%zu-f%zu",
                    workerRank, fileIndex);

            IF_UNLIKELY(printRes >= PATH_BUF_LEN)
                throw WorkerException("Verification object path too long for static buffer. "
                    "Buffer size: " + std::to_string(PATH_BUF_LEN) + "; "
                    "workerRank: " + std::to_string(workerRank) + "; "
                    "dirIndex: " + std::to_string(dirIndex) + "; "
                    "fileIndex: " + std::to_string(fileIndex) );

            if(objectPrefixRand)
                objectPrefix = getRandObjectPrefix(
                    workerRank, dirIndex, fileIndex, s3Args.getS3ObjectPrefix() );

            std::string currentObjectPath = objectPrefix + currentPath.data();

            expectedObjs.insert(currentObjectPath);
        }

        // receive listing of current dir
        do
        {
            worker.checkInterruptionRequest();

            std::chrono::steady_clock::time_point ioStartT = std::chrono::steady_clock::now();

            S3::ListObjectsV2Request request;
            request.SetBucket(bucketVec[bucketIndex] );
            request.SetPrefix(currentListPrefix);
            request.SetMaxKeys( (numObjectsLeft > 1000) ? 1000 : numObjectsLeft); // can't be >1000

            if(!nextContinuationToken.empty() )
                request.SetContinuationToken(nextContinuationToken);

            OPLOG_PRE_OP("S3ListObjectsV2", bucketVec[bucketIndex] + "/" + currentListPrefix, 0,
                request.GetMaxKeys() );

            S3::ListObjectsV2Outcome outcome = s3Client->ListObjectsV2(request);

            OPLOG_POST_OP("S3ListObjectsV2", bucketVec[bucketIndex] + "/" + currentListPrefix, 0,
                outcome.GetResult().GetKeyCount(), !outcome.IsSuccess() );

            IF_UNLIKELY(!outcome.IsSuccess() )
            {
                auto s3Error = outcome.GetError();

                throw WorkerException(std::string("Object listing v2 failed. ") +
                    "Endpoint: " + s3EndpointStr + "; "
                    "Bucket: " + bucketVec[bucketIndex] + "; "
                    "Prefix: " + objectPrefix + "; "
                    "ContinuationToken: " + nextContinuationToken + "; "
                    "NumObjectsLeft: " + std::to_string(numObjectsLeft) + "; "
                    "Exception: " + s3Error.GetExceptionName() + "; " +
                    "Message: " + s3Error.GetMessage() + "; " +
                    "HTTP Error Code: " + std::to_string( (int)s3Error.GetResponseCode() ) + " (" +
                        TranslatorTk::httpErrorCodeToHumanStr( (int)s3Error.GetResponseCode() ) +
                        "); " +
                    "Request ID: " + s3Error.GetRequestId() );
            }

            // calc entry operations latency
            std::chrono::steady_clock::time_point ioEndT = std::chrono::steady_clock::now();
            std::chrono::microseconds ioElapsedMicroSec =
                std::chrono::duration_cast<std::chrono::microseconds>
                (ioEndT - ioStartT);

            worker.entriesLatHisto.addLatency(ioElapsedMicroSec.count() );

            unsigned keyCount = outcome.GetResult().GetKeyCount();

            worker.atomicLiveOps.numEntriesDone += keyCount;
            numObjectsLeft -= keyCount;

            nextContinuationToken = outcome.GetResult().GetNextContinuationToken();
            isTruncated = outcome.GetResult().GetIsTruncated();

            // build list of received keys. (std::list to preserve order; must be alphabetic)
            IF_UNLIKELY(doListObjVerify)
                for(const S3::Object& obj : outcome.GetResult().GetContents() )
                    receivedObjs.push_back(obj.GetKey() );

        } while(isTruncated && numObjectsLeft); // end of while numObjectsLeft in dir loop

        IF_UNLIKELY(doListObjVerify)
            verifyListing(expectedObjs, receivedObjs, bucketVec[bucketIndex],
                currentListPrefix);

    } // end of dirs for-loop

#endif // S3_SUPPORT
}

/**
 * List objects and multi-delete them in given buckets with user-defined limit for number of
 * entries.
 *
 * @throw WorkerException on error.
 */
void S3Mode::listAndMultiDeleteObjects()
{
#ifndef S3_SUPPORT
    throw WorkerException(std::string(__func__) + " called, but this was built without S3 support");
#else

    const StringVec& bucketVec = progArgs->getBenchPaths();
    const size_t numBuckets = bucketVec.size();
    const size_t numDataSetThreads = progArgs->getNumDataSetThreads();
    const uint64_t numObjectsPerRequest = s3Args.getS3MultiDelObjNum();
    std::string objectPrefix = s3Args.getS3ObjectPrefix();
    const bool ignoreDelErrors = progArgs->getIgnoreDelErrors();

    worker.workerGotPhaseWork = false; // not all workers might get work

    for(unsigned bucketIndex = workerRank;
        bucketIndex < numBuckets;
        bucketIndex += numDataSetThreads)
    {
        std::string nextContinuationToken;
        bool isTruncated; // true if S3 server reports more objects left to retrieve

        worker.workerGotPhaseWork = true;

        do
        {
            worker.checkInterruptionRequest();

            std::chrono::steady_clock::time_point ioStartT = std::chrono::steady_clock::now();

            // receive a batch of object names through listing...

            S3::ListObjectsV2Request listRequest;
            listRequest.SetBucket(bucketVec[bucketIndex] );
            listRequest.SetPrefix(objectPrefix);
            listRequest.SetMaxKeys(numObjectsPerRequest);

            if(!nextContinuationToken.empty() )
                listRequest.SetContinuationToken(nextContinuationToken);

            OPLOG_PRE_OP("S3ListObjectsV2", bucketVec[bucketIndex] + "/" + objectPrefix, 0,
                numObjectsPerRequest);

            S3::ListObjectsV2Outcome listOutcome = s3Client->ListObjectsV2(listRequest);

            OPLOG_POST_OP("S3ListObjectsV2", bucketVec[bucketIndex] + "/" + objectPrefix, 0,
                listOutcome.GetResult().GetKeyCount(), !listOutcome.IsSuccess() );

            IF_UNLIKELY(!listOutcome.IsSuccess() )
            {
                auto s3Error = listOutcome.GetError();

                throw WorkerException(std::string("Object listing v2 failed. ") +
                    "Endpoint: " + s3EndpointStr + "; "
                    "Bucket: " + bucketVec[bucketIndex] + "; "
                    "Prefix: " + objectPrefix + "; "
                    "ContinuationToken: " + nextContinuationToken + "; "
                    "NumObjectsPerRequest: " + std::to_string(numObjectsPerRequest) + "; "
                    "Exception: " + s3Error.GetExceptionName() + "; " +
                    "Message: " + s3Error.GetMessage() + "; " +
                    "HTTP Error Code: " + std::to_string( (int)s3Error.GetResponseCode() ) + " (" +
                        TranslatorTk::httpErrorCodeToHumanStr( (int)s3Error.GetResponseCode() ) +
                        "); " +
                    "Request ID: " + s3Error.GetRequestId() );
            }

            // check if we have anything to delete in this round
            if(!listOutcome.GetResult().GetKeyCount() )
                break;

            // send multi-delete request for received batch of objects...

            S3::Delete deleteObjectList;

            for(const S3::Object& obj : listOutcome.GetResult().GetContents() )
                deleteObjectList.AddObjects(
                    S3::ObjectIdentifier().WithKey(obj.GetKey() ) );

            S3::DeleteObjectsRequest delRequest;
            delRequest.SetBucket(bucketVec[bucketIndex] );
            delRequest.SetDelete(deleteObjectList);

            OPLOG_PRE_OP("S3DeleteObjects", bucketVec[bucketIndex] + "/" + objectPrefix, 0,
                numObjectsPerRequest);

            S3::DeleteObjectsOutcome delOutcome = s3Client->DeleteObjects(delRequest);

            OPLOG_POST_OP("S3DeleteObjects", bucketVec[bucketIndex] + "/" + objectPrefix, 0,
                delOutcome.GetResult().GetDeleted().size(), !delOutcome.IsSuccess() );

            IF_UNLIKELY(!delOutcome.IsSuccess() &&
                (!ignoreDelErrors ||
                    (delOutcome.GetError().GetResponseCode() ==
                        Aws::Http::HttpResponseCode::NOT_FOUND) ) )
            {
                auto s3Error = delOutcome.GetError();

                throw WorkerException(std::string("DeleteObjects failed. ") +
                    "Endpoint: " + s3EndpointStr + "; "
                    "Bucket: " + bucketVec[bucketIndex] + "; "
                    "NumObjectsPerRequest: " + std::to_string(numObjectsPerRequest) + "; "
                    "Exception: " + s3Error.GetExceptionName() + "; " +
                    "Message: " + s3Error.GetMessage() + "; " +
                    "HTTP Error Code: " + std::to_string( (int)s3Error.GetResponseCode() ) + " (" +
                        TranslatorTk::httpErrorCodeToHumanStr( (int)s3Error.GetResponseCode() ) +
                        "); " +
                    "Request ID: " + s3Error.GetRequestId() );
            }

            // calc entry operations latency
            std::chrono::steady_clock::time_point ioEndT = std::chrono::steady_clock::now();
            std::chrono::microseconds ioElapsedMicroSec =
                std::chrono::duration_cast<std::chrono::microseconds>
                (ioEndT - ioStartT);

            worker.entriesLatHisto.addLatency(ioElapsedMicroSec.count() );

            unsigned keyCount = delOutcome.GetResult().GetDeleted().size();

            worker.atomicLiveOps.numEntriesDone += keyCount;

            nextContinuationToken = listOutcome.GetResult().GetNextContinuationToken();
            isTruncated = listOutcome.GetResult().GetIsTruncated();

        } while(isTruncated); // end of while numObjectsLeft loop
    }

#endif // S3_SUPPORT
}

/**
 * In S3 mode, decide if we do fallback to reverse upload. This would be the case if this is a write
 * phase and user selected random offsets.
 *
 * @return true if we do fallback to reverse sequential, false otherwise.
 */
bool S3Mode::getDoReverseSeqFallback()
{
    if(progArgs->getUseRandomOffsets() &&
        (progArgs->getBenchMode() == BenchMode_S3) &&
        (worker.benchPhase == BenchPhase_CREATEFILES) )
        return true;

    return false;
}


#ifdef S3_SUPPORT

/**
 * This is for s3 mode and only valid for reads. Randomly selects the next object and does one
 * random offset read within each object. Number of ops is defined by ProgArgs::randomAmount.
 *
 * This inits all of the used random generators (for offset, dir index, file index) internally.
 *
 * @throw WorkerException on error.
 */
void S3Mode::iterateObjectsRand()
{
    const bool haveSubdirs = (progArgs->getNumDirs() > 0);
    const size_t numDirs = haveSubdirs ? progArgs->getNumDirs() : 1; // set 1 to run dir loop once
    const size_t numFiles = progArgs->getNumFiles();
    const uint64_t fileSize = progArgs->getFileSize();
    const size_t blockSize = progArgs->getBlockSize();
    const StringVec& bucketVec = progArgs->getBenchPaths();
    std::array<char, PATH_BUF_LEN> currentPath;
    const size_t workerDirRank = progArgs->getDoDirSharing() ? 0 : workerRank; /* for dir sharing,
        all workers use the dirs of worker rank 0 */
    std::string objectPrefix = s3Args.getS3ObjectPrefix();
    const bool objectPrefixRand = s3Args.getUseS3ObjectPrefixRand();
    const BlockSizeMix& blockSizeMix = progArgs->getBlockSizeMix();
    const BlockSizeMix indexStepMix = BlockSizeMix::parse("1"); // step of 1 for index selection

    // init random generators for dir & file index selection

    std::unique_ptr<RandAlgoInterface> randIndexAlgo =
        RandAlgoSelectorTk::stringToAlgo(progArgs->getRandOffsetAlgo().empty() ?
            RANDALGO_BALANCED_SEQUENTIAL_STR : progArgs->getRandOffsetAlgo() );

    OffsetGenRandom randDirIndexGen(~(uint64_t)0, *randIndexAlgo, numDirs, 0, indexStepMix);
    OffsetGenRandom randFileIndexGen(~(uint64_t)0, *randIndexAlgo, numFiles, 0, indexStepMix);

    // init random offset gen for one read per file

    const uint64_t randomAmount = progArgs->getRandomAmount() / progArgs->getNumDataSetThreads();
    if(!progArgs->getUseRandomUnaligned() ) // random aligned
    {
        worker.rwOffsetGen = std::make_unique<OffsetGenRandomAligned>(blockSize,
            *worker.randOffsetAlgo,
            fileSize, 0, blockSizeMix);
    }
    else // random unaligned
        worker.rwOffsetGen = std::make_unique<OffsetGenRandom>(blockSize, *worker.randOffsetAlgo,
            fileSize, 0, blockSizeMix);

    // randomly select objects and do one random offset read from each

    uint64_t numBytesDone = 0;
    uint64_t interruptCheckBytes = blockSize * INTERRUPTION_CHECK_INTERVAL;

    while(numBytesDone < randomAmount)
    {
        // occasional interruption check
        IF_UNLIKELY( (numBytesDone % interruptCheckBytes) == 0)
            worker.checkInterruptionRequest();

        const size_t dirIndex = randDirIndexGen.getNextOffset();
        const size_t fileIndex = randFileIndexGen.getNextOffset();
        const uint64_t currentBlockSize = worker.rwOffsetGen->getNextBlockSizeToSubmit();

        // generate current dir path
        int printRes;

        if(haveSubdirs)
            printRes = snprintf(currentPath.data(), PATH_BUF_LEN, "r%zu/d%zu/r%zu-f%zu",
                workerDirRank, dirIndex, workerRank, fileIndex);
        else
            printRes = snprintf(currentPath.data(), PATH_BUF_LEN, "r%zu-f%zu",
                workerRank, fileIndex);

        IF_UNLIKELY(printRes >= PATH_BUF_LEN)
            throw WorkerException("object path too long for static buffer. "
                "Buffer size: " + std::to_string(PATH_BUF_LEN) + "; "
                "workerRank: " + std::to_string(workerRank) + "; "
                "dirIndex: " + std::to_string(dirIndex) + "; "
                "fileIndex: " + std::to_string(fileIndex) );

        if(objectPrefixRand)
            objectPrefix = getRandObjectPrefix(
                workerRank, dirIndex, fileIndex, s3Args.getS3ObjectPrefix() );

        const unsigned bucketIndex = (workerRank + dirIndex) % bucketVec.size();
        std::string currentObjectPath = objectPrefix + currentPath.data();

        downloadObject(bucketVec[bucketIndex], currentObjectPath, false);

        worker.rwOffsetGen->reset(); // reset for next rand read op
        numBytesDone += currentBlockSize;
    }

}

/**
 * Add server-side encryption to s3 request.
 */
template <typename REQUESTTYPE>
void S3Mode::addServerSideEncryption(REQUESTTYPE& request)
{
    if(useS3SSE)
        request.WithServerSideEncryption(S3::ServerSideEncryption::AES256);
    else
    if (!s3SSECKey.empty())
    {
        request.WithSSECustomerAlgorithm("AES256")
            .WithSSECustomerKey(s3SSECKey)
            .WithSSECustomerKeyMD5(s3SSECKeyMD5);
    }
    else
    if(!s3SSEKMSKey.empty())
    {
        request.WithServerSideEncryption(S3::ServerSideEncryption::aws_kms)
            .WithSSEKMSKeyId(s3SSEKMSKey);
    }
}

/**
 * Add checksum algorithm to given request if an algorithm has been explicitly selected.
 */
template <typename REQUESTTYPE>
void S3Mode::addChecksumAlgorithm(REQUESTTYPE& request)
{
    IF_UNLIKELY(s3ChecksumAlgorithm != S3ChecksumAlgorithm::NOT_SET)
        request.SetChecksumAlgorithm(s3ChecksumAlgorithm);
}

/**
 * Create given S3 bucket.
 *
 * @throw WorkerException on error.
 */
void S3Mode::createBucket(std::string bucketName)
{
    OPLOG_PRE_OP("S3CreateBucket", bucketName, 0, 0);

    S3::CreateBucketRequest createRequest;
    createRequest.SetBucket(bucketName);

    // Check if multi-credentials are being used and set ACL to public-read-write
    if(!s3Args.getS3CredentialsFile().empty() || !s3Args.getS3CredentialsList().empty())
    {
        createRequest.SetACL(S3::BucketCannedACL::public_read_write);
        LOGGER(Log_DEBUG,
            "Setting bucket ACL to public-read-write for multi-credentials job" << std::endl);
    }

    const auto createOutcome = s3Client->CreateBucket(createRequest);

    OPLOG_POST_OP("S3CreateBucket", bucketName, 0, 0, !createOutcome.IsSuccess());

    if (!createOutcome.IsSuccess())
    {
        auto s3Error = createOutcome.GetError();

        // bucket already existing is not an error
        if (s3Error.GetErrorType() != S3Errors::BUCKET_ALREADY_OWNED_BY_YOU &&
            s3Error.GetErrorType() != S3Errors::BUCKET_ALREADY_EXISTS)
        {
            throwOnError(createOutcome, "Bucket creation failed.", bucketName);
        }
    }
}

/**
 * Request attributes of given S3 bucket.
 *
 * @throw WorkerException on error.
 */
void S3Mode::headBucket(std::string bucketName)
{
    OPLOG_PRE_OP("HeadBucket", bucketName, 0, 0);

    const auto outcome = s3Client->HeadBucket(S3::HeadBucketRequest().WithBucket(bucketName));

    OPLOG_POST_OP("HeadBucket", bucketName, 0, 0, !outcome.IsSuccess());

    throwOnError(outcome, "Head bucket request failed.", bucketName);
}

/**
 * Create tags for given S3 bucket.
 *
 * @throw WorkerException on error.
 */
void S3Mode::createBucketTagging(const std::string& bucketName)
{
    const auto tagging = S3::Tagging().WithTagSet(
        {
            S3::Tag().WithKey(TAG_KEY_MEDIUM_NAME)
                     .WithValue(StringTk::generateRandomS3TagValue(bucketName,
                         TAG_VALUE_MEDIUM_LEN)),
            S3::Tag().WithKey(TAG_KEY_LONG_NAME)
                     .WithValue(StringTk::generateRandomS3TagValue(bucketName, TAG_VALUE_LONG_LEN)),
        }
    );

    S3::PutBucketTaggingRequest taggingRequest;
    taggingRequest.WithBucket(bucketName).WithTagging(tagging);

    addChecksumAlgorithm(taggingRequest);

    OPLOG_PRE_OP("PutBucketTagging", bucketName, 0, 0);

    const auto taggingOutcome = s3Client->PutBucketTagging(taggingRequest);

    OPLOG_POST_OP("PutBucketTagging", bucketName, 0, 0, !taggingOutcome.IsSuccess());

    throwOnError(taggingOutcome, "Put bucket tagging failed.", bucketName);
}

/**
 * Get tags for given S3 bucket.
 *
 * @throw WorkerException on error.
 */
void S3Mode::getBucketTagging(const std::string& bucketName)
{
    OPLOG_PRE_OP("GetBucketTagging", bucketName, 0, 0);

    const auto outcome = s3Client->GetBucketTagging(
        S3::GetBucketTaggingRequest().WithBucket(bucketName) );

    OPLOG_POST_OP("GetBucketTagging", bucketName, 0, 0, !outcome.IsSuccess());

    throwOnError(outcome, "Get bucket tagging failed.", bucketName);

    if (!s3Args.getDoS3BucketTaggingVerify())
        return;

    for (const auto& tag : outcome.GetResult().GetTagSet())
    {
        if (!StringTk::verifyRandomS3TagValue(tag.GetValue(), bucketName))
        {
            std::stringstream errStr;
            errStr << "Bucket tag value is corrupted (invalid checksum). "
                   << "Bucket: " << bucketName << "; "
                   << "Tag: " << tag.GetKey() << "=" << tag.GetValue() << std::endl;
            throw WorkerException(errStr.str());
        }
    }
}

/**
 * Delete bucket tags for given S3 bucket.
 *
 * @throw WorkerException on error.
 */
void S3Mode::deleteBucketTagging(const std::string& bucketName)
{
    OPLOG_PRE_OP("DeleteBucketTagging", bucketName, 0, 0);

    const auto outcome = s3Client->DeleteBucketTagging(
        S3::DeleteBucketTaggingRequest().WithBucket(bucketName)
    );

    OPLOG_POST_OP("DeleteBucketTagging", bucketName, 0, 0, !outcome.IsSuccess());

    throwOnError(outcome, "Delete bucket tagging failed.", bucketName);
}

/**
 * Delete given S3 bucket.
 *
 * @throw WorkerException on error.
 */
void S3Mode::deleteBucket(const std::string& bucketName)
{
    const bool ignoreDelErrors = progArgs->getIgnoreDelErrors();

    S3::DeleteBucketRequest request;
    request.SetBucket(bucketName);

    OPLOG_PRE_OP("S3DeleteBucket", bucketName, 0, 0);

    S3::DeleteBucketOutcome deleteOutcome = s3Client->DeleteBucket(request);

    OPLOG_POST_OP("S3DeleteBucket", bucketName, 0, 0, !deleteOutcome.IsSuccess() );

    if(!deleteOutcome.IsSuccess() )
    {
        auto s3Error = deleteOutcome.GetError();

        if( (s3Error.GetErrorType() != S3Errors::NO_SUCH_BUCKET) || !ignoreDelErrors)
            throwOnError(deleteOutcome, "Bucket deletion failed.", bucketName);
    }
}

/**
 * Put ACL of given S3 object.
 *
 * @throw WorkerException on error.
 */
void S3Mode::putBucketAcl(std::string bucketName)
{
    S3::PutBucketAclRequest request;
    request.WithBucket(bucketName);

    S3AclTk::applyS3PutAclRequestGrants<S3::BucketCannedACL>(progArgs, request);

    OPLOG_PRE_OP("S3PutBucketAcl", bucketName, 0, 0);

    S3::PutBucketAclOutcome outcome = s3Client->PutBucketAcl(request);

    OPLOG_POST_OP("S3PutBucketAcl", bucketName, 0, 0, !outcome.IsSuccess() );

    throwOnError(outcome, "Putting bucket ACL failed.", bucketName);
}

/**
 * Get ACL of given S3 object.
 *
 * @throw WorkerException on error.
 */
void S3Mode::getBucketAcl(std::string bucketName)
{
    bool doS3AclVerify = s3Args.getDoS3AclVerify();

    S3::GetBucketAclRequest request;
    request.WithBucket(bucketName);

    OPLOG_PRE_OP("S3GetBucketAcl", bucketName, 0, 0);

    S3::GetBucketAclOutcome outcome = s3Client->GetBucketAcl(request);

    OPLOG_POST_OP("S3GetBucketAcl", bucketName, 0, 0, !outcome.IsSuccess() );

    throwOnError(outcome, "Getting bucket ACL failed.", bucketName);

    IF_UNLIKELY(doS3AclVerify)
    {
        // check canned ACL as special grantee...

        S3::BucketCannedACL cannedAcl = S3::BucketCannedACLMapper::GetBucketCannedACLForName(
            s3Args.getS3AclGrantee() );

        /* note: checking for ::NOT_SET alone here is not enough, because GetObjectCannedACLForName()
            can return other values if granteeStr doesn't match another enum value. */
        switch( (S3::ObjectCannedACL)cannedAcl)
        {
            case S3::ObjectCannedACL::private_:
            case S3::ObjectCannedACL::public_read:
            case S3::ObjectCannedACL::public_read_write:
            case S3::ObjectCannedACL::authenticated_read:
            case S3::ObjectCannedACL::aws_exec_read:
            case S3::ObjectCannedACL::bucket_owner_read:
            case S3::ObjectCannedACL::bucket_owner_full_control:
            { // found canned ACL as special grantee
                throw WorkerException("Verification of canned ACLs is not supported.");
            }

            case S3::ObjectCannedACL::NOT_SET:
            default:
            { // normal grantee, not a canned ACL
                break;
            }
        }

        // check list of grants...

        std::vector<S3::Grant> verifyGrants;
        S3AclTk::getS3ObjectAclGrants(progArgs, verifyGrants);

        const std::vector<S3::Grant>& outcomeGrants = outcome.GetResult().GetGrants();

        // iterate over all grants that need to be verified
        for(S3::Grant& verifyGrant : verifyGrants)
        {
            bool grantFound = false;

            // iterate over all outcome grants to see if any grant matches current verifyGrant
            for(const S3::Grant& outcomeGrant : outcomeGrants)
            {
                if( (outcomeGrant.GetGrantee().GetID() ==
                        verifyGrant.GetGrantee().GetID() ) ||
                    (outcomeGrant.GetGrantee().GetEmailAddress() ==
                        verifyGrant.GetGrantee().GetEmailAddress() ) ||
                    (outcomeGrant.GetGrantee().GetURI() ==
                        verifyGrant.GetGrantee().GetURI() ) )
                { // grantee matches => check if permission also matches
                    if(outcomeGrant.GetPermission() == verifyGrant.GetPermission() )
                    { // permission matches
                        grantFound = true;
                        break;
                    }
                }
            }

            if(!grantFound)
                throw WorkerException(std::string("S3 ACL verification failed. ") +
                    "Endpoint: " + s3EndpointStr + "; "
                    "Bucket: " + bucketName + "; "
                    "Grantee ID: " + verifyGrant.GetGrantee().GetID() + "; "
                    "Grantee Email: " + verifyGrant.GetGrantee().GetEmailAddress() + "; "
                    "Grantee URI: " + verifyGrant.GetGrantee().GetURI() + "; "
                    "Permission: " + S3AclTk::s3AclPermissionToStr(
                        verifyGrant.GetPermission() ) );
        }
    } // end of verifcation
}

void S3Mode::getBucketVersioning(const std::string& bucketName)
{
    OPLOG_PRE_OP("GetBucketVersioning", bucketName, 0, 0);

    const auto bucketVersioningOutcome = s3Client->GetBucketVersioning(
            S3::GetBucketVersioningRequest().WithBucket(bucketName)
    );

    OPLOG_POST_OP("GetBucketVersioning", bucketName, 0, 0, !bucketVersioningOutcome.IsSuccess());

    throwOnError(bucketVersioningOutcome, "Get bucket versioning failed.", bucketName);

    const auto bucketVersioningStatus = bucketVersioningOutcome.GetResult().GetStatus();

    if (!s3Args.getDoS3BucketVersioningVerify())
        return;

    const auto statusNameMapper =
        S3::BucketVersioningStatusMapper::GetNameForBucketVersioningStatus;
    const auto expectedStatus = S3::BucketVersioningStatus::Suspended;

    IF_UNLIKELY(bucketVersioningStatus != expectedStatus)
    {
        std::stringstream errStr;
        errStr << "Bucket versioning is set to '" << statusNameMapper(bucketVersioningStatus)
               << "' but the expected value was '" << statusNameMapper(expectedStatus) << "'." << std::endl
               << "Bucket: " << bucketName << ';';
        throw WorkerException(errStr.str());
    }
}

void S3Mode::putBucketVersioning(const std::string& bucketName, bool enable)
{
    S3::VersioningConfiguration versioningConfiguration;

    versioningConfiguration.SetStatus(enable ? S3::BucketVersioningStatus::Enabled
                                             : S3::BucketVersioningStatus::Suspended);

    OPLOG_PRE_OP("PutBucketVersioning", bucketName, 0, 0);

    const auto putBucketVersioningOutcome = s3Client->PutBucketVersioning(
            S3::PutBucketVersioningRequest()
                    .WithBucket(bucketName)
                    .WithVersioningConfiguration(versioningConfiguration)
    );

    OPLOG_POST_OP("PutBucketVersioning", bucketName, 0, 0, !putBucketVersioningOutcome.IsSuccess());

    throwOnError(putBucketVersioningOutcome, "Put bucket versioning failed.", bucketName);
}

/**
 * Singlepart upload of an S3 object to an existing bucket. This assumes that progArgs fileSize
 * is not larger than blockSize. Or in other words: This can only upload objects consisting of a
 * single block.
 *
 * @throw WorkerException on error.
 */
void S3Mode::uploadObjectSinglePart(std::string bucketName, std::string objectName)
{
    const uint64_t currentOffset = worker.rwOffsetGen->getNextOffset();
    const size_t blockSize = worker.rwOffsetGen->getNextBlockSizeToSubmit();
    const bool doS3AclPutInline = s3Args.getDoS3AclPutInline();
    const bool ignoreS3Errors = s3Args.getIgnoreS3Errors();

    char* ioBuf = blockSize ? worker.ioBufVec[0] : NULL;
    char* gpuIOBuf = blockSize ? worker.gpuIOBufVec[0] : NULL;

    if(blockSize)
        worker.rwRateLimiter(blockSize);

    std::chrono::steady_clock::time_point ioStartT = std::chrono::steady_clock::now();

    if(blockSize)
    {
        worker.preWriteBlockModifier(ioBuf, gpuIOBuf, blockSize, currentOffset);
        worker.preWriteCudaMemcpy(ioBuf, gpuIOBuf, blockSize);
    }

    S3Transport::PutObjectRequest request;
    request.WithBucket(bucketName)
        .WithKey(objectName)
        .WithContentLength(blockSize);

    if(doS3AclPutInline)
        S3AclTk::applyS3PutObjectAclGrants(progArgs, request);

    addServerSideEncryption(request);

    IF_UNLIKELY(s3ChecksumAlgorithm != S3ChecksumAlgorithm::NOT_SET)
        S3Tk::addUploadPartRequestChecksum(request, NULL,
            s3ChecksumAlgorithm, (unsigned char*) ioBuf, blockSize);

    #if !defined(S3_AWSCRT) || AWS_SDK_AT_LEAST(1, 11, 708)
        request.SetContinueRequestHandler( [&](const Aws::Http::HttpRequest* request)
            { return !worker.isInterruptionRequested.load(); } );
    #endif // !S3_AWSCRT or AWS SDK >= 1.11.708

    OPLOG_PRE_OP("S3PutObject", bucketName + "/" + objectName, currentOffset, blockSize);

    S3Transport::PutObjectOutcome outcome = transport.putObject(*s3Client, request, ioBuf,
        gpuIOBuf, blockSize, worker.atomicLiveOps);

    OPLOG_POST_OP("S3PutObject", bucketName + "/" + objectName, currentOffset, blockSize,
        !outcome.IsSuccess() );

    worker.checkInterruptionRequest(); // (placed here to avoid outcome check on interruption)

    IF_UNLIKELY(!outcome.IsSuccess() && !ignoreS3Errors)
        throwOnError(outcome, "Object upload failed.", bucketName, objectName);

    if(blockSize)
    {
        worker.postReadCudaMemcpy(ioBuf, gpuIOBuf, blockSize);
        worker.postReadBlockChecker(ioBuf, gpuIOBuf, blockSize, currentOffset);
    }

    // calc io operation latency
    std::chrono::steady_clock::time_point ioEndT = std::chrono::steady_clock::now();
    std::chrono::microseconds ioElapsedMicroSec =
        std::chrono::duration_cast<std::chrono::microseconds>
        (ioEndT - ioStartT);

    worker.iopsLatHisto.addLatency(ioElapsedMicroSec.count() );

    worker.numIOPSSubmitted++;
    worker.rwOffsetGen->addBytesSubmitted(blockSize);
    worker.atomicLiveOps.numIOPSDone++;
}

/**
 * Block-sized multipart upload of an S3 object to an existing bucket.
 *
 * This will delegate to uploadObjectMultiPartAsync() if iodepth > 1.
 *
 * @throw WorkerException on error.
 */
void S3Mode::uploadObjectMultiPart(std::string bucketName, std::string objectName)
{
    const bool doS3AclPutInline = s3Args.getDoS3AclPutInline();
    const bool ignoreS3Errors = s3Args.getIgnoreS3Errors();
    const bool s3NoMpuCompletion = s3Args.getS3NoMpuCompletion();
    const size_t s3MpuSizeVariance = s3Args.getS3MpuSizeVariance();

    // S T E P 0: hand over to async function if iodepth is given

    if(progArgs->getIODepth() > 1)
    {
        uploadObjectMultiPartAsync(bucketName, objectName);
        return;
    }

    // S T E P 1: retrieve multipart upload ID from server

    S3::CreateMultipartUploadRequest createMultipartUploadRequest;
    createMultipartUploadRequest.SetBucket(bucketName);
    createMultipartUploadRequest.SetKey(objectName);

    addServerSideEncryption(createMultipartUploadRequest);
    addChecksumAlgorithm(createMultipartUploadRequest);

    if(doS3AclPutInline)
        S3AclTk::applyS3PutObjectAclGrants(progArgs, createMultipartUploadRequest);

    LOGGER_DEBUG_BUILD(__func__ << ":" << __LINE__ << ": "
        "Worker: " << workerRank << "; " <<
        "Obj: " << bucketName + "/" + objectName << std::endl);

    OPLOG_PRE_OP("S3CreateMultipartUpload", bucketName + "/" + objectName, 0, 0);

    auto createMultipartUploadOutcome = s3Client->CreateMultipartUpload(
        createMultipartUploadRequest);

    OPLOG_POST_OP("S3CreateMultipartUpload", bucketName + "/" + objectName, 0, 0,
        !createMultipartUploadOutcome.IsSuccess() );

    IF_UNLIKELY(!createMultipartUploadOutcome.IsSuccess() && !ignoreS3Errors)
        throwOnError(createMultipartUploadOutcome, "Multipart upload creation failed.",
            bucketName, objectName);

    Aws::String uploadID = createMultipartUploadOutcome.GetResult().GetUploadId();

    S3::CompletedMultipartUpload completedMultipartUpload;

    // S T E P 2: upload one block-sized part in each loop pass

    uint64_t currentPartNum = 0; // valid range is 1..10K

    while(worker.rwOffsetGen->getNumBytesLeftToSubmit() )
    {
        const uint64_t currentOffset = worker.rwOffsetGen->getNextOffset();
        size_t blockSize = worker.rwOffsetGen->getNextBlockSizeToSubmit();

        /* note: in normal forward mode, blockSize is variable because of s3MpuSizeVariance, so we
            can't use the currentPartNum formula with fixed blockSize from reverse mode. */
        currentPartNum = (progArgs->getDoReverseSeqOffsets() || getDoReverseSeqFallback() ) ?
            1 + (currentOffset / worker.rwOffsetGen->getBlockSize() ) : (currentPartNum+1);

        /* note: rwOffsetGen->getBlockSize() comparison is to prevent random subtract from last part
            (to avoid risk of two trailing parts of less than usual 5MiB min allowed part size). */
        if(s3MpuSizeVariance && (blockSize == worker.rwOffsetGen->getBlockSize() ) )
        {
            const size_t randBlockSizeVar = worker.randBlockVarReseed->next() % s3MpuSizeVariance;

            IF_LIKELY(randBlockSizeVar < blockSize)
                blockSize = blockSize - randBlockSizeVar;
        }

        worker.rwRateLimiter(blockSize);

        std::chrono::steady_clock::time_point ioStartT = std::chrono::steady_clock::now();

        worker.preWriteBlockModifier(worker.ioBufVec[0], worker.gpuIOBufVec[0], blockSize,
            currentOffset);
        worker.preWriteCudaMemcpy(worker.ioBufVec[0], worker.gpuIOBufVec[0], blockSize);

        // prepare part upload

        S3::CompletedPart completedPart;
        S3Transport::UploadPartRequest uploadPartRequest;
        uploadPartRequest.WithBucket(bucketName)
            .WithKey(objectName)
            .WithUploadId(uploadID)
            .WithPartNumber(currentPartNum)
            .WithContentLength(blockSize);

        // (no addServerSideEncryptionHeaders() because this one is only for SSE-C)
        if(!s3SSECKey.empty() )
            uploadPartRequest.WithSSECustomerAlgorithm("AES256")
                    .WithSSECustomerKey(s3SSECKey)
                    .WithSSECustomerKeyMD5(s3SSECKeyMD5);

        IF_UNLIKELY(s3ChecksumAlgorithm != S3ChecksumAlgorithm::NOT_SET)
            S3Tk::addUploadPartRequestChecksum(uploadPartRequest, &completedPart,
                s3ChecksumAlgorithm, (unsigned char*) worker.ioBufVec[0], blockSize);

        #if !defined(S3_AWSCRT) || AWS_SDK_AT_LEAST(1, 11, 708)
            uploadPartRequest.SetContinueRequestHandler( [&](const Aws::Http::HttpRequest* request)
                { return !worker.isInterruptionRequested.load(); } );
        #endif // !S3_AWSCRT or AWS SDK >= 1.11.708

        OPLOG_PRE_OP("S3UploadPart", bucketName + "/" + objectName, currentOffset, blockSize);

        auto uploadPartOutcome = transport.uploadPart(*s3Client, uploadPartRequest,
            worker.ioBufVec[0], worker.gpuIOBufVec[0], blockSize, worker.atomicLiveOps);

        /* note: there is no way to tell the server about the offset of a part within an object, so
            concurrent part uploads might add overhead on completion for the S3 server to assemble
            the full object in correct parts order. */

        OPLOG_POST_OP("S3UploadPart", bucketName + "/" + objectName, currentOffset, blockSize,
            !uploadPartOutcome.IsSuccess() );

        worker.checkInterruptionRequest( // (placed here to avoid outcome check on interruption)
            [&] { abortMultipartUpload(bucketName, objectName, uploadID); } );

        IF_UNLIKELY(!uploadPartOutcome.IsSuccess() )
        {
            abortMultipartUpload(bucketName, objectName, uploadID);

            if (!ignoreS3Errors)
            {
                auto s3Error = uploadPartOutcome.GetError();

                throw WorkerException(std::string("Multipart part upload failed. ") +
                    "Endpoint: " + s3EndpointStr + "; "
                    "Bucket: " + bucketName + "; "
                    "Object: " + objectName + "; "
                    "Part: " + std::to_string(currentPartNum) + "; "
                    "Exception: " + s3Error.GetExceptionName() + "; " +
                    "Message: " + s3Error.GetMessage() + "; " +
                    "HTTP Error Code: " + std::to_string( (int)s3Error.GetResponseCode() ) + " (" +
                        TranslatorTk::httpErrorCodeToHumanStr( (int)s3Error.GetResponseCode() ) +
                        "); "
                    "Request ID: " + s3Error.GetRequestId() );
            }
        }

        // mark part as completed

        completedPart.SetPartNumber(currentPartNum);
        auto partETag = uploadPartOutcome.GetResult().GetETag();
        completedPart.SetETag(partETag);

        completedMultipartUpload.AddParts(completedPart);

        worker.postReadCudaMemcpy(worker.ioBufVec[0], worker.gpuIOBufVec[0], blockSize);
        worker.postReadBlockChecker(worker.ioBufVec[0], worker.gpuIOBufVec[0], blockSize,
            currentOffset);

        // calc io operation latency
        std::chrono::steady_clock::time_point ioEndT = std::chrono::steady_clock::now();
        std::chrono::microseconds ioElapsedMicroSec =
            std::chrono::duration_cast<std::chrono::microseconds>
            (ioEndT - ioStartT);

        worker.iopsLatHisto.addLatency(ioElapsedMicroSec.count() );

        worker.numIOPSSubmitted++;
        worker.rwOffsetGen->addBytesSubmitted(blockSize);
        worker.atomicLiveOps.numIOPSDone++;
    }

    // S T E P 3: submit upload completion

    IF_UNLIKELY(s3NoMpuCompletion)
        return; // user-defined skip of completion message

    if(progArgs->getDoReverseSeqOffsets() || getDoReverseSeqFallback() )
    { // we need to reverse the parts vector for ascending order
        const Aws::Vector<S3::CompletedPart>& reversePartsVec = completedMultipartUpload.GetParts();
        Aws::Vector<S3::CompletedPart> forwardPartsVec(reversePartsVec.size() );

        std::reverse_copy(std::begin(reversePartsVec), std::end(reversePartsVec),
            std::begin(forwardPartsVec) );

        completedMultipartUpload.SetParts(forwardPartsVec);
    }

    S3::CompleteMultipartUploadRequest completionRequest;
    completionRequest.WithBucket(bucketName)
        .WithKey(objectName)
        .WithUploadId(uploadID)
        .WithMultipartUpload(completedMultipartUpload);

    // (no addServerSideEncryptionHeaders() because this one is only for SSE-C)
    if(!s3SSECKey.empty() )
        completionRequest.WithSSECustomerAlgorithm("AES256")
                .WithSSECustomerKey(s3SSECKey)
                .WithSSECustomerKeyMD5(s3SSECKeyMD5);

    OPLOG_PRE_OP("S3CompleteMultipartUpload", bucketName + "/" + objectName, 0,
        completedMultipartUpload.GetParts().size() );

    auto completionOutcome = s3Client->CompleteMultipartUpload(completionRequest);

    OPLOG_POST_OP("S3CompleteMultipartUpload", bucketName + "/" + objectName, 0,
        completedMultipartUpload.GetParts().size(), !completionOutcome.IsSuccess() );

    IF_UNLIKELY(!completionOutcome.IsSuccess())
    {
        auto s3Error = completionOutcome.GetError();

        if (!(s3Args.getS3IgnoreMultipartUpload404() &&
              s3Error.GetResponseCode() == Aws::Http::HttpResponseCode::NOT_FOUND))
        {
            abortMultipartUpload(bucketName, objectName, uploadID);

            if (!ignoreS3Errors)
            {
                throw WorkerException(std::string("Multipart upload completion failed. ") +
                     "Endpoint: " + s3EndpointStr + "; "
                     "Bucket: " + bucketName + "; "
                     "Object: " + objectName + "; "
                     "NumParts: " + std::to_string(completedMultipartUpload.GetParts().size() ) +
                        "; "
                     "Exception: " + s3Error.GetExceptionName() + "; " +
                     "Message: " + s3Error.GetMessage() + "; " +
                     "HTTP Error Code: " + std::to_string( (int)s3Error.GetResponseCode() ) + " (" +
                         TranslatorTk::httpErrorCodeToHumanStr( (int)s3Error.GetResponseCode() ) +
                         "); "
                     "Request ID: " + s3Error.GetRequestId() );
            }
        }
    }
}

/**
 * Async block-sized multipart upload of an S3 object to an existing bucket.
 *
 * @throw WorkerException on error.
 */
void S3Mode::uploadObjectMultiPartAsync(std::string bucketName, std::string objectName)
{
    const bool doS3AclPutInline = s3Args.getDoS3AclPutInline();
    const bool ignoreS3Errors = s3Args.getIgnoreS3Errors();
    const bool s3NoMpuCompletion = s3Args.getS3NoMpuCompletion();
    const unsigned ioDepth = progArgs->getIODepth();

    // S T E P 1: retrieve multipart upload ID from server

    S3::CreateMultipartUploadRequest createMultipartUploadRequest;
    createMultipartUploadRequest.SetBucket(bucketName);
    createMultipartUploadRequest.SetKey(objectName);

    addServerSideEncryption(createMultipartUploadRequest);
    addChecksumAlgorithm(createMultipartUploadRequest);

    if(doS3AclPutInline)
        S3AclTk::applyS3PutObjectAclGrants(progArgs, createMultipartUploadRequest);

    LOGGER_DEBUG_BUILD(__func__ << ":" << __LINE__ << ": "
        "Worker: " << workerRank << "; " <<
        "Obj: " << bucketName + "/" + objectName << std::endl);

    OPLOG_PRE_OP("S3CreateMultipartUpload", bucketName + "/" + objectName, 0, 0);

    auto createMultipartUploadOutcome = s3Client->CreateMultipartUpload(
        createMultipartUploadRequest);

    OPLOG_POST_OP("S3CreateMultipartUpload", bucketName + "/" + objectName, 0, 0,
        !createMultipartUploadOutcome.IsSuccess() );

    IF_UNLIKELY(!createMultipartUploadOutcome.IsSuccess() && !ignoreS3Errors)
        throwOnError(createMultipartUploadOutcome, "Multipart upload creation failed.",
            bucketName, objectName);

    Aws::String uploadID = createMultipartUploadOutcome.GetResult().GetUploadId();


    // S T E P 2: upload parts

    S3::CompletedMultipartUpload completedMultipartUpload;
    std::vector<S3AsyncUploadPartContext> partCompletionsVec;

    partCompletionsVec.reserve(ioDepth); /* (reserve is important to not invalidate pointers for
        async callbacks on vector grow) */

    uint64_t currentPartNum = 0; // valid range is 1..10K

    /* NOTE on try-catch: before we can exit this function we need to wait for all async tasks to
        complete because they might still be accessing members of partCompletionsVec. */
    try
    {
        while(worker.rwOffsetGen->getNumBytesLeftToSubmit() )
        {
            // S T E P 2.1: submit parts asynchronously up to iodepth

            for(unsigned currentIODepth = 0;
                (currentIODepth < ioDepth) && worker.rwOffsetGen->getNumBytesLeftToSubmit();
                currentIODepth++)
            {
                const size_t blockSize = worker.rwOffsetGen->getNextBlockSizeToSubmit();
                const uint64_t currentOffset = worker.rwOffsetGen->getNextOffset();

                /* note: in normal forward mode, blockSize can be variable because of a block size
                    mix (or s3MpuSizeVariance in the non-async sibling function), so we can't use
                    the currentPartNum formula with fixed blockSize from reverse mode. */
                currentPartNum = (progArgs->getDoReverseSeqOffsets() ||
                    getDoReverseSeqFallback() ) ?
                    1 + (currentOffset / worker.rwOffsetGen->getBlockSize() ) : (currentPartNum+1);

                worker.rwRateLimiter(blockSize);

                /* note: nothing that throws an exception (e.g. funcRWRateLimiter for
                    "--rwmixthrpct") must be between emplace_back and UploadPartAsync()
                    because otherwise the wait loop in catch() will wait infinitely. */
                S3AsyncUploadPartContext& asyncPartContext = partCompletionsVec.emplace_back(
                    currentOffset, blockSize, currentPartNum);

                asyncPartContext.ioStartT = std::chrono::steady_clock::now();

                worker.preWriteBlockModifier(worker.ioBufVec[currentIODepth],
                    worker.gpuIOBufVec[currentIODepth], blockSize, currentOffset);
                worker.preWriteCudaMemcpy(worker.ioBufVec[currentIODepth],
                    worker.gpuIOBufVec[currentIODepth], blockSize);

                // prepare part upload

                S3Transport::UploadPartRequest& uploadPartRequest =
                    asyncPartContext.uploadPartRequest;
                uploadPartRequest.WithBucket(bucketName)
                    .WithKey(objectName)
                    .WithUploadId(uploadID)
                    .WithPartNumber(currentPartNum)
                    .WithContentLength(blockSize);

                // (no addServerSideEncryptionHeaders() because this one is only for SSE-C)
                if(!s3SSECKey.empty() )
                    uploadPartRequest.WithSSECustomerAlgorithm("AES256")
                            .WithSSECustomerKey(s3SSECKey)
                            .WithSSECustomerKeyMD5(s3SSECKeyMD5);

                IF_UNLIKELY(s3ChecksumAlgorithm != S3ChecksumAlgorithm::NOT_SET)
                    S3Tk::addUploadPartRequestChecksum(uploadPartRequest,
                        &asyncPartContext.completedPart, s3ChecksumAlgorithm,
                        (unsigned char*) worker.ioBufVec[currentIODepth], blockSize);

                #if !defined(S3_AWSCRT) || AWS_SDK_AT_LEAST(1, 11, 708)
                    uploadPartRequest.SetContinueRequestHandler(
                        [&isInterruptionRequested = worker.isInterruptionRequested]
                        (const Aws::Http::HttpRequest* request)
                        { return !isInterruptionRequested.load(); } );
                #endif // !S3_AWSCRT or AWS SDK >= 1.11.708

                OPLOG_PRE_OP("S3UploadPartAsync", bucketName + "/" + objectName, currentOffset,
                    blockSize);

                transport.uploadPartAsync(*s3Client, uploadPartRequest,
                    worker.ioBufVec[currentIODepth], worker.gpuIOBufVec[currentIODepth], blockSize,
                    worker.atomicLiveOps, asyncPartContext.partCompletePromise);

                worker.numIOPSSubmitted++;
                worker.rwOffsetGen->addBytesSubmitted(asyncPartContext.blockSize);

            } // enf of step 2.1: async submit for-loop

            // S T E P 2.2: handle completion of submitted parts

            for(unsigned currentIODepth = 0;
                currentIODepth < partCompletionsVec.size();
                currentIODepth++)
            {
                S3AsyncUploadPartContext& asyncPartContext = partCompletionsVec[currentIODepth];

                // wait for part upload to complete ("future.get()" blocks)
                S3Transport::UploadPartOutcome uploadPartOutcome =
                    asyncPartContext.partCompleteFuture.get();

                OPLOG_POST_OP("S3UploadPartAsync", bucketName + "/" + objectName,
                    asyncPartContext.currentOffset, asyncPartContext.blockSize,
                    !uploadPartOutcome.IsSuccess() );

                // (interrupt check placed here to avoid outcome check on interruption)
                worker.checkInterruptionRequest([this, &bucketName, &objectName, &uploadID]
                    { abortMultipartUpload(bucketName, objectName, uploadID); } );

                IF_UNLIKELY(!uploadPartOutcome.IsSuccess() )
                {
                    abortMultipartUpload(bucketName, objectName, uploadID);

                    if (!ignoreS3Errors)
                    {
                        auto s3Error = uploadPartOutcome.GetError();

                        throw WorkerException(std::string("Multipart part upload failed. ") +
                            "Endpoint: " + s3EndpointStr + "; "
                            "Bucket: " + bucketName + "; "
                            "Object: " + objectName + "; "
                            "Part: " + std::to_string(asyncPartContext.partNum) + "; "
                            "Exception: " + s3Error.GetExceptionName() + "; " +
                            "Message: " + s3Error.GetMessage() + "; " +
                            "HTTP Error Code: " + std::to_string( (int)s3Error.GetResponseCode() ) + " (" +
                                TranslatorTk::httpErrorCodeToHumanStr( (int)s3Error.GetResponseCode() ) +
                                "); "
                            "Request ID: " + s3Error.GetRequestId() );
                    }
                }

                // mark part as completed

                auto partETag = uploadPartOutcome.GetResult().GetETag();
                asyncPartContext.completedPart.SetETag(partETag);

                completedMultipartUpload.AddParts(asyncPartContext.completedPart);

                worker.postReadCudaMemcpy(
                    worker.ioBufVec[currentIODepth], worker.gpuIOBufVec[currentIODepth],
                    asyncPartContext.blockSize);
                worker.postReadBlockChecker(
                    worker.ioBufVec[currentIODepth], worker.gpuIOBufVec[currentIODepth],
                    asyncPartContext.blockSize, asyncPartContext.currentOffset);

                // calc io operation latency
                std::chrono::steady_clock::time_point ioEndT = std::chrono::steady_clock::now();
                std::chrono::microseconds ioElapsedMicroSec =
                    std::chrono::duration_cast<std::chrono::microseconds>
                    (ioEndT - asyncPartContext.ioStartT);

                worker.iopsLatHisto.addLatency(ioElapsedMicroSec.count() );

                worker.atomicLiveOps.numIOPSDone++;
            } // end of step 2.2: wait for part batch completion for-loop

            worker.checkInterruptionRequest([this, &bucketName, &objectName, &uploadID]
                { abortMultipartUpload(bucketName, objectName, uploadID); } );

            // reset completions vec for next round
            partCompletionsVec.clear();

        } // end of step 2 loop: submit parts + wait for completions
    }
    catch(...)
    {
        worker.interruptExecution(); // for SetContinueRequestHandler()

        // wait for all parts to complete ("future.get()" blocks)
        for(unsigned i = 0; i < partCompletionsVec.size(); i++)
            SAFE_FUTURE_GET_IGNORE_ERR(partCompletionsVec[i].partCompleteFuture);

        /* ignore errors because we're already handling an error (and ".get()" will also throw
            an exception if value has already been retrieved) */
        try { abortMultipartUpload(bucketName, objectName, uploadID); }
        catch(...) {}

        throw;
    }

    // S T E P 3: submit upload completion

    IF_UNLIKELY(s3NoMpuCompletion)
        return; // user-defined skip of completion message

    if(progArgs->getDoReverseSeqOffsets() || getDoReverseSeqFallback() )
    { // we need to reverse the parts vector for ascending order
        const Aws::Vector<S3::CompletedPart>& reversePartsVec = completedMultipartUpload.GetParts();
        Aws::Vector<S3::CompletedPart> forwardPartsVec(reversePartsVec.size() );

        std::reverse_copy(std::begin(reversePartsVec), std::end(reversePartsVec),
            std::begin(forwardPartsVec) );

        completedMultipartUpload.SetParts(forwardPartsVec);
    }

    S3::CompleteMultipartUploadRequest completionRequest;
    completionRequest.WithBucket(bucketName)
        .WithKey(objectName)
        .WithUploadId(uploadID)
        .WithMultipartUpload(completedMultipartUpload);

    // (no addServerSideEncryptionHeaders() because this one is only for SSE-C)
    if(!s3SSECKey.empty() )
        completionRequest.WithSSECustomerAlgorithm("AES256")
                .WithSSECustomerKey(s3SSECKey)
                .WithSSECustomerKeyMD5(s3SSECKeyMD5);

    OPLOG_PRE_OP("S3CompleteMultipartUpload", bucketName + "/" + objectName, 0,
        completedMultipartUpload.GetParts().size() );

    auto completionOutcome = s3Client->CompleteMultipartUpload(completionRequest);

    OPLOG_POST_OP("S3CompleteMultipartUpload", bucketName + "/" + objectName, 0,
        completedMultipartUpload.GetParts().size(), !completionOutcome.IsSuccess() );

    IF_UNLIKELY(!completionOutcome.IsSuccess())
    {
        auto s3Error = completionOutcome.GetError();

        if (!(s3Args.getS3IgnoreMultipartUpload404() &&
              s3Error.GetResponseCode() == Aws::Http::HttpResponseCode::NOT_FOUND))
        {
            abortMultipartUpload(bucketName, objectName, uploadID);

            if (!ignoreS3Errors)
            {
                throw WorkerException(std::string("Multipart upload completion failed. ") +
                    "Endpoint: " + s3EndpointStr + "; "
                    "Bucket: " + bucketName + "; "
                    "Object: " + objectName + "; "
                    "NumParts: " + std::to_string(
                        completedMultipartUpload.GetParts().size() ) + "; "
                    "Exception: " + s3Error.GetExceptionName() + "; " +
                    "Message: " + s3Error.GetMessage() + "; " +
                    "HTTP Error Code: " + std::to_string( (int)s3Error.GetResponseCode() ) + " (" +
                        TranslatorTk::httpErrorCodeToHumanStr( (int)s3Error.GetResponseCode() ) +
                        "); " +
                    "Request ID: " + s3Error.GetRequestId() );
            }
        }
    }
}

/**
 * Block-sized multipart upload of an S3 object to an existing bucket, shared by multiple workers.
 *
 * This will delegate to uploadObjectMultiPartSharedAsync() if iodepth > 1.
 *
 * @throw WorkerException on error.
 */
void S3Mode::uploadObjectMultiPartShared(std::string bucketName, std::string objectName,
    uint64_t objectTotalSize)
{
    const bool s3NoMpuCompletion = s3Args.getS3NoMpuCompletion();

    // S T E P 0: hand over to async function if iodepth is given

    if(progArgs->getIODepth() > 1)
    {
        uploadObjectMultiPartSharedAsync(bucketName, objectName, objectTotalSize);
        return;
    }

    // S T E P 1: retrieve multipart upload ID from server

    Aws::String uploadID = s3SharedUploadStore.getMultipartUploadID(
        bucketName, objectName, s3Client, opsLog);

    std::unique_ptr<Aws::Vector<S3::CompletedPart>> allCompletedParts; // need sort before send

    // S T E P 2: upload one block-sized part in each loop pass

    while(worker.rwOffsetGen->getNumBytesLeftToSubmit() )
    {
        const size_t blockSize = worker.rwOffsetGen->getNextBlockSizeToSubmit();
        const uint64_t currentOffset = worker.rwOffsetGen->getNextOffset();

        /* note: this object is shared by multiple concurrent worker threads, each covering a
            different (non-zero-based) byte range of the object, so the part number has to be
            derived from the absolute offset. (A local incrementing counter, as used in the
            single-owner upload functions, would not work here: it can't be combined with a
            block size mix, which ProgArgs rejects for shared multipart uploads.) */
        const uint64_t currentPartNum =
            1 + (currentOffset / worker.rwOffsetGen->getBlockSize() ); // +1 for range 1..10K

        worker.rwRateLimiter(blockSize);

        std::chrono::steady_clock::time_point ioStartT = std::chrono::steady_clock::now();

        worker.preWriteBlockModifier(worker.ioBufVec[0], worker.gpuIOBufVec[0], blockSize,
            currentOffset);
        worker.preWriteCudaMemcpy(worker.ioBufVec[0], worker.gpuIOBufVec[0], blockSize);

        // prepare part upload

        S3::CompletedPart completedPart;
        S3Transport::UploadPartRequest uploadPartRequest;
        uploadPartRequest.WithBucket(bucketName)
            .WithKey(objectName)
            .WithUploadId(uploadID)
            .WithPartNumber(currentPartNum)
            .WithContentLength(blockSize);

        IF_UNLIKELY(s3ChecksumAlgorithm != S3ChecksumAlgorithm::NOT_SET)
            S3Tk::addUploadPartRequestChecksum(uploadPartRequest, &completedPart,
                s3ChecksumAlgorithm, (unsigned char*) worker.ioBufVec[0], blockSize);

        #if !defined(S3_AWSCRT) || AWS_SDK_AT_LEAST(1, 11, 708)
            uploadPartRequest.SetContinueRequestHandler( [&](const Aws::Http::HttpRequest* request)
                { return !worker.isInterruptionRequested.load(); } );
        #endif // !S3_AWSCRT or AWS SDK >= 1.11.708

        OPLOG_PRE_OP("S3UploadPart", bucketName + "/" + objectName, currentOffset, blockSize);

        auto uploadPartOutcome = transport.uploadPart(*s3Client, uploadPartRequest,
            worker.ioBufVec[0], worker.gpuIOBufVec[0], blockSize, worker.atomicLiveOps);

        OPLOG_POST_OP("S3UploadPart", bucketName + "/" + objectName, currentOffset, blockSize,
            !uploadPartOutcome.IsSuccess() );

        worker.checkInterruptionRequest(); /* (placed here to avoid outcome check on interruption; abort
            message will be sent during s3SharedUploadStore cleanup) */

        IF_UNLIKELY(!uploadPartOutcome.IsSuccess() )
        {
            // (note: abort message will be sent during s3SharedUploadStore cleanup)

            auto s3Error = uploadPartOutcome.GetError();

            throw WorkerException(std::string("Shared multipart part upload failed. ") +
                "Endpoint: " + s3EndpointStr + "; "
                "Bucket: " + bucketName + "; "
                "Object: " + objectName + "; "
                "Part: " + std::to_string(currentPartNum) + "; "
                "UploadID: " + uploadID + "; "
                "Rank: " + std::to_string(workerRank) + "; "
                "Exception: " + s3Error.GetExceptionName() + "; " +
                "Message: " + s3Error.GetMessage() + "; " +
                "HTTP Error Code: " + std::to_string( (int)s3Error.GetResponseCode() ) + " (" +
                    TranslatorTk::httpErrorCodeToHumanStr( (int)s3Error.GetResponseCode() ) +
                    "); " +
                "Request ID: " + s3Error.GetRequestId() );
        }

        // mark part as completed

        completedPart.SetPartNumber(currentPartNum);
        auto partETag = uploadPartOutcome.GetResult().GetETag();
        completedPart.SetETag(partETag);

        LOGGER_DEBUG_BUILD(__func__ << __LINE__ << ": worker=" << workerRank << "; "
            "blocksize=" << blockSize << "; "
            "totalSize=" << objectTotalSize << "; " << "objectName=" << objectName << std::endl);

        allCompletedParts = s3SharedUploadStore.addCompletedPart(
            bucketName, objectName, blockSize, objectTotalSize, completedPart);

        worker.postReadCudaMemcpy(worker.ioBufVec[0], worker.gpuIOBufVec[0], blockSize);
        worker.postReadBlockChecker(worker.ioBufVec[0], worker.gpuIOBufVec[0], blockSize,
            currentOffset);

        // calc io operation latency
        std::chrono::steady_clock::time_point ioEndT = std::chrono::steady_clock::now();
        std::chrono::microseconds ioElapsedMicroSec =
            std::chrono::duration_cast<std::chrono::microseconds>
            (ioEndT - ioStartT);

        worker.iopsLatHisto.addLatency(ioElapsedMicroSec.count() );

        worker.numIOPSSubmitted++;
        worker.rwOffsetGen->addBytesSubmitted(blockSize);
        worker.atomicLiveOps.numIOPSDone++;

        // sanity check after rwOffsetGen update
        IF_UNLIKELY(allCompletedParts && worker.rwOffsetGen->getNumBytesLeftToSubmit() )
            throw WorkerException(std::string("Shared multipart upload logic error. ") +
                "Completion returned by upload store, but bytes left to submit. "
                "Endpoint: " + s3EndpointStr + "; "
                "Bucket: " + bucketName + "; "
                "Object: " + objectName + "; "
                "Part: " + std::to_string(currentPartNum) + "; "
                "BytesLeft: " + std::to_string(worker.rwOffsetGen->getNumBytesLeftToSubmit() ) + "; " +
                "ObjectSize: " + std::to_string(objectTotalSize) );
    }

    // S T E P 3: submit upload completion

    IF_UNLIKELY(s3NoMpuCompletion)
        return; // user-defined skip of completion message

    if(!allCompletedParts)
        return; // another worker still needs to upload parts, so no completion yet

    // (note: part vec must be in ascending order)
    std::sort(allCompletedParts->begin(), allCompletedParts->end(),
        [](const S3::CompletedPart& a, const S3::CompletedPart& b) -> bool
        { return a.GetPartNumber() < b.GetPartNumber(); } );

    S3::CompletedMultipartUpload completedMultipartUpload;
    completedMultipartUpload.SetParts(*allCompletedParts);

    S3::CompleteMultipartUploadRequest completionRequest;
    completionRequest.WithBucket(bucketName)
        .WithKey(objectName)
        .WithUploadId(uploadID)
        .WithMultipartUpload(completedMultipartUpload);

    OPLOG_PRE_OP("S3CompleteMultipartUpload", bucketName + "/" + objectName, 0, objectTotalSize);

    auto completionOutcome = s3Client->CompleteMultipartUpload(completionRequest);

    OPLOG_POST_OP("S3CompleteMultipartUpload", bucketName + "/" + objectName, 0, objectTotalSize,
        !completionOutcome.IsSuccess() );


    IF_UNLIKELY(!completionOutcome.IsSuccess() )
    {
        auto s3Error = completionOutcome.GetError();

        if (!(s3Args.getS3IgnoreMultipartUpload404() &&
              s3Error.GetResponseCode() == Aws::Http::HttpResponseCode::NOT_FOUND))
        {
            abortMultipartUpload(bucketName, objectName, uploadID);

            throw WorkerException(std::string("Shared multipart upload completion failed. ") +
                "Endpoint: " + s3EndpointStr + "; "
                "Bucket: " + bucketName + "; "
                "Object: " + objectName + "; "
                "NumParts: " + std::to_string(completedMultipartUpload.GetParts().size() ) + "; "
                "Exception: " + s3Error.GetExceptionName() + "; " +
                "Message: " + s3Error.GetMessage() + "; " +
                "HTTP Error Code: " + std::to_string( (int)s3Error.GetResponseCode() ) + " (" +
                    TranslatorTk::httpErrorCodeToHumanStr( (int)s3Error.GetResponseCode() ) +
                    "); " +
                "Request ID: " + s3Error.GetRequestId() );
        }
    }
}

/**
 * Async block-sized multipart upload of an S3 object to an existing bucket, shared by multiple
 * workers.
 *
 * @throw WorkerException on error.
 */
void S3Mode::uploadObjectMultiPartSharedAsync(std::string bucketName,
    std::string objectName, uint64_t objectTotalSize)
{
    const bool s3NoMpuCompletion = s3Args.getS3NoMpuCompletion();
    const unsigned ioDepth = progArgs->getIODepth();

    // S T E P 1: retrieve multipart upload ID from server

    Aws::String uploadID = s3SharedUploadStore.getMultipartUploadID(
        bucketName, objectName, s3Client, opsLog);

    std::unique_ptr<Aws::Vector<S3::CompletedPart>> allCompletedParts; // need sort before send

    // S T E P 2: upload parts

    std::vector<S3AsyncUploadPartContext> partCompletionsVec;

    partCompletionsVec.reserve(ioDepth); /* (reserve is important to not invalidate pointers for
        async callbacks on vector grow) */

    /* NOTE on try-catch: before we can exit this function we need to wait for all async tasks to
        complete because they might still be accessing members of partCompletionsVec. */
    try
    {
        while(worker.rwOffsetGen->getNumBytesLeftToSubmit() )
        {
            // S T E P 2.1: submit parts asynchronously up to iodepth

            for(unsigned currentIODepth = 0;
                (currentIODepth < ioDepth) && worker.rwOffsetGen->getNumBytesLeftToSubmit();
                currentIODepth++)
            {
                const size_t blockSize = worker.rwOffsetGen->getNextBlockSizeToSubmit();
                const uint64_t currentOffset = worker.rwOffsetGen->getNextOffset();

                /* note: this object is shared by multiple concurrent worker threads, each
                    covering a different (non-zero-based) byte range of the object, so the part
                    number has to be derived from the absolute offset. (A local incrementing
                    counter, as used in the single-owner upload functions, would not work here:
                    it can't be combined with a block size mix, which ProgArgs rejects for
                    shared multipart uploads.) */
                const uint64_t currentPartNum =
                    1 + (currentOffset / worker.rwOffsetGen->getBlockSize() ); // +1 for 1-based range

                worker.rwRateLimiter(blockSize);

                /* note: nothing that throws an exception (e.g. funcRWRateLimiter for
                    "--rwmixthrpct") must be between emplace_back and UploadPartAsync()
                    because otherwise the wait loop in catch() will wait infinitely. */
                S3AsyncUploadPartContext& asyncPartContext = partCompletionsVec.emplace_back(
                    currentOffset, blockSize, currentPartNum);

                asyncPartContext.ioStartT = std::chrono::steady_clock::now();

                worker.preWriteBlockModifier(worker.ioBufVec[currentIODepth],
                    worker.gpuIOBufVec[currentIODepth], blockSize, currentOffset);
                worker.preWriteCudaMemcpy(worker.ioBufVec[currentIODepth],
                    worker.gpuIOBufVec[currentIODepth], blockSize);

                // prepare part upload

                S3Transport::UploadPartRequest& uploadPartRequest =
                    asyncPartContext.uploadPartRequest;
                uploadPartRequest.WithBucket(bucketName)
                    .WithKey(objectName)
                    .WithUploadId(uploadID)
                    .WithPartNumber(currentPartNum)
                    .WithContentLength(blockSize);

                // (no addServerSideEncryptionHeaders() because this one is only for SSE-C)
                if(!s3SSECKey.empty() )
                    uploadPartRequest.WithSSECustomerAlgorithm("AES256")
                            .WithSSECustomerKey(s3SSECKey)
                            .WithSSECustomerKeyMD5(s3SSECKeyMD5);

                IF_UNLIKELY(s3ChecksumAlgorithm != S3ChecksumAlgorithm::NOT_SET)
                    S3Tk::addUploadPartRequestChecksum(uploadPartRequest,
                        &asyncPartContext.completedPart, s3ChecksumAlgorithm,
                        (unsigned char*) worker.ioBufVec[currentIODepth], blockSize);

                #if !defined(S3_AWSCRT) || AWS_SDK_AT_LEAST(1, 11, 708)
                    uploadPartRequest.SetContinueRequestHandler(
                        [&isInterruptionRequested = worker.isInterruptionRequested]
                        (const Aws::Http::HttpRequest* request)
                        { return !isInterruptionRequested.load(); } );
                #endif // !S3_AWSCRT or AWS SDK >= 1.11.708

                OPLOG_PRE_OP("S3UploadPartAsync", bucketName + "/" + objectName, currentOffset,
                    blockSize);

                transport.uploadPartAsync(*s3Client, uploadPartRequest,
                    worker.ioBufVec[currentIODepth], worker.gpuIOBufVec[currentIODepth], blockSize,
                    worker.atomicLiveOps, asyncPartContext.partCompletePromise);

                worker.numIOPSSubmitted++;
                worker.rwOffsetGen->addBytesSubmitted(asyncPartContext.blockSize);

            } // enf of step 2.1: async submit for-loop

            // S T E P 2.2: handle completion of submitted parts

            for(unsigned currentIODepth = 0;
                currentIODepth < partCompletionsVec.size();
                currentIODepth++)
            {
                S3AsyncUploadPartContext& asyncPartContext = partCompletionsVec[currentIODepth];

                // wait for part upload to complete ("future.get()" blocks)
                S3Transport::UploadPartOutcome uploadPartOutcome =
                    asyncPartContext.partCompleteFuture.get();

                OPLOG_POST_OP("S3UploadPartAsync", bucketName + "/" + objectName,
                    asyncPartContext.currentOffset, asyncPartContext.blockSize,
                    !uploadPartOutcome.IsSuccess() );

                worker.checkInterruptionRequest(); /* (placed here to avoid outcome check on interruption;
                    abort message will be sent during s3SharedUploadStore cleanup) */

                IF_UNLIKELY(!uploadPartOutcome.IsSuccess() )
                {
                    // (note: abort message will be sent during s3SharedUploadStore cleanup)

                    auto s3Error = uploadPartOutcome.GetError();

                    throw WorkerException(std::string("Shared multipart part upload failed. ") +
                        "Endpoint: " + s3EndpointStr + "; "
                        "Bucket: " + bucketName + "; "
                        "Object: " + objectName + "; "
                        "Part: " + std::to_string(asyncPartContext.partNum) + "; "
                        "UploadID: " + uploadID + "; "
                        "Rank: " + std::to_string(workerRank) + "; "
                        "Exception: " + s3Error.GetExceptionName() + "; " +
                        "Message: " + s3Error.GetMessage() + "; " +
                        "HTTP Error Code: " + std::to_string( (int)s3Error.GetResponseCode() ) +
                            " (" +
                            TranslatorTk::httpErrorCodeToHumanStr( (int)s3Error.GetResponseCode() ) +
                            "); " +
                        "Request ID: " + s3Error.GetRequestId() );
                }

                // mark part as completed

                auto partETag = uploadPartOutcome.GetResult().GetETag();
                asyncPartContext.completedPart.SetETag(partETag);

                allCompletedParts = s3SharedUploadStore.addCompletedPart(bucketName, objectName,
                    asyncPartContext.blockSize, objectTotalSize, asyncPartContext.completedPart);

                worker.postReadCudaMemcpy(
                    worker.ioBufVec[currentIODepth], worker.gpuIOBufVec[currentIODepth],
                    asyncPartContext.blockSize);
                worker.postReadBlockChecker(
                    worker.ioBufVec[currentIODepth], worker.gpuIOBufVec[currentIODepth],
                    asyncPartContext.blockSize, asyncPartContext.currentOffset);

                // calc io operation latency
                std::chrono::steady_clock::time_point ioEndT = std::chrono::steady_clock::now();
                std::chrono::microseconds ioElapsedMicroSec =
                    std::chrono::duration_cast<std::chrono::microseconds>
                    (ioEndT - asyncPartContext.ioStartT);

                worker.iopsLatHisto.addLatency(ioElapsedMicroSec.count() );

                worker.atomicLiveOps.numIOPSDone++;

                // sanity check after rwOffsetGen update
                IF_UNLIKELY(allCompletedParts && worker.rwOffsetGen->getNumBytesLeftToSubmit() )
                    throw WorkerException(std::string("Shared multipart upload logic error. ") +
                        "Completion returned by upload store, but bytes left to submit. "
                        "Endpoint: " + s3EndpointStr + "; "
                        "Bucket: " + bucketName + "; "
                        "Object: " + objectName + "; "
                        "Part: " + std::to_string(asyncPartContext.partNum) + "; "
                        "BytesLeft: " + std::to_string(worker.rwOffsetGen->getNumBytesLeftToSubmit() ) +
                            "; " +
                        "ObjectSize: " + std::to_string(objectTotalSize) );
            } // end of step 2.2: wait for part batch completion for-loop

            worker.checkInterruptionRequest(); /* (placed here to avoid outcome check on interruption;
                abort message will be sent during s3SharedUploadStore cleanup) */

            // reset completions vec for next round
            partCompletionsVec.clear();

        } // end of step 2 loop: submit parts + wait for completions
    }
    catch(...)
    {
        worker.interruptExecution(); // for SetContinueRequestHandler()

        // wait for all parts to complete ("future.get()" blocks)
        for(unsigned i = 0; i < partCompletionsVec.size(); i++)
            SAFE_FUTURE_GET_IGNORE_ERR(partCompletionsVec[i].partCompleteFuture);

        /* note: no s3AbortMultipartUpload() here, abort message will be sent during
            s3SharedUploadStore cleanup */

        throw;
    }

    // S T E P 3: submit upload completion

    IF_UNLIKELY(s3NoMpuCompletion)
        return; // user-defined skip of completion message

    if(!allCompletedParts)
        return; // another worker still needs to upload parts, so no completion yet

    // (note: part vec must be in ascending order)
    std::sort(allCompletedParts->begin(), allCompletedParts->end(),
        [](const S3::CompletedPart& a, const S3::CompletedPart& b) -> bool
        { return a.GetPartNumber() < b.GetPartNumber(); } );

    S3::CompletedMultipartUpload completedMultipartUpload;
    completedMultipartUpload.SetParts(*allCompletedParts);

    S3::CompleteMultipartUploadRequest completionRequest;
    completionRequest.WithBucket(bucketName)
        .WithKey(objectName)
        .WithUploadId(uploadID)
        .WithMultipartUpload(completedMultipartUpload);

    OPLOG_PRE_OP("S3CompleteMultipartUpload", bucketName + "/" + objectName, 0, objectTotalSize);

    auto completionOutcome = s3Client->CompleteMultipartUpload(completionRequest);

    OPLOG_POST_OP("S3CompleteMultipartUpload", bucketName + "/" + objectName, 0, objectTotalSize,
        !completionOutcome.IsSuccess() );


    IF_UNLIKELY(!completionOutcome.IsSuccess() )
    {
        auto s3Error = completionOutcome.GetError();

        if (!(s3Args.getS3IgnoreMultipartUpload404() &&
              s3Error.GetResponseCode() == Aws::Http::HttpResponseCode::NOT_FOUND))
        {
            abortMultipartUpload(bucketName, objectName, uploadID);

            throw WorkerException(std::string("Shared multipart upload completion failed. ") +
                "Endpoint: " + s3EndpointStr + "; "
                "Bucket: " + bucketName + "; "
                "Object: " + objectName + "; "
                "NumParts: " + std::to_string(completedMultipartUpload.GetParts().size() ) + "; "
                "Exception: " + s3Error.GetExceptionName() + "; " +
                "Message: " + s3Error.GetMessage() + "; " +
                "HTTP Error Code: " + std::to_string( (int)s3Error.GetResponseCode() ) + " (" +
                    TranslatorTk::httpErrorCodeToHumanStr( (int)s3Error.GetResponseCode() ) +
                    "); " +
                "Request ID: " + s3Error.GetRequestId() );
        }
    }
}

/**
 * Retrieves all parts of a multipart upload based on given uploadID, verifies the total size,
 * and completes the upload.
 *
 * @param bucketName The name of the S3 bucket.
 * @param objectKey The key (path/filename) of the S3 object.
 * @param uploadId The Multipart Upload ID string.
 * @param expectedTotalSize The combined size of all uploaded parts in bytes.
 * @return true if the upload was successfully completed, false otherwise.
 */
void S3Mode::queryAndFinishMultipartUpload(std::string bucketName,
    std::string objectName, std::string uploadID, uint64_t expectedTotalSize)
{
    S3::ListPartsRequest listPartsRequest;
    listPartsRequest.SetBucket(bucketName);
    listPartsRequest.SetKey(objectName);
    listPartsRequest.SetUploadId(uploadID);

    std::vector<S3::CompletedPart> completedParts;
    uint64_t actualTotalSize = 0; // sum of parts sizes from listing
    bool isTruncated = true; // for pagination >1000 parts
    int nextPartNumberMarker = 0; // for pagination >1000 parts

    LOGGER_DEBUG_BUILD("Retrieving parts for Upload ID: '" << uploadID << "'" <<
        "Worker: " << workerRank << "Obj: " << bucketName + "/" + objectName << std::endl);

    // STEP 1: Retrieve all uploaded parts (loop on pagination)

    while(isTruncated)
    {
        OPLOG_PRE_OP("S3ListParts", bucketName + "/" + objectName, nextPartNumberMarker, 0);

        auto outcome = s3Client->ListParts(listPartsRequest);

        OPLOG_POST_OP("S3ListParts", bucketName + "/" + objectName, nextPartNumberMarker,
            outcome.IsSuccess() ? outcome.GetResult().GetParts().size() : 0, !outcome.IsSuccess() );

        throwOnError(outcome, "Listing parts of unfinished MPU failed.", bucketName,
            objectName);

        const auto& result = outcome.GetResult();

        for(const auto& part : result.GetParts() )
        {
            S3::CompletedPart completedPart;
            completedPart.SetPartNumber(part.GetPartNumber() );
            completedPart.SetETag(part.GetETag() );

            completedParts.push_back(completedPart);
            actualTotalSize += part.GetSize();
        }

        isTruncated = result.GetIsTruncated();
        if(isTruncated)
        { // pagination
            nextPartNumberMarker = result.GetNextPartNumberMarker();
            listPartsRequest.SetPartNumberMarker(nextPartNumberMarker);
        }
    }

    LOGGER_DEBUG_BUILD("Retrieved parts: " << completedParts.size() << "; "
        "Worker: " << workerRank << std::endl);

    // STEP 2: Verify the total size against the expected size

    IF_UNLIKELY(actualTotalSize != expectedTotalSize)
    {
        throw WorkerException("MPU parts listing result does not match expected size. "
            "Bucket: " + bucketName + "; "
            "Object: " + objectName + "; "
            "ExpectedSize: " + std::to_string(expectedTotalSize) + "; "
            "ActualSize: " + std::to_string(actualTotalSize) + "; "
            "ListedNumParts: " + std::to_string(completedParts.size() ) + "; "
            "MpuID: " + uploadID);
    }

    // STEP 3: Assemble the completion structure

    S3::CompletedMultipartUpload completedUpload;
    completedUpload.SetParts(completedParts);

    S3::CompleteMultipartUploadRequest completeRequest;
    completeRequest.SetBucket(bucketName);
    completeRequest.SetKey(objectName);
    completeRequest.SetUploadId(uploadID);
    completeRequest.SetMultipartUpload(completedUpload);

    LOGGER_DEBUG_BUILD("Sending CompleteMultipartUpload request... "
        "Worker: " << workerRank << std::endl);

    // STEP 4: Send the completion request

    OPLOG_PRE_OP("S3CompleteMultipartUpload", bucketName + "/" + objectName, 0,
        completedUpload.GetParts().size() );

    auto completeOutcome = s3Client->CompleteMultipartUpload(completeRequest);

    OPLOG_POST_OP("S3CompleteMultipartUpload", bucketName + "/" + objectName, 0,
        completedUpload.GetParts().size(), !completeOutcome.IsSuccess() );

    throwOnError(completeOutcome, "Error completing multipart upload.", bucketName,
        objectName);

    LOGGER_DEBUG_BUILD("Successfully completed multipart upload for object: " << objectName << " "
        "Worker: " << workerRank << std::endl);
}

/**
 * Abort an incomplete S3 multipart upload.
 *
 * @return true if abort request succeeded, false otherwise.
 */
bool S3Mode::abortMultipartUpload(std::string bucketName, std::string objectName,
    std::string uploadID)
{
    if(!s3Client)
        return false;

    S3::AbortMultipartUploadRequest abortMultipartUploadRequest;

    abortMultipartUploadRequest.SetBucket(bucketName);
    abortMultipartUploadRequest.SetKey(objectName);
    abortMultipartUploadRequest.SetUploadId(uploadID);

    OPLOG_PRE_OP("S3AbortMultipartUpload", bucketName + "/" + objectName, 0, 0);

    auto abortOutcome = s3Client->AbortMultipartUpload(abortMultipartUploadRequest);

    OPLOG_POST_OP("S3AbortMultipartUpload", bucketName + "/" + objectName, 0, 0,
        !abortOutcome.IsSuccess() );

    return abortOutcome.IsSuccess();
}

/**
 * Block-sized download of an S3 object.
 *
 * This will delegate to downloadObjectAsync() if iodepth > 1.
 *
 * @isRWMixedReader true if this is a reader of a mixed read/write phase, so that the corresponding
 *      statistics get increased.
 *
 * @throw WorkerException on error.
 */
void S3Mode::downloadObject(std::string bucketName, std::string objectName,
    const bool isRWMixedReader)
{
    const bool useS3FastRead = s3Args.getUseS3FastRead();
    const bool ignoreS3Errors = s3Args.getIgnoreS3Errors();
    const uint64_t objectOffsetBase = progArgs->getFileOffset(); /* offset gen works in a logical
        range starting at 0, so the user-defined min offset gets added here */

    // async delegation: hand over to async function if iodepth is given
    if(progArgs->getIODepth() > 1)
    {
        downloadObjectAsync(bucketName, objectName, isRWMixedReader);
        return;
    }

    // download one block-sized chunk in each loop pass
    while(worker.rwOffsetGen->getNumBytesLeftToSubmit() )
    {
        const uint64_t currentOffset = worker.rwOffsetGen->getNextOffset() + objectOffsetBase;
        const size_t blockSize = worker.rwOffsetGen->getNextBlockSizeToSubmit();

        std::string objectRange = "bytes=" + std::to_string(currentOffset) + "-" +
            std::to_string(currentOffset+blockSize-1);

        char* ioBuf = useS3FastRead ? NULL : worker.ioBufVec[0];
        char* gpuIOBuf = useS3FastRead ? NULL : worker.gpuIOBufVec[0];

        worker.rwRateLimiter(blockSize);

        std::chrono::steady_clock::time_point ioStartT = std::chrono::steady_clock::now();

        worker.preWriteBlockModifier(ioBuf, gpuIOBuf, blockSize, currentOffset);
        worker.preWriteCudaMemcpy(ioBuf, gpuIOBuf, blockSize);

        S3Transport::GetObjectRequest request;
        request.WithBucket(bucketName)
            .WithKey(objectName)
            .WithRange(objectRange);

        // (no addServerSideEncryptionHeaders() because this one is only for SSE-C)
        if(!s3SSECKey.empty())
            request.WithSSECustomerAlgorithm("AES256")
                    .WithSSECustomerKey(s3SSECKey)
                    .WithSSECustomerKeyMD5(s3SSECKeyMD5);

        #if !defined(S3_AWSCRT) || AWS_SDK_AT_LEAST(1, 11, 708)
            request.SetContinueRequestHandler( [&](const Aws::Http::HttpRequest* request)
                { return !worker.isInterruptionRequested.load(); } );
        #endif // !S3_AWSCRT or AWS SDK >= 1.11.708

        OPLOG_PRE_OP("S3GetObject", bucketName + "/" + objectName, currentOffset, blockSize);

        S3Transport::GetObjectOutcome outcome = transport.getObject(*s3Client, request, ioBuf,
            gpuIOBuf, currentOffset, blockSize,
            isRWMixedReader ? worker.atomicLiveOpsReadMix : worker.atomicLiveOps);

        OPLOG_POST_OP("S3GetObject", bucketName + "/" + objectName, currentOffset, blockSize,
            !outcome.IsSuccess() );

        worker.checkInterruptionRequest(); // (placed here to avoid outcome check on interruption)

        IF_UNLIKELY(!outcome.IsSuccess() && !ignoreS3Errors)
            throwOnError(outcome, "Object download failed.", bucketName, objectName);

        IF_UNLIKELY( (S3Transport::getBytesReceived(outcome) < blockSize) && !ignoreS3Errors)
        {
            throw WorkerException(std::string("Object too small. ") +
                "Endpoint: " + s3EndpointStr + "; "
                "Bucket: " + bucketName + "; "
                "Object: " + objectName + "; "
                "Offset: " + std::to_string(currentOffset) + "; "
                "Requested blocksize: " + std::to_string(blockSize) + "; "
                "Received length: " + std::to_string(S3Transport::getBytesReceived(outcome) ) );
        }

        worker.postReadCudaMemcpy(ioBuf, gpuIOBuf, blockSize);
        worker.postReadBlockChecker(ioBuf, gpuIOBuf, blockSize, currentOffset);

        // calc io operation latency
        std::chrono::steady_clock::time_point ioEndT = std::chrono::steady_clock::now();
        std::chrono::microseconds ioElapsedMicroSec =
            std::chrono::duration_cast<std::chrono::microseconds>
            (ioEndT - ioStartT);

        if(isRWMixedReader)
        {
            worker.iopsLatHistoReadMix.addLatency(ioElapsedMicroSec.count() );
            worker.atomicLiveOpsReadMix.numIOPSDone++;
        }
        else
        {
            worker.iopsLatHisto.addLatency(ioElapsedMicroSec.count() );
            worker.atomicLiveOps.numIOPSDone++;
        }

        worker.numIOPSSubmitted++;
        worker.rwOffsetGen->addBytesSubmitted(blockSize);
    }
}

/**
 * Async block-sized download of an S3 object.
 *
 * @isRWMixedReader true if this is a reader of a mixed read/write phase, so that the corresponding
 *      statistics get increased.
 *
 * @throw WorkerException on error.
 */
void S3Mode::downloadObjectAsync(std::string bucketName, std::string objectName,
    const bool isRWMixedReader)
{
    const bool useS3FastRead = s3Args.getUseS3FastRead();
    const bool ignoreS3Errors = s3Args.getIgnoreS3Errors();
    const unsigned ioDepth = progArgs->getIODepth();
    const uint64_t objectOffsetBase = progArgs->getFileOffset(); /* offset gen works in a logical
        range starting at 0, so the user-defined min offset gets added here */

    std::vector<S3AsyncDownloadContext> partCompletionsVec;

    partCompletionsVec.reserve(ioDepth); /* (reserve is important to not invalidate pointers for
        async callbacks on vector grow) */


    /* NOTE on try-catch: before we can exit this function we need to wait for all async tasks to
        complete because they might still be accessing members of partCompletionsVec. */
    try
    {
        // download block-sized chunks async up to iodepth in each loop pass
        while(worker.rwOffsetGen->getNumBytesLeftToSubmit() )
        {
            // S T E P 1: submit parts asynchronously up to iodepth

            for(unsigned currentIODepth = 0;
                (currentIODepth < ioDepth) && worker.rwOffsetGen->getNumBytesLeftToSubmit();
                currentIODepth++)
            {
                const uint64_t currentOffset =
                    worker.rwOffsetGen->getNextOffset() + objectOffsetBase;
                const size_t blockSize = worker.rwOffsetGen->getNextBlockSizeToSubmit();

                worker.rwRateLimiter(blockSize);

                /* note: nothing that throws an exception (e.g. funcRWRateLimiter for
                    "--rwmixthrpct") must be between emplace_back and asyncPartContext.request()
                    because otherwise the wait loop in catch() will wait infinitely. */
                S3AsyncDownloadContext& asyncPartContext = partCompletionsVec.emplace_back(
                    currentOffset, blockSize);

                std::string objectRange = "bytes=" + std::to_string(currentOffset) + "-" +
                    std::to_string(currentOffset+blockSize-1);

                char* ioBuf = useS3FastRead ? NULL : worker.ioBufVec[currentIODepth];
                char* gpuIOBuf = useS3FastRead ? NULL : worker.gpuIOBufVec[currentIODepth];

                asyncPartContext.ioStartT = std::chrono::steady_clock::now();

                worker.preWriteBlockModifier(ioBuf, gpuIOBuf, blockSize, currentOffset);
                worker.preWriteCudaMemcpy(ioBuf, gpuIOBuf, blockSize);

                S3Transport::GetObjectRequest& request = asyncPartContext.request;
                request.WithBucket(bucketName)
                    .WithKey(objectName)
                    .WithRange(objectRange);

                // (no addServerSideEncryptionHeaders() because this one is only for SSE-C)
                if(!s3SSECKey.empty())
                    request.WithSSECustomerAlgorithm("AES256")
                            .WithSSECustomerKey(s3SSECKey)
                            .WithSSECustomerKeyMD5(s3SSECKeyMD5);

                #if !defined(S3_AWSCRT) || AWS_SDK_AT_LEAST(1, 11, 708)
                    request.SetContinueRequestHandler(
                        [&isInterruptionRequested = worker.isInterruptionRequested]
                        (const Aws::Http::HttpRequest* request)
                        { return !isInterruptionRequested.load(); } );
                #endif // !S3_AWSCRT or AWS SDK >= 1.11.708

                OPLOG_PRE_OP("S3GetObjectAsync", bucketName + "/" + objectName, currentOffset,
                    blockSize);

                transport.getObjectAsync(*s3Client, request, ioBuf, gpuIOBuf, currentOffset,
                    blockSize, isRWMixedReader ? worker.atomicLiveOpsReadMix : worker.atomicLiveOps,
                    asyncPartContext.partCompletePromise);

                worker.numIOPSSubmitted++;
                worker.rwOffsetGen->addBytesSubmitted(blockSize);

            } // end of step 1: async submission loop


            // S T E P 2: handle completion of submitted part downloads

            for(unsigned currentIODepth = 0;
                currentIODepth < partCompletionsVec.size();
                currentIODepth++)
            {
                S3AsyncDownloadContext& asyncPartContext = partCompletionsVec[currentIODepth];

                // wait for this part to complete ("future.get()" blocks)
                S3Transport::GetObjectOutcome outcome = asyncPartContext.partCompleteFuture.get();

                OPLOG_POST_OP("S3GetObjectAsync", bucketName + "/" + objectName,
                    asyncPartContext.currentOffset, asyncPartContext.blockSize,
                    !outcome.IsSuccess() );

                worker.checkInterruptionRequest(); // (placed here to avoid outcome check on interruption)

                IF_UNLIKELY(!outcome.IsSuccess() && !ignoreS3Errors)
                    throwOnError(outcome, "Object download failed.", bucketName, objectName);

                IF_UNLIKELY(
                    (S3Transport::getBytesReceived(outcome) < asyncPartContext.blockSize) &&
                    !ignoreS3Errors)
                {
                    throw WorkerException(std::string("Object too small. ") +
                        "Endpoint: " + s3EndpointStr + "; "
                        "Bucket: " + bucketName + "; "
                        "Object: " + objectName + "; "
                        "Offset: " + std::to_string(asyncPartContext.currentOffset) + "; "
                        "Requested blocksize: " + std::to_string(asyncPartContext.blockSize) + "; "
                        "Received length: " +
                            std::to_string(S3Transport::getBytesReceived(outcome) ) );
                }

                char* ioBuf = useS3FastRead ? NULL : worker.ioBufVec[currentIODepth];
                char* gpuIOBuf = useS3FastRead ? NULL : worker.gpuIOBufVec[currentIODepth];

                worker.postReadCudaMemcpy(ioBuf, gpuIOBuf, asyncPartContext.blockSize);
                worker.postReadBlockChecker(ioBuf, gpuIOBuf, asyncPartContext.blockSize,
                    asyncPartContext.currentOffset);

                // calc io operation latency
                std::chrono::steady_clock::time_point ioEndT = std::chrono::steady_clock::now();
                std::chrono::microseconds ioElapsedMicroSec =
                    std::chrono::duration_cast<std::chrono::microseconds>
                    (ioEndT - asyncPartContext.ioStartT);

                if(isRWMixedReader)
                {
                    worker.iopsLatHistoReadMix.addLatency(ioElapsedMicroSec.count() );
                    worker.atomicLiveOpsReadMix.numIOPSDone++;
                }
                else
                {
                    worker.iopsLatHisto.addLatency(ioElapsedMicroSec.count() );
                    worker.atomicLiveOps.numIOPSDone++;
                }

            } // end of step 2: for-loop: async completion

            worker.checkInterruptionRequest();

            // reset completions vec for next round
            partCompletionsVec.clear();

        } // end of while-loop

    }
    catch(...)
    {
        worker.interruptExecution(); // for SetContinueRequestHandler()

        // wait for all parts to complete ("future.get()" blocks)
        for(unsigned i = 0; i < partCompletionsVec.size(); i++)
            SAFE_FUTURE_GET_IGNORE_ERR(partCompletionsVec[i].partCompleteFuture);

        throw;
    }
}

/**
 * Retrieve object metadata by sending a HeadObject request (the equivalent of a stat() call in the
 * file world).
 *
 * @throw WorkerException on error.
 */
void S3Mode::statObject(std::string bucketName, std::string objectName)
{
    S3::HeadObjectRequest request;
    request.WithBucket(bucketName)
        .WithKey(objectName);

    OPLOG_PRE_OP("S3HeadObject", bucketName + "/" + objectName, 0, 0);

    S3::HeadObjectOutcome outcome = s3Client->HeadObject(request);

    OPLOG_POST_OP("S3HeadObject", bucketName + "/" + objectName, 0, 0, !outcome.IsSuccess() );

    throwOnError(outcome, "Object metadata retrieval via HeadObject failed.", bucketName,
        objectName);
}

/**
 * Delete given S3 object.
 *
 * @throw WorkerException on error.
 */
void S3Mode::deleteObject(std::string bucketName, std::string objectName)
{
    const bool ignoreDelErrors = progArgs->getIgnoreDelErrors();

    S3::DeleteObjectRequest request;
    request.WithBucket(bucketName)
        .WithKey(objectName);

    OPLOG_PRE_OP("S3DeleteObject", bucketName + "/" + objectName, 0, 0);

    S3::DeleteObjectOutcome outcome = s3Client->DeleteObject(request);

    OPLOG_POST_OP("S3DeleteObject", bucketName + "/" + objectName, 0, 0, !outcome.IsSuccess() );

    IF_UNLIKELY(!outcome.IsSuccess() &&
        (!ignoreDelErrors ||
            (outcome.GetError().GetResponseCode() == Aws::Http::HttpResponseCode::NOT_FOUND) ) )
    {
        throwOnError(outcome, "Object deletion failed.", bucketName, objectName);
    }
}

/**
 * Verify expected and received dir listing. This includes a verification of the entry order
 * inside the listing.
 *
 * @listPrefix the prefix that was used in the object listing request.
 *
 * @throw WorkerException on error (e.g. mismatch between expected and received).
 */
void S3Mode::verifyListing(StringSet& expectedSet, StringList& receivedList,
    std::string bucketName, std::string listPrefix)
{
    if(expectedSet.size() != receivedList.size() )
        throw WorkerException(std::string("Object listing v2 verification failed. ") +
            "Number of expected and number of received entries differ. "
            "Endpoint: " + s3EndpointStr + "; "
            "Bucket: " + bucketName + "; "
            "Prefix: " + listPrefix + "; "
            "NumObjectsExpected: " + std::to_string(expectedSet.size() ) + "; " +
            "NumObjectsReceived: " + std::to_string(receivedList.size() ) );

    uint64_t currentOffset = 0; // offset inside listing

    while(!expectedSet.empty() && !receivedList.empty() )
    {
        if(*expectedSet.begin() == *receivedList.begin() )
        { // all good with this entry, so delete and move on to the next one
            expectedSet.erase(expectedSet.begin() );
            receivedList.erase(receivedList.begin() );

            currentOffset++;

            continue;
        }

        // entries differ, so verification failed

        throw WorkerException(std::string("Object listing v2 verification failed. ") +
            "Found object differs from expected object at offset. "
            "FoundObject: " + *receivedList.begin() + "; "
            "ExpectedObject: " + *expectedSet.begin() + "; "
            "ListingOffset: " + std::to_string(currentOffset) + "; "
            "Endpoint: " + s3EndpointStr + "; "
            "Bucket: " + bucketName + "; "
            "Prefix: " + listPrefix + "; "
            "NumObjectsExpected: " + std::to_string(expectedSet.size() ) + "; " +
            "NumObjectsReceived: " + std::to_string(receivedList.size() ) );
    }
}

/**
 * Put ACL of given S3 object.
 *
 * @throw WorkerException on error.
 */
void S3Mode::putObjectAcl(std::string bucketName, std::string objectName)
{
    S3::PutObjectAclRequest request;
    request.WithBucket(bucketName)
        .WithKey(objectName);

    S3AclTk::applyS3PutAclRequestGrants<S3::ObjectCannedACL>(progArgs, request);

    OPLOG_PRE_OP("S3PutObjectAcl", bucketName + "/" + objectName, 0, 0);

    S3::PutObjectAclOutcome outcome = s3Client->PutObjectAcl(request);

    OPLOG_POST_OP("S3PutObjectAcl", bucketName + "/" + objectName, 0, 0, !outcome.IsSuccess() );

    throwOnError(outcome, "Putting object ACL failed.", bucketName, objectName);
}

/**
 * Get ACL of given S3 object.
 *
 * @throw WorkerException on error.
 */
void S3Mode::getObjectAcl(std::string bucketName, std::string objectName)
{
    bool doS3AclVerify = s3Args.getDoS3AclVerify();

    S3::GetObjectAclRequest request;
    request.WithBucket(bucketName)
        .WithKey(objectName);

    OPLOG_PRE_OP("S3GetObjectAcl", bucketName + "/" + objectName, 0, 0);

    S3::GetObjectAclOutcome outcome = s3Client->GetObjectAcl(request);

    OPLOG_POST_OP("S3GetObjectAcl", bucketName + "/" + objectName, 0, 0, !outcome.IsSuccess() );

    throwOnError(outcome, "Getting object ACL failed.", bucketName, objectName);

    IF_UNLIKELY(doS3AclVerify)
    {
        // check canned ACL as special grantee...

        S3::BucketCannedACL cannedAcl = S3::BucketCannedACLMapper::GetBucketCannedACLForName(
            s3Args.getS3AclGrantee() );

        /* note: checking for ::NOT_SET alone here is not enough, because GetObjectCannedACLForName()
            can return other values if granteeStr doesn't match another enum value. */
        switch( (S3::ObjectCannedACL)cannedAcl)
        {
            case S3::ObjectCannedACL::private_:
            case S3::ObjectCannedACL::public_read:
            case S3::ObjectCannedACL::public_read_write:
            case S3::ObjectCannedACL::authenticated_read:
            case S3::ObjectCannedACL::aws_exec_read:
            case S3::ObjectCannedACL::bucket_owner_read:
            case S3::ObjectCannedACL::bucket_owner_full_control:
            { // found canned ACL as special grantee
                throw WorkerException("Verification of canned ACLs is not supported.");
            }

            case S3::ObjectCannedACL::NOT_SET:
            default:
            { // normal grantee, not a canned ACL
                break;
            }
        }

        // check list of grants...

        std::vector<S3::Grant> verifyGrants;
        S3AclTk::getS3ObjectAclGrants(progArgs, verifyGrants);

        const std::vector<S3::Grant>& outcomeGrants = outcome.GetResult().GetGrants();

        // iterate over all grants that need to be verified
        for(S3::Grant& verifyGrant : verifyGrants)
        {
            bool grantFound = false;

            // iterate over all outcome grants to see if any grant matches current verifyGrant
            for(const S3::Grant& outcomeGrant : outcomeGrants)
            {
                if( (outcomeGrant.GetGrantee().GetID() ==
                        verifyGrant.GetGrantee().GetID() ) ||
                    (outcomeGrant.GetGrantee().GetEmailAddress() ==
                        verifyGrant.GetGrantee().GetEmailAddress() ) ||
                    (outcomeGrant.GetGrantee().GetURI() ==
                        verifyGrant.GetGrantee().GetURI() ) )
                { // grantee matches => check if permission also matches
                    if(outcomeGrant.GetPermission() == verifyGrant.GetPermission() )
                    { // permission matches
                        grantFound = true;
                        break;
                    }
                }
            }

            if(!grantFound)
                throw WorkerException(std::string("S3 ACL verification failed. ") +
                    "Endpoint: " + s3EndpointStr + "; "
                    "Bucket: " + bucketName + "; "
                    "Object: " + objectName + "; "
                    "Grantee ID: " + verifyGrant.GetGrantee().GetID() + "; "
                    "Grantee Email: " + verifyGrant.GetGrantee().GetEmailAddress() + "; "
                    "Grantee URI: " + verifyGrant.GetGrantee().GetURI() + "; "
                    "Permission: " + S3AclTk::s3AclPermissionToStr(
                        verifyGrant.GetPermission() ) );
        }
    } // end of verifcation
}

void S3Mode::getObjectTags(const std::string& bucketName, const std::string& objectName)
{
    OPLOG_PRE_OP("GetObjectTagging", bucketName + "/" + objectName, 0, 0);

    const auto getTagOutcome = s3Client->GetObjectTagging(
        S3::GetObjectTaggingRequest()
            .WithBucket(bucketName)
            .WithKey(objectName)
    );

    OPLOG_POST_OP("GetObjectTagging", bucketName + "/" + objectName, 0, 0,
        !getTagOutcome.IsSuccess());

    throwOnError(getTagOutcome, "Get object tagging failed.", bucketName, objectName);

    // Continue only if we need to verify
    if (!s3Args.getDoS3ObjectTaggingVerify())
        return;

    const auto& tagSet = getTagOutcome.GetResult().GetTagSet();

    IF_UNLIKELY(tagSet.empty())
    {
        std::stringstream errStr;
        errStr << "Object has no tags, but 1 tag was expected" << std::endl
               << "Bucket: " << bucketName << "; "
               << "Key: " << objectName << std::endl
               << "Tag: " << TAG_KEY_MEDIUM_NAME << std::endl;
        throw WorkerException(errStr.str());
    }

    const auto& firstTag = tagSet.front();

    IF_UNLIKELY(!StringTk::verifyRandomS3TagValue(firstTag.GetValue(), objectName))
    {
        std::stringstream errStr;
        errStr << "Random tag value is corrupted (invalid checksum). "
               << "Bucket: " << bucketName << "; "
               << "Key: " << objectName << std::endl
               << "Tag: " << firstTag.GetKey() << "=" << firstTag.GetValue() << std::endl;
        throw WorkerException(errStr.str());
    }
}

void S3Mode::putObjectTags(const std::string& bucketName, const std::string& objectName)
{
    const auto tag = S3::Tag()
        .WithKey(TAG_KEY_MEDIUM_NAME)
        .WithValue(StringTk::generateRandomS3TagValue(objectName, TAG_VALUE_MEDIUM_LEN));

    S3::PutObjectTaggingRequest request;
    request.WithBucket(bucketName)
          .WithKey(objectName)
          .WithTagging(S3::Tagging().AddTagSet(tag));

    addChecksumAlgorithm(request);

    OPLOG_PRE_OP("PutObjectTagging", bucketName + "/" + objectName, 0, TAG_VALUE_MEDIUM_LEN);

    const auto putTagOutcome = s3Client->PutObjectTagging(request);

    OPLOG_POST_OP("PutObjectTagging", bucketName + "/" + objectName,
                  0, TAG_VALUE_MEDIUM_LEN, !putTagOutcome.IsSuccess());

    throwOnError(putTagOutcome, "Put object tagging failed.", bucketName, objectName);
}

void S3Mode::deleteObjectTags(const std::string& bucketName, const std::string& objectName)
{
    OPLOG_PRE_OP("DeleteObjectTagging", bucketName + "/" + objectName, 0, 0);

    const auto delTagOutcome = s3Client->DeleteObjectTagging(
        S3::DeleteObjectTaggingRequest()
            .WithBucket(bucketName)
            .WithKey(objectName)
    );

    OPLOG_POST_OP("DeleteObjectTagging", bucketName + "/" + objectName, 0, 0,
        !delTagOutcome.IsSuccess());

    throwOnError(delTagOutcome, "Delete object tagging failed.", bucketName, objectName);
}

void S3Mode::getObjectLockConfiguration(const std::string &bucketName)
{
    OPLOG_PRE_OP("GetObjectLockConfiguration", bucketName, 0, 0);

    const auto getObjLockOutcome = s3Client->GetObjectLockConfiguration(
        S3::GetObjectLockConfigurationRequest().WithBucket(bucketName)
    );

    OPLOG_POST_OP("GetObjectLockConfiguration", bucketName, 0, 0, !getObjLockOutcome.IsSuccess());

    throwOnError(getObjLockOutcome, "Get object lock configuration failed.", bucketName);

    // Continue only if we need to verify
    if (!s3Args.getDoS3ObjectLockConfigurationVerify())
        return;

    const auto objLockCfg = getObjLockOutcome.GetResult().GetObjectLockConfiguration();

    IF_UNLIKELY(!objLockCfg.ObjectLockEnabledHasBeenSet())
        throw WorkerException("Object lock has not been enabled for bucket: " + bucketName);

    IF_UNLIKELY(!objLockCfg.RuleHasBeenSet())
        throw WorkerException("Object lock rule has not been set for bucket: " + bucketName);

    const auto objRetentionRule = objLockCfg.GetRule();

    IF_UNLIKELY(!objRetentionRule.DefaultRetentionHasBeenSet())
        throw WorkerException("Object lock default retention has not been set for bucket: " +
            bucketName);

    const auto objDefaultRetention = objRetentionRule.GetDefaultRetention();

    IF_UNLIKELY(!objDefaultRetention.DaysHasBeenSet())
        throw WorkerException("Object lock retention days have not been set for bucket: " +
            bucketName);

    const auto retentionDays = objDefaultRetention.GetDays();

    IF_UNLIKELY(retentionDays != RETENTION_PERIOD_DAYS)
    {
        std::stringstream errStr;
        errStr << "Object lock default retention was set to '" << retentionDays <<
            " days' in bucket '" << bucketName << "', but the expected value was '" <<
            RETENTION_PERIOD_DAYS << " days'";
        throw WorkerException(errStr.str());
    }
}

void S3Mode::putObjectLockConfiguration(const std::string &bucketName, bool unset)
{
    S3::ObjectLockConfiguration objectLockCfg;

    if (unset)
        objectLockCfg.SetObjectLockEnabled(S3::ObjectLockEnabled::NOT_SET);
    else
    {
        objectLockCfg.SetObjectLockEnabled(S3::ObjectLockEnabled::Enabled);
        objectLockCfg.SetRule(
            S3::ObjectLockRule().WithDefaultRetention(
                S3::DefaultRetention().WithMode(S3::ObjectLockRetentionMode::COMPLIANCE).WithDays(1)
            )
        );
    }

    OPLOG_PRE_OP("PutObjectLockConfiguration", bucketName, 0, 0);

    const auto putObjLockOutcome = s3Client->PutObjectLockConfiguration(
        S3::PutObjectLockConfigurationRequest()
            .WithBucket(bucketName)
            .WithObjectLockConfiguration(objectLockCfg));

    OPLOG_POST_OP("PutObjectLockConfiguration", bucketName, 0, 0, !putObjLockOutcome.IsSuccess());

    throwOnError(putObjLockOutcome, "Put object lock configuration failed.", bucketName);
}

/**
 * In S3 mode, replace any sequence of at least 3 consecutive RAND_PREFIX_MARK_CHAR chars with a
 * random uppercase hex string based on worker rank, dir index and file index. It's based on these
 * so that we can calculate the same random values again later to find the files.
 *
 * Note: It's a good idea to check s3Args.getUseS3ObjectPrefixRand() to avoid calling this
 *      unnecessairly.
 *
 * @objectPrefix string in which to repace the consecutive occurences of RAND_PREFIX_MARK_CHAR.
 * @return objectPrefix with replaced RAND_PREFIX_MARK_CHAR chars.
 */
std::string S3Mode::getRandObjectPrefix(size_t workerRank, size_t dirIdx,
    size_t fileIdx, const std::string& objectPrefix)
{
    size_t threeMarksPos = objectPrefix.find(RAND_PREFIX_MARKS_SUBSTR);

    if(threeMarksPos == std::string::npos)
        return objectPrefix; // not found, so nothing to replace here

    std::string randObjectPrefix(objectPrefix); // the copy to replace chars

    // we don't want any zero-based to turn result to all-zero (e.g. "-n 0" would always be 0)
    workerRank++;
    dirIdx++;
    fileIdx++;

    uint64_t randomNum = RandAlgoGoldenPrime(workerRank * dirIdx * fileIdx).next();

    for(size_t i = threeMarksPos;
        (i < objectPrefix.size() ) && (objectPrefix[i] == RAND_PREFIX_MARK_CHAR);
        i++)
    {
        randObjectPrefix[i] = ( (char*)HEX_ALPHABET)[randomNum % HEX_ALPHABET_LEN];

        randomNum /= HEX_ALPHABET_LEN;
    }

    return randObjectPrefix;
}

#endif // S3_SUPPORT
