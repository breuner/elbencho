// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef RDMAS3CLIENT_H_
#define RDMAS3CLIENT_H_

#include <cstdint>
#include <memory>
#include <string>

#include <cuobjclient.h>

namespace httplib
{
    class Client;
}

/**
 * The client side of the S3-over-RDMA protocol of NVIDIA cuObject, as the elbencho plugins speak
 * it: object data moves through a cuObjClient token, the HTTP request is the body-less control
 * message with the x-amz-rdma-* headers. Which transport is behind the cuObjClient depends on
 * the library this is linked against: NVIDIA's libcuobjclient (DC transport) or the RC shim in
 * ../cuobjclient-shim. Requests are not signed, so this only works with servers that do not check
 * signatures, like tools/s3rdma/server.
 */
class RdmaS3Client
{
    public:
        RdmaS3Client(const std::string& endpoint);
        ~RdmaS3Client();

        bool isConnected() const { return connected; }

        bool registerBuffer(char* buf, size_t len);
        bool createBucket(const std::string& bucket);
        bool deleteBucket(const std::string& bucket);
        bool deleteObject(const std::string& bucket, const std::string& key);
        bool putObject(const std::string& bucket, const std::string& key, char* buf, size_t len);
        ssize_t getObject(const std::string& bucket, const std::string& key, char* buf,
            size_t len, uint64_t offset);

    private:
        CUObjIOOps ops = {}; // empty: the token flow does not use the callbacks
        std::unique_ptr<cuObjClient> rdma;
        std::unique_ptr<httplib::Client> http;
        bool connected = false;

        bool simpleRequest(const char* method, const std::string& path);
};

#endif // RDMAS3CLIENT_H_
