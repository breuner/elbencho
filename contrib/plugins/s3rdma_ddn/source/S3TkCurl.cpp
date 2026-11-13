// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

// S3TkCurl.cpp - Implementation
#include <chrono>

#include "Common.h"
#include "ProgArgs.h"
#include "modes/s3/toolkits/S3CredentialStore.h"
#include "S3TkCurl.h"
#include "S3RdmaDdnPlugin.h"

#include <cassert>
#include <charconv>
#include <mutex>
#include <string>

#include <aws/core/auth/AWSCredentialsProviderChain.h>

#define ECE(expr) if (CURLcode ec = (expr); ec != CURLE_OK) return false
#define ECC(ctx, expr) \
do {                                            \
    if (CURLcode ec = (expr); ec != CURLE_OK) { \
        (ctx).failed();                         \
        return false;                           \
    }                                           \
} while(0)

static const size_t maxResponseSize = 64 * 1024;

static std::mutex cuClientMutex;
static std::mutex connCacheMutex;

bool extract_hdr_val(std::string_view &hdr, std::string_view name)
{
    if (hdr.find(name) != 0)
        return false;

    hdr.remove_prefix(name.size());

    static constexpr std::string_view spaces = " \t\r\n";
    hdr.remove_prefix(hdr.find_first_not_of(spaces));

    auto last_valid = hdr.find_last_not_of(spaces);
    if (last_valid != hdr.npos)
        hdr.remove_suffix(hdr.size() - last_valid - 1);

    return true;
}

void parse_int_hdr(std::string_view hdr, std::string_view name, size_t &ret)
{
    if (!extract_hdr_val(hdr, name))
        return;

    std::from_chars(hdr.data(), hdr.data() + hdr.size(), ret);
}

void parse_etag_hdr(std::string_view hdr, std::string &etag)
{
    static constexpr std::string_view etag_name = "ETag:";
    if (!extract_hdr_val(hdr, etag_name))
        return;

    if (hdr.front() == '"' && hdr.back() == '"')
        hdr = hdr.substr(1, hdr.size() - 2);

    etag = hdr;
}

size_t header_callback(char* buf, size_t size, size_t nitems, void* userdata)
{
    std::string_view hdr(buf, size * nitems);
    auto ctx = static_cast<TkCurlContext *>(userdata);

    parse_int_hdr(hdr, "x-amz-rdma-bytes-transferred:", ctx->rdmaTransferred);
    parse_int_hdr(hdr, "x-amz-rdma-reply:", ctx->rdmaCode);
    parse_etag_hdr(hdr, ctx->etag);

    return size * nitems;
}

/* body recv. */
extern size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto ctx = static_cast<TkCurlContext *>(userdata);

    size_t bytes = size * nmemb;
    if (ctx->responseData.size() < maxResponseSize)
        ctx->responseData.append(ptr, bytes);

    return bytes;
}

TkCurlContext::TkCurlContext(S3ClientCurl *client)
    : client(client) {}

TkCurlContext::~TkCurlContext()
{
    if (headers)
        curl_slist_free_all(headers);

    if (hndl)
        client->putHandler(hndl);
}

bool TkCurlContext::registerBufferGetToken(
        void *buf, size_t size, bool is_write)
{
    if (!client->isRegisteredBuffer(buf, size))
    {
        failed("Buffer not registered");
        return false;
    }

    buffer = buf;

    auto token = client->getToken(buf, size, is_write);
    if (token.empty())
    {
        failed("Can't get RDMA token");
        return false;
    }

    return addHeader(("x-amz-rdma-token: " + token).c_str());
}

void TkCurlContext::failed(const char *errdescr)
{
    if (!err_descr)
        err_descr = errdescr;

    curl_easy_cleanup(hndl);
    hndl = nullptr;
}

void TkCurlContext::discardConnection()
{
    if (hndl)
        curl_easy_cleanup(hndl);

    hndl = nullptr;
}

bool TkCurlContext::addHeader(const char *value)
{
    auto newlist = curl_slist_append(headers, value);
    if (!newlist)
    {
        failed("Can't append header");
        return false;
    }

    headers = newlist;
    return true;
}

bool TkCurlContext::finalize()
{
    ECC(*this, curl_easy_setopt(hndl, CURLOPT_HTTPHEADER, headers));
    return true;
}

void TkCurlContext::requestDone()
{
    httpCode = 0;
    curl_easy_getinfo(hndl, CURLINFO_RESPONSE_CODE, &httpCode);
}

