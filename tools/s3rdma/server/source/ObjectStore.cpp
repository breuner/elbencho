// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "ObjectStore.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>
#include <sstream>
#include <sys/stat.h>

namespace fs = std::filesystem;

typedef ObjectStore::Result Result;

namespace
{
    const char* MPU_DIR_NAME = ".mpu"; // holds the multipart uploads in progress of a bucket

    // Split a path into its components, e.g. to look for ".." in an object key.
    bool hasDotDotComponent(const std::string& path)
    {
        std::istringstream stream(path);
        std::string component;

        while(std::getline(stream, component, '/') )
            if(component == "..")
                return true;

        return false;
    }
}

ObjectStore::ObjectStore(const std::string& rootDir) : root(rootDir)
{
    fs::create_directories(root);
}

/////////////////////////////////// Buckets ///////////////////////////////////

/**
 * Creating a bucket that already exists is not an error here (AWS behaves like that in the
 * default region as well), so that a re-run of a benchmark just works.
 */
Result ObjectStore::createBucket(const std::string& bucket)
{
    if(!isValidBucketName(bucket) )
        return Result::InvalidName;

    std::error_code errorCode;
    fs::create_directory(bucketPath(bucket), errorCode);

    return (errorCode && !fs::is_directory(bucketPath(bucket) ) ) ? Result::IOError : Result::OK;
}

bool ObjectStore::bucketExists(const std::string& bucket) const
{
    return isValidBucketName(bucket) && fs::is_directory(bucketPath(bucket) );
}

/**
 * A bucket counts as empty when it contains no files (S3 has no dirs, so leftover dirs of nested
 * keys are ignored). An upload in progress counts as content, as it does on AWS.
 */
Result ObjectStore::deleteBucket(const std::string& bucket)
{
    if(!bucketExists(bucket) )
        return Result::NoSuchBucket;

    for(const fs::directory_entry& entry : fs::recursive_directory_iterator(bucketPath(bucket) ) )
        if(!entry.is_directory() )
            return Result::BucketNotEmpty;

    std::error_code errorCode;
    fs::remove_all(bucketPath(bucket), errorCode);

    return errorCode ? Result::IOError : Result::OK;
}

std::vector<std::string> ObjectStore::listBuckets() const
{
    std::vector<std::string> buckets;

    for(const fs::directory_entry& entry : fs::directory_iterator(root) )
        if(entry.is_directory() )
            buckets.push_back(entry.path().filename().string() );

    std::sort(buckets.begin(), buckets.end() );

    return buckets;
}

/**
 * All objects of a bucket that start with prefix, sorted by key.
 */
Result ObjectStore::listObjects(const std::string& bucket, const std::string& prefix,
    std::vector<ObjectInfo>& outObjects) const
{
    if(!bucketExists(bucket) )
        return Result::NoSuchBucket;

    const fs::path bucketDir = bucketPath(bucket);

    for(auto iter = fs::recursive_directory_iterator(bucketDir);
        iter != fs::recursive_directory_iterator(); iter++)
    {
        if(iter.depth() == 0 && iter->path().filename() == MPU_DIR_NAME)
        {
            iter.disable_recursion_pending();
            continue;
        }

        if(!iter->is_regular_file() )
            continue;

        const std::string key = iter->path().lexically_relative(bucketDir).generic_string();

        if(key.compare(0, prefix.size(), prefix) != 0)
            continue;

        ObjectInfo info;
        if(statFile(iter->path(), key, info) == Result::OK)
            outObjects.push_back(std::move(info) );
    }

    std::sort(outObjects.begin(), outObjects.end(),
        [](const ObjectInfo& a, const ObjectInfo& b) { return a.key < b.key; } );

    return Result::OK;
}

/////////////////////////////////// Objects ///////////////////////////////////

Result ObjectStore::statObject(const std::string& bucket, const std::string& key,
    ObjectInfo& outInfo) const
{
    if(!bucketExists(bucket) )
        return Result::NoSuchBucket;

    if(!isValidKey(key) )
        return Result::InvalidName;

    return statFile(objectPath(bucket, key), key, outInfo);
}

