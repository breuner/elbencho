// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef MODES_S3_S3MODE_H_
#define MODES_S3_S3MODE_H_

#include <memory>
#include <string>

#include "Common.h"
#include "modes/s3/S3Transport.h"
#include "modes/s3/S3UploadStore.h"
#include "toolkits/OpsLogger.h"
#include "modes/s3/toolkits/S3Tk.h"

class LocalWorker;
class ProgArgs;
class S3ProgArgs;


/**
 * The S3 benchmark mode of a LocalWorker thread: bucket & object operations for all phases.
 *
 * This is a friend of LocalWorker and works directly on its buffers, offset generator and
 * statistics, so that it adds no indirection to the I/O path.
 */
class S3Mode
{
    public:
        explicit S3Mode(LocalWorker& worker);

        void init();
        void uninit();
        bool prepareCustomTreePathStores(bool throwOnSmallerThanBlockSize);

        void iterateBuckets();
        void iterateObjects();
        void iterateCustomObjects();
        void iterateAndCompleteMpuIDs();
        void listObjects();
        void listObjParallel();
        void listAndMultiDeleteObjects();
        void abortUnfinishedSharedUploads();
        bool getDoReverseSeqFallback();

        /**
         * @return true if the transport moves object data directly from/to the GPU buffers.
         */
        bool isGpuDirect() const
        {
        #ifdef S3_SUPPORT
            return transport.isGpuDirect();
        #else
            return false;
        #endif // S3_SUPPORT
        }

        /**
         * Called by ProgArgs to reset singleton members.
         */
        static void resetSingletons()
        {
        #ifdef S3_SUPPORT
            s3SharedUploadStore.reset();
        #endif // S3_SUPPORT
        }

    private:
        LocalWorker& worker;
        ProgArgs* progArgs; // shortcut for worker member
        const S3ProgArgs& s3Args; // shortcut for progArgs member
        size_t workerRank; // shortcut for worker member
        OpsLogger& opsLog; // shortcut for worker member (name is required by OPLOG_* macros)

#ifdef S3_SUPPORT
        std::shared_ptr<S3Client> s3Client; // (shared_ptr expected by some SDK functions)
        S3Transport transport; // moves object data between the buffers and the server
        std::string s3EndpointStr; // set after s3Client initialized
        static S3UploadStore s3SharedUploadStore; // singleton for shared uploads

        bool useS3SSE{false}; // for plain server-side encryption
        std::string s3SSECKey; // SSE-C encryption key
        std::string s3SSECKeyMD5; // SSE-C encryption key MD5 hash
        std::string s3SSEKMSKey; // SSE-KMS encryption key
        S3ChecksumAlgorithm s3ChecksumAlgorithm; // for x-amz-sdk-checksum-algorithm header

        template <typename R>
        void throwOnError(const Aws::Utils::Outcome<R, S3ErrorType>& outcome,
            const std::string& failMessage, const std::string& bucketName,
            const std::string& objectName="");
        template <typename REQUESTTYPE>
        void addServerSideEncryption(REQUESTTYPE& request);
        template <typename REQUESTTYPE>
        inline void addChecksumAlgorithm(REQUESTTYPE& request);

        void createBucket(std::string bucketName);
        void headBucket(std::string bucketName);
        void createBucketTagging(const std::string& bucketName);
        void deleteBucketTagging(const std::string& bucketName);
        void getBucketTagging(const std::string& bucketName);
        void deleteBucket(const std::string& bucketName);
        void putBucketAcl(std::string bucketName);
        void getBucketAcl(std::string bucketName);
        void getBucketVersioning(const std::string& bucketName);
        void putBucketVersioning(const std::string& bucketName, bool enable = true);
        void iterateObjectsRand();
        void uploadObjectSinglePart(std::string bucketName, std::string objectName);
        void uploadObjectMultiPart(std::string bucketName, std::string objectName);
        void uploadObjectMultiPartAsync(std::string bucketName, std::string objectName);
        void uploadObjectMultiPartShared(std::string bucketName, std::string objectName,
            uint64_t objectTotalSize);
        void uploadObjectMultiPartSharedAsync(std::string bucketName, std::string objectName,
            uint64_t objectTotalSize);
        void queryAndFinishMultipartUpload(std::string bucketName,
            std::string objectName, std::string uploadID, uint64_t expectedTotalSize);
        bool abortMultipartUpload(std::string bucketName, std::string objectName,
            std::string uploadID);
        void downloadObject(std::string bucketName, std::string objectName,
            const bool isRWMixedReader);
        void downloadObjectAsync(std::string bucketName, std::string objectName,
            const bool isRWMixedReader);
        void statObject(std::string bucketName, std::string objectName);
        void deleteObject(std::string bucketName, std::string objectName);
        void verifyListing(StringSet& expectedSet, StringList& receivedList,
            std::string bucketName, std::string listPrefix);
        void putObjectAcl(std::string bucketName, std::string objectName);
        void getObjectAcl(std::string bucketName, std::string objectName);
        void getObjectTags(const std::string& bucketName, const std::string& objectName);
        void putObjectTags(const std::string& bucketName, const std::string& objectName);
        void deleteObjectTags(const std::string& bucketName, const std::string& objectName);
        void getObjectLockConfiguration(const std::string& bucketName);
        void putObjectLockConfiguration(const std::string& bucketName, bool unset = false);
        std::string getRandObjectPrefix(size_t workerRank, size_t dirIdx, size_t fileIdx,
            const std::string& objectPrefix);
#endif // S3_SUPPORT
};


#endif /* MODES_S3_S3MODE_H_ */