static bool httpRespOk(long code)
{
    return code == 200 || code == 204 || code == 206;
}

bool TkCurlContext::httpOk() const
{
    return httpRespOk(httpCode) && (rdmaCode == 0 || httpRespOk(rdmaCode));
}

static bool httpRespRetryable(long code)
{
    return code == 429 || code == 500 || code == 502 || code == 503 || code == 504;
}

bool TkCurlContext::isRetryable() const
{
    // transport-level failure of the request (timeout, connect failure, conn reset, ...)
    if (performed && curlCode != CURLE_OK)
        return true;

    // local setup failures (no handle, buffer not registered, no RDMA token) are not transient
    if (!performed)
        return false;

    return httpRespRetryable(httpCode) || httpRespRetryable((long)rdmaCode);
}

const std::string TkCurlContext::errDescr() const
{
    if (errbuf[0])
        return errbuf;

    if (err_descr)
        return err_descr;

    if (!httpOk())
    {
        std::string ret;
        if (httpCode == 501)
            ret = "RDMA not implemented; ";
        ret += "HTTP code: " + std::to_string(httpCode);
        if (rdmaCode)
            ret += ", RDMA code: " + std::to_string(rdmaCode);
        auto msg = getRespMsg();
        if (!msg.empty())
            ret += ", Message: " + msg;

        return ret;
    }

    if (!isSuccess())
        return "Unknown error";

    return {};
}

std::string TkCurlContext::getRespMsg() const
{
    const static std::string key = "<Message>";

    auto start = responseData.find(key);
    if (start == responseData.npos)
        return "";

    start += key.size();
    auto end = responseData.find("</", start);
    if (end == responseData.npos)
        return "";

    return responseData.substr(start, end - start);
}


S3ClientCurl::S3ClientCurl(const ProgArgs* progArgs, size_t workerRank) :
    workerRank(workerRank)
{
    // libCurl supports multiple init/cleanups. If already inited, does nothing.
    if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK)
    {
        ERRLOGGER(Log_NORMAL, "Can't init libcurl." << std::endl);
        return;
    }

    {
        const std::lock_guard<std::mutex> lock(cuClientMutex);
        if (!cuClient().isConnected())
        {
            ERRLOGGER(Log_NORMAL, "CuClient not connected." << std::endl);
            return;
        }
    }

    auto &s3Args = progArgs->getS3Args();

    Aws::String region = s3Args.getS3Region();
    if (region.empty())
        region = "us-east-1";
    sigv4 = "aws:amz:" + region + ":s3";

    std::shared_ptr<Aws::Auth::AWSCredentialsProvider> cred_provider;
    if(!s3Args.getS3AccessKey().empty() || !s3Args.getS3AccessSecret().empty())
    { // Single credential mode
        cred_provider = std::make_shared<Aws::Auth::SimpleAWSCredentialsProvider>(
                s3Args.getS3AccessKey(), s3Args.getS3AccessSecret(),
                s3Args.getS3SessionToken() );
    }
    else
    if(!s3Args.getS3CredentialsFile().empty() || !s3Args.getS3CredentialsList().empty() )
    { // Multi-credential mode (round-robin)
        cred_provider = S3CredentialStore::getInstance().getCredential(workerRank);
    }
    else
    { // Fallback: use default credential provider chain
        cred_provider = std::make_shared<Aws::Auth::DefaultAWSCredentialsProviderChain>();
    }

    auto cred = cred_provider->GetAWSCredentials();
    userpwd = cred.GetAWSAccessKeyId() + ":" + cred.GetAWSSecretKey();

    if (s3Args.getS3EndpointsVec().empty())
    {
        ERRLOGGER(Log_NORMAL, "Endpoints aren't explicitly defined." << std::endl);
        return;
    }

    const S3RdmaDdnPlugin& plugin = S3RdmaDdnPlugin::getInstance();
    requestTimeoutMs = (long)plugin.getRequestTimeoutMS();
    connectTimeoutMs = (long)plugin.getConnectTimeoutMS();

    auto &endpointsVec = s3Args.getS3EndpointsVec();
    auto numEndpoints = endpointsVec.size();
    urlBase = endpointsVec[workerRank % numEndpoints];

    valid = true;
}

S3ClientCurl::~S3ClientCurl()
{
    unregisterBuffers();

    for (auto hndl : connCache)
        curl_easy_cleanup(hndl);

    curl_global_cleanup();
}

