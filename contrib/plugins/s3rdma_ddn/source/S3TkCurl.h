// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

// S3TkCurl.h - Direct cURL wrapper for RDMA support
#ifndef DDN_S3TKCURL_H_
#define DDN_S3TKCURL_H_

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <cuobjclient.h>
#include <curl/curl.h>

class ProgArgs;
class S3ClientCurl;

/**
 * Async context for cURL operations (similar to aws-sdk-cpp's async pattern)
 */
class TkCurlContext
{
public:
    explicit TkCurlContext(S3ClientCurl *client);
    ~TkCurlContext();

    bool registerBufferGetToken(
            void *buf, size_t size, bool is_write);

    void failed(const char *errdescr = nullptr);
    bool addHeader(const char *value);
    bool finalize();
    bool isSuccess() const { return hndl && !err_descr && httpOk(); }
    void requestDone();
    bool httpOk() const;
    bool isRetryable() const; // only meaningful after a failed request
    void discardConnection(); // don't return handle (and its connection) to the cache
    const std::string errDescr() const;

    std::string getEtag() const { return etag; }

    CURL *hndl = nullptr;
    char errbuf[CURL_ERROR_SIZE] = {};

    std::string etag;
    std::string responseData;

    size_t rdmaCode = 0;
    size_t rdmaTransferred = 0;
    long httpCode = 0;
    CURLcode curlCode = CURLE_OK;
    bool performed = false; // true once curl_easy_perform() was called

private:
    std::string getRespMsg() const;

    S3ClientCurl *client = nullptr;
    struct curl_slist *headers = nullptr;
    void *buffer = nullptr;
    const char *err_descr = nullptr;
};

/**
 * Minimal wrapper for cURL API to enable RDMA
 */
class S3ClientCurl
{
public:
    S3ClientCurl(const ProgArgs* progArgs, size_t workerRank);
    ~S3ClientCurl();

    bool isValid() const { return valid; }

    // Prepare request for PUT with user buffer + RDMA
    bool putObjectRDMAPrepare(TkCurlContext &ctx,
        const std::string& bucket, const std::string& key,
        void* buffer, size_t size);

    // Prepare request for GET with user buffer + RDMA
    bool getObjectRDMAPrepare(TkCurlContext &ctx,
        const std::string& bucket, const std::string& key,
        void* buffer, size_t chunk_size, uint64_t offset);

    // Prepare request for UploadPart with user buffer + RDMA
    bool uploadPartRDMAPrepare(TkCurlContext &ctx,
        const std::string& bucket, const std::string& key,
        const std::string& uploadId, uint32_t partNumber,
        void* buffer, size_t size);

    // Run prepared requests
    // Return false on any error, including http error responce codes.
    bool runSync(TkCurlContext &ctx);

    bool registerBuffer(void *buf, size_t size);
    bool isRegisteredBuffer(void *buf, size_t size);
    void unregisterBuffers();
    std::string getToken(void *buf, size_t size, bool is_write);
    void putHandler(CURL *hndl);

private:
    size_t workerRank;
    bool valid = false;

    std::vector<CURL*> connCache;
    std::map<void*, size_t> registeredBuffers;

    std::string sigv4;
    std::string userpwd;
    std::string urlBase;

    long requestTimeoutMs = 0;
    long connectTimeoutMs = 0;

    bool prepareRequestCommon(TkCurlContext &ctx, std::string url);
    bool prepareRequestRdmaPut(TkCurlContext &ctx, std::string url);
    bool prepareRequestRdmaGet(TkCurlContext &ctx, std::string url);
    std::string createBaseUrl(const std::string& bucket, const std::string& key) const;

    CURL *getHandler();
    bool initHandler(CURL *hndl);

    static cuObjClient &cuClient();
};

#endif // DDN_S3TKCURL_H_