Result ObjectStore::deleteObject(const std::string& bucket, const std::string& key)
{
    if(!bucketExists(bucket) )
        return Result::NoSuchBucket;

    if(!isValidKey(key) )
        return Result::InvalidName;

    std::error_code errorCode;
    fs::remove(objectPath(bucket, key), errorCode); // (deleting a missing key is fine in S3)

    return Result::OK;
}

/**
 * Open (and truncate) the object file for writing. Parent dirs of nested keys get created.
 */
Result ObjectStore::openObjectForWrite(const std::string& bucket, const std::string& key,
    std::ofstream& outStream)
{
    if(!bucketExists(bucket) )
        return Result::NoSuchBucket;

    if(!isValidKey(key) )
        return Result::InvalidName;

    return openForWrite(objectPath(bucket, key), outStream);
}

Result ObjectStore::openObjectForRead(const std::string& bucket, const std::string& key,
    std::ifstream& outStream, ObjectInfo& outInfo) const
{
    const Result statRes = statObject(bucket, key, outInfo);
    if(statRes != Result::OK)
        return statRes;

    outStream.open(objectPath(bucket, key), std::ios::binary);

    return outStream ? Result::OK : Result::IOError;
}

/////////////////////////////// Multipart uploads ///////////////////////////////

Result ObjectStore::createMultipartUpload(const std::string& bucket, const std::string& key,
    std::string& outUploadID)
{
    if(!bucketExists(bucket) )
        return Result::NoSuchBucket;

    if(!isValidKey(key) )
        return Result::InvalidName;

    // random 32 hex chars as upload id
    static thread_local std::mt19937_64 randGen{std::random_device{}() };
    char idBuf[33];
    snprintf(idBuf, sizeof(idBuf), "%016llx%016llx", (unsigned long long)randGen(),
        (unsigned long long)randGen() );
    outUploadID = idBuf;

    const fs::path uploadDir = uploadPath(bucket, outUploadID);

    std::error_code errorCode;
    fs::create_directories(uploadDir / "parts", errorCode);
    if(errorCode)
        return Result::IOError;

    std::ofstream keyFile(uploadDir / "key", std::ios::binary);
    keyFile << key;

    return keyFile ? Result::OK : Result::IOError;
}

Result ObjectStore::openPartForWrite(const std::string& bucket, const std::string& uploadID,
    unsigned partNumber, std::ofstream& outStream)
{
    if(!bucketExists(bucket) )
        return Result::NoSuchBucket;

    if(!isValidUploadID(uploadID) || !fs::is_directory(uploadPath(bucket, uploadID) ) )
        return Result::NoSuchUpload;

    return openForWrite(partPath(bucket, uploadID, partNumber), outStream);
}

Result ObjectStore::statPart(const std::string& bucket, const std::string& uploadID,
    unsigned partNumber, ObjectInfo& outInfo) const
{
    if(!isValidUploadID(uploadID) )
        return Result::NoSuchUpload;

    return statFile(partPath(bucket, uploadID, partNumber), std::to_string(partNumber), outInfo);
}

/**
 * Concatenate all uploaded parts in ascending part number order into the object and remove the
 * upload. The part list that the client sends is not consulted, all uploaded parts are used.
 */