bool S3ClientCurl::putObjectRDMAPrepare(
    TkCurlContext &ctx,
    const std::string& bucket, const std::string& key,
    void* buffer, size_t size)
{
    auto url = createBaseUrl(bucket, key);
    if (!prepareRequestRdmaPut(ctx, std::move(url)))
        return false;

    if (!ctx.registerBufferGetToken(buffer, size, true))
        return false;

    return ctx.finalize();
}

bool S3ClientCurl::getObjectRDMAPrepare(
    TkCurlContext &ctx,
    const std::string& bucket, const std::string& key,
    void* buffer, size_t size, uint64_t offset)
{
    auto url = createBaseUrl(bucket, key);
    if (!prepareRequestRdmaGet(ctx, std::move(url)))
        return false;

    if (size > 0 || offset > 0)
    {
        std::string header = "Range: bytes=" + std::to_string(offset) + "-" + std::to_string(offset + size - 1);
        if (!ctx.addHeader(header.c_str()))
            return false;
    }

    if (!ctx.registerBufferGetToken(buffer, size, false))
        return false;

    return ctx.finalize();
}

bool S3ClientCurl::uploadPartRDMAPrepare(TkCurlContext &ctx,
    const std::string& bucket, const std::string& key,
    const std::string& uploadId, uint32_t partNumber,
    void* buffer, size_t size)
{
    auto url = createBaseUrl(bucket, key) + "?partNumber=" + std::to_string(partNumber) + "&uploadId=" + uploadId;
    if (!prepareRequestRdmaPut(ctx, std::move(url)))
        return false;

    if (!ctx.registerBufferGetToken(buffer, size, true))
        return false;

    return ctx.finalize();
}

bool S3ClientCurl::runSync(TkCurlContext &ctx)
{
    auto res = curl_easy_perform(ctx.hndl);
    ctx.performed = true;
    ctx.curlCode = res;
    ctx.requestDone();

    if (res != CURLE_OK)
        ctx.failed("Failed sync request perform");

    return res == CURLE_OK && ctx.httpOk();
}

bool S3ClientCurl::prepareRequestCommon(TkCurlContext &ctx, std::string url)
{
    assert(ctx.hndl == nullptr);

    auto hndl = getHandler();
    if (!hndl)
    {
        ctx.failed("Failed to create Curl handle");
        return false;
    }

    ctx.hndl = hndl;
    ECC(ctx, curl_easy_setopt(hndl, CURLOPT_PRIVATE, &ctx));
    ECC(ctx, curl_easy_setopt(hndl, CURLOPT_ERRORBUFFER, ctx.errbuf));
    ECC(ctx, curl_easy_setopt(hndl, CURLOPT_URL, url.c_str()));

    return true;
}

bool S3ClientCurl::prepareRequestRdmaPut(TkCurlContext &ctx, std::string url)
{
    if (!prepareRequestCommon(ctx, std::move(url)))
        return false;

    ECC(ctx, curl_easy_setopt(ctx.hndl, CURLOPT_HEADERFUNCTION, header_callback));
    ECC(ctx, curl_easy_setopt(ctx.hndl, CURLOPT_HEADERDATA, &ctx));
    ECC(ctx, curl_easy_setopt(ctx.hndl, CURLOPT_WRITEFUNCTION, write_callback));
    ECC(ctx, curl_easy_setopt(ctx.hndl, CURLOPT_WRITEDATA, &ctx));
    ECC(ctx, curl_easy_setopt(ctx.hndl, CURLOPT_UPLOAD, 1L));
    ECC(ctx, curl_easy_setopt(ctx.hndl, CURLOPT_INFILESIZE_LARGE, 0L));

    if (!ctx.addHeader("Expect:"))
        return false;
    if (!ctx.addHeader("x-amz-content-sha256: UNSIGNED-PAYLOAD"))
        return false;

    return true;
}

bool S3ClientCurl::prepareRequestRdmaGet(TkCurlContext &ctx, std::string url)
{
    if (!prepareRequestCommon(ctx, std::move(url)))
        return false;

    ECC(ctx, curl_easy_setopt(ctx.hndl, CURLOPT_HEADERFUNCTION, header_callback));
    ECC(ctx, curl_easy_setopt(ctx.hndl, CURLOPT_HEADERDATA, &ctx));
    ECC(ctx, curl_easy_setopt(ctx.hndl, CURLOPT_WRITEFUNCTION, write_callback));
    ECC(ctx, curl_easy_setopt(ctx.hndl, CURLOPT_WRITEDATA, &ctx));

    return true;
}

