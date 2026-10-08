// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef OBJECTSTORE_H_
#define OBJECTSTORE_H_

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

struct ObjectInfo
{
    std::string key;
    uint64_t size = 0;
    time_t mtime = 0;
    std::string etag; // including the double quotes
};

/**
 * Stores buckets as directories and objects as plain files below a root dir:
 *   <root>/<bucket>/<key>
 *   <root>/<bucket>/.mpu/<uploadId>/key        (target key of a multipart upload in progress)
 *   <root>/<bucket>/.mpu/<uploadId>/parts/NNNNN (its uploaded parts)
 */
class ObjectStore
{
    public:
        enum class Result
        {
            OK,
            NoSuchBucket,
            NoSuchKey,
            NoSuchUpload,
            BucketNotEmpty,
            InvalidName,
            IOError,
        };

        explicit ObjectStore(const std::string& rootDir);

        Result createBucket(const std::string& bucket);
        bool bucketExists(const std::string& bucket) const;
        Result deleteBucket(const std::string& bucket);
        std::vector<std::string> listBuckets() const;
        Result listObjects(const std::string& bucket, const std::string& prefix,
            std::vector<ObjectInfo>& outObjects) const;

        Result statObject(const std::string& bucket, const std::string& key,
            ObjectInfo& outInfo) const;
        Result deleteObject(const std::string& bucket, const std::string& key);
        Result openObjectForWrite(const std::string& bucket, const std::string& key,
            std::ofstream& outStream);
        Result openObjectForRead(const std::string& bucket, const std::string& key,
            std::ifstream& outStream, ObjectInfo& outInfo) const;

        Result createMultipartUpload(const std::string& bucket, const std::string& key,
            std::string& outUploadID);
        Result openPartForWrite(const std::string& bucket, const std::string& uploadID,
            unsigned partNumber, std::ofstream& outStream);
        Result statPart(const std::string& bucket, const std::string& uploadID,
            unsigned partNumber, ObjectInfo& outInfo) const;
        Result completeMultipartUpload(const std::string& bucket, const std::string& uploadID,
            ObjectInfo& outInfo);
        Result abortMultipartUpload(const std::string& bucket, const std::string& uploadID);

    private:
        std::filesystem::path root;

        std::filesystem::path bucketPath(const std::string& bucket) const;
        std::filesystem::path objectPath(const std::string& bucket, const std::string& key) const;
        std::filesystem::path uploadPath(const std::string& bucket,
            const std::string& uploadID) const;
        std::filesystem::path partPath(const std::string& bucket, const std::string& uploadID,
            unsigned partNumber) const;

        static bool isValidBucketName(const std::string& bucket);
        static bool isValidKey(const std::string& key);
        static bool isValidUploadID(const std::string& uploadID);
        static Result statFile(const std::filesystem::path& path, const std::string& key,
            ObjectInfo& outInfo);
        static Result openForWrite(const std::filesystem::path& path, std::ofstream& outStream);
};

#endif // OBJECTSTORE_H_