Result ObjectStore::completeMultipartUpload(const std::string& bucket, const std::string& uploadID,
    ObjectInfo& outInfo)
{
    if(!bucketExists(bucket) )
        return Result::NoSuchBucket;

    const fs::path uploadDir = uploadPath(bucket, uploadID);

    if(!isValidUploadID(uploadID) || !fs::is_directory(uploadDir) )
        return Result::NoSuchUpload;

    std::string key;

    { // (closed before the upload dir is removed: NFS keeps unlinked open files as .nfs* entries)
        std::ifstream keyFile(uploadDir / "key", std::ios::binary);
        std::getline(keyFile, key);
    }

    std::vector<fs::path> partPaths;
    for(const fs::directory_entry& entry : fs::directory_iterator(uploadDir / "parts") )
        partPaths.push_back(entry.path() );

    std::sort(partPaths.begin(), partPaths.end() ); // (zero-padded names, so lexical is numeric)

    std::ofstream objectStream;
    const Result openRes = openObjectForWrite(bucket, key, objectStream);
    if(openRes != Result::OK)
        return openRes;

    for(const fs::path& partPath : partPaths)
    {
        std::ifstream partStream(partPath, std::ios::binary);
        objectStream << partStream.rdbuf();

        if(!objectStream || (partStream.fail() && !partStream.eof() ) )
            return Result::IOError;
    }

    objectStream.close();

    std::error_code errorCode;
    fs::remove_all(uploadDir, errorCode);

    return statFile(objectPath(bucket, key), key, outInfo);
}

Result ObjectStore::abortMultipartUpload(const std::string& bucket, const std::string& uploadID)
{
    if(!bucketExists(bucket) )
        return Result::NoSuchBucket;

    if(!isValidUploadID(uploadID) || !fs::is_directory(uploadPath(bucket, uploadID) ) )
        return Result::NoSuchUpload;

    std::error_code errorCode;
    fs::remove_all(uploadPath(bucket, uploadID), errorCode);

    return errorCode ? Result::IOError : Result::OK;
}

//////////////////////////////////// Helpers ////////////////////////////////////

fs::path ObjectStore::bucketPath(const std::string& bucket) const
{
    return root / bucket;
}

fs::path ObjectStore::objectPath(const std::string& bucket, const std::string& key) const
{
    return root / bucket / key;
}

fs::path ObjectStore::uploadPath(const std::string& bucket, const std::string& uploadID) const
{
    return root / bucket / MPU_DIR_NAME / uploadID;
}

fs::path ObjectStore::partPath(const std::string& bucket, const std::string& uploadID,
    unsigned partNumber) const
{
    char nameBuf[16];
    snprintf(nameBuf, sizeof(nameBuf), "%05u", partNumber);

    return uploadPath(bucket, uploadID) / "parts" / nameBuf;
}

bool ObjectStore::isValidBucketName(const std::string& bucket)
{
    return !bucket.empty() && bucket != "." && bucket != ".." && bucket != MPU_DIR_NAME &&
        bucket.find('/') == std::string::npos;
}

/**
 * Keys map to file paths, so they must not escape the bucket dir or clash with the uploads dir.
 */
bool ObjectStore::isValidKey(const std::string& key)
{
    return !key.empty() && key.back() != '/' && key.compare(0, strlen(MPU_DIR_NAME), MPU_DIR_NAME) &&
        !hasDotDotComponent(key);
}

bool ObjectStore::isValidUploadID(const std::string& uploadID)
{
    return !uploadID.empty() && uploadID.find('/') == std::string::npos &&
        uploadID != "." && uploadID != "..";
}

Result ObjectStore::statFile(const fs::path& path, const std::string& key, ObjectInfo& outInfo)
{
    struct stat statBuf;

    if(stat(path.c_str(), &statBuf) || !S_ISREG(statBuf.st_mode) )
        return Result::NoSuchKey;

    char etagBuf[64];
    snprintf(etagBuf, sizeof(etagBuf), "\"%llx-%llx-%llx\"", (unsigned long long)statBuf.st_size,
        (unsigned long long)statBuf.st_mtime, (unsigned long long)statBuf.st_ino);

    outInfo.key = key;
    outInfo.size = statBuf.st_size;
    outInfo.mtime = statBuf.st_mtime;
    outInfo.etag = etagBuf; // (opaque; nothing here verifies ETags)

    return Result::OK;
}

Result ObjectStore::openForWrite(const fs::path& path, std::ofstream& outStream)
{
    std::error_code errorCode;
    fs::create_directories(path.parent_path(), errorCode);

    outStream.open(path, std::ios::binary | std::ios::trunc);

    return outStream ? Result::OK : Result::IOError;
}