std::string S3ClientCurl::createBaseUrl(const std::string& bucket, const std::string& key) const
{
    return urlBase + "/" + bucket + "/" + key;
}

bool S3ClientCurl::registerBuffer(void *buf, size_t size)
{
    auto it = registeredBuffers.find(buf);
    // double registration, ignore
    if (it != registeredBuffers.end())
        return it->second == size;

    {
        const std::lock_guard<std::mutex> lock(cuClientMutex);
        if (cuClient().cuMemObjGetDescriptor(buf, size) != CU_OBJ_SUCCESS)
            return false;
    }

    registeredBuffers.emplace(buf, size);
    return true;
}

bool S3ClientCurl::isRegisteredBuffer(void *buf, size_t size)
{
    auto it = registeredBuffers.find(buf);
    return it != registeredBuffers.end() && it->second == size;
}

void S3ClientCurl::unregisterBuffers()
{
    for (auto &buf: registeredBuffers)
    {
        const std::lock_guard<std::mutex> lock(cuClientMutex);
        cuClient().cuMemObjPutDescriptor(buf.first);
    }

    registeredBuffers.clear();
}

std::string S3ClientCurl::getToken(void *buf, size_t size, bool is_write)
{
    std::string res;

    char *token = nullptr;
    cuObjErr_t err;

    {
        const std::lock_guard<std::mutex> lock(cuClientMutex);
        err = cuClient().cuMemObjGetRDMAToken(buf, size, 0, is_write ? CUOBJ_PUT : CUOBJ_GET, &token);
    }

    if (err != CU_OBJ_SUCCESS)
        return "";

    res = token;

    {
        const std::lock_guard<std::mutex> lock(cuClientMutex);
        cuClient().cuMemObjPutRDMAToken(token);
    }

    return res;
}

CURL *S3ClientCurl::getHandler()
{
    bool empty = false;
    {
        const std::lock_guard<std::mutex> lock(connCacheMutex);
        empty = connCache.empty();
    }

    if (empty)
    {
        auto hndl = curl_easy_init();
        if (!hndl)
            return nullptr;

        if (!initHandler(hndl))
        {
            curl_easy_cleanup(hndl);
            return nullptr;
        }

        return hndl;
    }

    const std::lock_guard<std::mutex> lock(connCacheMutex);
    auto res = connCache.back();
    connCache.pop_back();
    return res;
}

void S3ClientCurl::putHandler(CURL *hndl)
{
    assert(hndl);

    curl_easy_setopt(hndl, CURLOPT_COOKIEFILE, NULL);
    curl_easy_reset(hndl);

    if (!initHandler(hndl))
    {
        curl_easy_cleanup(hndl);
        return;
    }

    const std::lock_guard<std::mutex> lock(connCacheMutex);
    connCache.push_back(hndl);
}

bool S3ClientCurl::initHandler(CURL *hndl)
{

// Currently elbencho doesn't explicitly set http version, use aws-c defaults.
#if LIBCURL_VERSION_NUM >= 0x073E00 // 7.62.0
    long http_version = CURL_HTTP_VERSION_2TLS;
#else
    long http_version = CURL_HTTP_VERSION_1_1;
#endif

    // Network settings
    ECE(curl_easy_setopt(hndl, CURLOPT_NOSIGNAL, 1L));
    ECE(curl_easy_setopt(hndl, CURLOPT_TCP_NODELAY, 1L));
    ECE(curl_easy_setopt(hndl, CURLOPT_TIMEOUT_MS, requestTimeoutMs));
    ECE(curl_easy_setopt(hndl, CURLOPT_CONNECTTIMEOUT_MS, connectTimeoutMs));
    ECE(curl_easy_setopt(hndl, CURLOPT_TCP_KEEPALIVE, 1L));
    ECE(curl_easy_setopt(hndl, CURLOPT_HTTP_VERSION, http_version));
    // Support self-signed cert
    ECE(curl_easy_setopt(hndl, CURLOPT_SSL_VERIFYPEER, 0L));
    ECE(curl_easy_setopt(hndl, CURLOPT_SSL_VERIFYHOST, 0L));

    // Common s3 settings
    ECE(curl_easy_setopt(hndl, CURLOPT_AWS_SIGV4, sigv4.c_str()));
    ECE(curl_easy_setopt(hndl, CURLOPT_USERPWD, userpwd.c_str()));

    return true;
}

cuObjClient &S3ClientCurl::cuClient()
{
    static CUObjOps_t opts{};
    static cuObjClient c(opts, CUOBJ_PROTO_RDMA_DC_V1);

    return c;
}
