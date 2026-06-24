// SPDX-FileCopyrightText: 2020-2025 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "toolkits/S3RdmaTk.h"

#if defined(S3_SUPPORT) && defined(CUOBJ_SUPPORT)

#include <exception>
#include <map>
#include <sstream>

#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentials.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/auth/AWSCredentialsProviderChain.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/http/HttpClient.h>
#include <aws/core/http/HttpClientFactory.h>
#include <aws/core/http/HttpRequest.h>
#include <aws/core/http/HttpResponse.h>
#include <aws/core/http/URI.h>
#include <aws/core/utils/DateTime.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/stream/ResponseStream.h>

#include "Logger.h"
#include "ProgArgs.h"
#include "toolkits/S3CredentialStore.h"

using namespace S3Rdma;

namespace
{
	// S3 ETag values are returned wrapped in double quotes; strip them.
	std::string stripQuotes(const std::string& s)
	{
		size_t b = 0, e = s.size();
		if(e >= 2 && s.front() == '"' && s.back() == '"')
		{
			b = 1;
			e = s.size() - 1;
		}
		return s.substr(b, e - b);
	}
} // anonymous namespace

// S3RdmaControlPlane::Impl holds the AWS SDK low-level HTTP/signing primitives;
// the protocol logic in rdmaPut/rdmaGet stays SDK-agnostic.
struct S3RdmaControlPlane::Impl
{
	Aws::String scheme; // "http" / "https"
	Aws::String host; // endpoint host (no port; GetAuthority strips it)
	unsigned port = 0; // explicit port (0 => scheme default)
	Aws::String region;
	bool virtualAddressing = false;
	std::shared_ptr<Aws::Http::HttpClient> http;
	Aws::String accessKey;
	Aws::String secretKey;
	Aws::String sessionToken;

	// SigV4-sign the request with payload hash "UNSIGNED-PAYLOAD". We sign manually
	// (rather than via the SDK's AWSAuthV4Signer) because the S3 RDMA server only
	// skips content-sha256 validation when the header is exactly UNSIGNED-PAYLOAD,
	// and the data here travels out-of-band over RDMA. All non-signed headers
	// (host, x-amz-rdma-token, content-*) must already be set before calling this.
	void signV4(Aws::Http::HttpRequest& req) const
	{
		using Aws::Utils::HashingUtils;
		using Aws::Utils::StringUtils;
		const Aws::String service = "s3";
		const Aws::String payloadHash = UNSIGNED_PAYLOAD;

		Aws::Utils::DateTime now = Aws::Utils::DateTime::Now();
		const Aws::String amzDate = now.ToGmtString("%Y%m%dT%H%M%SZ");
		const Aws::String dateStamp = now.ToGmtString("%Y%m%d");

		// Host header (with port) must be signed and match what is sent.
		Aws::String hostHeader = req.GetUri().GetAuthority();
		const unsigned p = req.GetUri().GetPort();
		if(p != 0 && p != 80 && p != 443)
			hostHeader += ":" + std::to_string(p);

		req.SetHeaderValue("host", hostHeader);
		req.SetHeaderValue("x-amz-date", amzDate);
		req.SetHeaderValue("x-amz-content-sha256", payloadHash);
		if(!sessionToken.empty() )
			req.SetHeaderValue("x-amz-security-token", sessionToken);

		// Canonical headers: lowercase name, trimmed value, sorted by name.
		std::map<Aws::String, Aws::String> hdrs;
		for(const auto& h : req.GetHeaders() )
			hdrs[StringUtils::ToLower(h.first.c_str() )] = StringUtils::Trim(h.second.c_str() );

		Aws::String canonicalHeaders, signedHeaders;
		for(const auto& kv : hdrs)
		{
			canonicalHeaders += kv.first + ":" + kv.second + "\n";
			if(!signedHeaders.empty() )
				signedHeaders += ";";
			signedHeaders += kv.first;
		}

		// Canonical query string: sorted, RFC3986-encoded key=value.
		const auto qp = req.GetUri().GetQueryStringParameters();
		std::map<Aws::String, Aws::String> q(qp.begin(), qp.end() );
		Aws::String canonicalQuery;
		for(const auto& kv : q)
		{
			if(!canonicalQuery.empty() )
				canonicalQuery += "&";
			canonicalQuery += StringUtils::URLEncode(kv.first.c_str() ) + "=" +
				StringUtils::URLEncode(kv.second.c_str() );
		}

		Aws::String canonicalUri = req.GetUri().GetURLEncodedPathRFC3986();
		if(canonicalUri.empty() )
			canonicalUri = "/";

		const Aws::String method =
			Aws::Http::HttpMethodMapper::GetNameForHttpMethod(req.GetMethod() );
		const Aws::String canonicalRequest = method + "\n" + canonicalUri + "\n" +
			canonicalQuery + "\n" + canonicalHeaders + "\n" + signedHeaders + "\n" + payloadHash;

		const Aws::String scope = dateStamp + "/" + region + "/" + service + "/aws4_request";
		const Aws::String crHash =
			HashingUtils::HexEncode(HashingUtils::CalculateSHA256(canonicalRequest) );
		const Aws::String stringToSign =
			"AWS4-HMAC-SHA256\n" + amzDate + "\n" + scope + "\n" + crHash;

		auto hmac = [](const Aws::Utils::ByteBuffer& key, const Aws::String& data)
		{
			return HashingUtils::CalculateSHA256HMAC(
				Aws::Utils::ByteBuffer(reinterpret_cast<const unsigned char*>(data.c_str() ),
					data.size() ),
				key);
		};

		const Aws::String kSecretStr = "AWS4" + secretKey;
		Aws::Utils::ByteBuffer kSecret(
			reinterpret_cast<const unsigned char*>(kSecretStr.c_str() ), kSecretStr.size() );
		Aws::Utils::ByteBuffer kDate = hmac(kSecret, dateStamp);
		Aws::Utils::ByteBuffer kRegion = hmac(kDate, region);
		Aws::Utils::ByteBuffer kService = hmac(kRegion, service);
		Aws::Utils::ByteBuffer kSigning = hmac(kService, "aws4_request");
		const Aws::String signature = HashingUtils::HexEncode(hmac(kSigning, stringToSign) );

		req.SetHeaderValue("authorization",
			"AWS4-HMAC-SHA256 Credential=" + accessKey + "/" + scope +
			", SignedHeaders=" + signedHeaders + ", Signature=" + signature);
	}

	// Build the request URI for a given object key, applying path-style or
	// virtual-hosted-style addressing.
	Aws::Http::URI buildUri(const std::string& bucket, const std::string& key) const
	{
		Aws::Http::URI uri;
		uri.SetScheme(scheme == "http" ? Aws::Http::Scheme::HTTP : Aws::Http::Scheme::HTTPS);

		if(virtualAddressing)
		{
			uri.SetAuthority(Aws::String(bucket.c_str() ) + "." + host);
			uri.SetPath("/" + Aws::String(key.c_str() ) );
		}
		else
		{
			uri.SetAuthority(host);
			uri.SetPath("/" + Aws::String(bucket.c_str() ) + "/" + Aws::String(key.c_str() ) );
		}

		// GetAuthority() drops the port, so set it explicitly — otherwise the
		// request goes to the scheme default (80/443) and fails to connect.
		if(port != 0)
			uri.SetPort(static_cast<uint16_t>(port) );

		return uri;
	}
};

S3RdmaControlPlane::S3RdmaControlPlane(const ProgArgs* progArgs, size_t workerRank) :
	impl(new Impl() )
{
	try
	{
		impl->region = progArgs->getS3Region().empty() ?
			Aws::String("us-east-1") : Aws::String(progArgs->getS3Region().c_str() );
		impl->virtualAddressing = progArgs->getUseS3VirtualAddressing();
		impl->scheme = "https";

		// Endpoint authority: round-robin select like S3Tk::initS3Client().
		const StringVec& endpointsVec = progArgs->getS3EndpointsVec();
		if(!endpointsVec.empty() )
		{
			// elbencho endpoint format is "[http(s)://]hostname[:port]". Aws::Http::URI
			// misparses a scheme-less authority (treats the host as the scheme), so default
			// to https (matching the SDK's default scheme) when no scheme is present.
			std::string endpoint = endpointsVec[workerRank % endpointsVec.size()];
			if(endpoint.find("://") == std::string::npos)
				endpoint = "https://" + endpoint;

			Aws::Http::URI ep(endpoint.c_str() );
			impl->scheme = (ep.GetScheme() == Aws::Http::Scheme::HTTP) ? "http" : "https";
			impl->host = ep.GetAuthority();
			impl->port = ep.GetPort();
		}
		else
		{
			impl->host = "s3." + impl->region + ".amazonaws.com";
			impl->port = (impl->scheme == "http") ? 80 : 443;
		}

		// Resolve credentials once (explicit params, credential store, else the
		// default chain) and store them for manual SigV4 signing.
		std::shared_ptr<Aws::Auth::AWSCredentialsProvider> credentialsProvider;

		if(!progArgs->getS3AccessKey().empty() || !progArgs->getS3AccessSecret().empty() )
			credentialsProvider = std::make_shared<Aws::Auth::SimpleAWSCredentialsProvider>(
				progArgs->getS3AccessKey(), progArgs->getS3AccessSecret(),
				progArgs->getS3SessionToken() );
		else
		if(!progArgs->getS3CredentialsFile().empty() ||
			!progArgs->getS3CredentialsList().empty() )
			credentialsProvider = S3CredentialStore::getInstance().getCredential(workerRank);
		else
			credentialsProvider = std::make_shared<Aws::Auth::DefaultAWSCredentialsProviderChain>();

		Aws::Auth::AWSCredentials creds = credentialsProvider->GetAWSCredentials();
		impl->accessKey = creds.GetAWSAccessKeyId();
		impl->secretKey = creds.GetAWSSecretKey();
		impl->sessionToken = creds.GetSessionToken();

		Aws::Client::ClientConfiguration config;
		config.region = impl->region;
		config.verifySSL = false;
		config.connectTimeoutMs = RDMA_CONNECT_TIMEOUT_SECS * 1000;
		config.requestTimeoutMs = RDMA_TIMEOUT_SECS * 1000;
		impl->http = Aws::Http::CreateHttpClient(config);

		valid = impl->http != nullptr && !impl->accessKey.empty();

		if(!valid)
			ERRLOGGER(Log_NORMAL, "S3 RDMA control plane init incomplete "
				"(missing HTTP client or credentials)." << std::endl);
	}
	catch(const std::exception& e)
	{
		ERRLOGGER(Log_NORMAL, "S3 RDMA control plane init failed: " << e.what() << std::endl);
		valid = false;
	}
}

S3RdmaControlPlane::~S3RdmaControlPlane() = default;

ssize_t S3RdmaControlPlane::rdmaPut(S3RdmaClientCtx& ctx, const char* token, uint64_t bufAddr,
	uint64_t size)
{
	try
	{
		Aws::Http::URI uri = impl->buildUri(ctx.bucket, ctx.object);

		if(!ctx.uploadID.empty() )
		{
			if(ctx.partNumber == 0 || ctx.partNumber > 10000)
			{
				ERRLOGGER(Log_NORMAL, "rdmaPut: invalid partNumber " << ctx.partNumber <<
					" for key=" << ctx.object << std::endl);
				return RDMA_ERROR;
			}
			uri.AddQueryStringParameter("uploadId", ctx.uploadID.c_str() );
			uri.AddQueryStringParameter("partNumber",
				std::to_string(ctx.partNumber).c_str() );
		}

		auto req = Aws::Http::CreateHttpRequest(uri, Aws::Http::HttpMethod::HTTP_PUT,
			Aws::Utils::Stream::DefaultResponseStreamFactoryMethod);
		req->SetHeaderValue("x-amz-content-sha256", UNSIGNED_PAYLOAD);
		req->SetHeaderValue(AMZ_RDMA_TOKEN, formatRdmaToken(token, bufAddr, size).c_str() );
		req->SetHeaderValue("content-type", "application/octet-stream");
		req->SetContentLength("0");

		impl->signV4(*req); // manual SigV4 with UNSIGNED-PAYLOAD

		auto resp = impl->http->MakeRequest(req);
		if(!resp)
		{
			ERRLOGGER(Log_NORMAL, "rdmaPut: MakeRequest returned null for key=" << ctx.object <<
				std::endl);
			return RDMA_ERROR;
		}

		const int httpStatus = static_cast<int>(resp->GetResponseCode() );
		const std::string etag =
			resp->HasHeader("etag") ? stripQuotes(resp->GetHeader("etag").c_str() ) : "";

		// Success: the server completed the RDMA_READ and returns a standard HTTP
		// 200 + ETag (the object payload moved out-of-band; the HTTP body is empty).
		if(httpStatus == 200 && !etag.empty() )
		{
			ctx.etag = etag;
			return static_cast<ssize_t>(size);
		}

		// Otherwise inspect the RDMA reply marker: 501 (or absent) => declined.
		const std::string reply = resp->HasHeader(AMZ_RDMA_REPLY) ?
			std::string(resp->GetHeader(AMZ_RDMA_REPLY).c_str() ) : "";
		const int replyCode = parseRdmaReply(reply);

		std::ostringstream body;
		body << resp->GetResponseBody().rdbuf();

		if(replyCode == static_cast<int>(RDMA_NOT_SUPPORTED) )
		{
			ERRLOGGER(Log_NORMAL, "rdmaPut declined. http=" << httpStatus <<
				"; x-amz-rdma-reply='" << reply << "'; key=" << ctx.object <<
				"; body=" << body.str().substr(0, 400) << std::endl);
			return RDMA_NOT_SUPPORTED;
		}

		ERRLOGGER(Log_NORMAL, "rdmaPut failed. http=" << httpStatus <<
			"; x-amz-rdma-reply='" << reply << "'; replyCode=" << replyCode <<
			"; key=" << ctx.object << "; body=" << body.str().substr(0, 400) << std::endl);
		return RDMA_ERROR;
	}
	catch(const std::exception& e)
	{
		ERRLOGGER(Log_NORMAL, "rdmaPut failed: " << e.what() << std::endl);
		return RDMA_ERROR;
	}
}

ssize_t S3RdmaControlPlane::rdmaGet(S3RdmaClientCtx& ctx, const char* token, uint64_t bufAddr,
	uint64_t size, uint64_t offset)
{
	try
	{
		Aws::Http::URI uri = impl->buildUri(ctx.bucket, ctx.object);

		auto req = Aws::Http::CreateHttpRequest(uri, Aws::Http::HttpMethod::HTTP_GET,
			Aws::Utils::Stream::DefaultResponseStreamFactoryMethod);
		req->SetHeaderValue("x-amz-content-sha256", UNSIGNED_PAYLOAD);
		req->SetHeaderValue(AMZ_RDMA_TOKEN, formatRdmaToken(token, bufAddr, size).c_str() );

		// Byte-range fetch when reading a slice of the object (server replies 206).
		if(size != 0)
			req->SetHeaderValue("range",
				("bytes=" + std::to_string(offset) + "-" +
					std::to_string(offset + size - 1) ).c_str() );

		impl->signV4(*req);

		auto resp = impl->http->MakeRequest(req);
		if(!resp)
		{
			ERRLOGGER(Log_NORMAL, "rdmaGet: MakeRequest returned null for key=" << ctx.object <<
				std::endl);
			return RDMA_ERROR;
		}

		// A non-RDMA server omits x-amz-rdma-reply, which parseRdmaReply maps to
		// "declined".
		const int httpStatus = static_cast<int>(resp->GetResponseCode() );
		const std::string reply = resp->HasHeader(AMZ_RDMA_REPLY) ?
			std::string(resp->GetHeader(AMZ_RDMA_REPLY).c_str() ) : "";
		const int replyCode = parseRdmaReply(reply);

		if(replyCode == static_cast<int>(RDMA_NOT_SUPPORTED) )
			return RDMA_NOT_SUPPORTED;

		if(replyCode != RDMA_REPLY_SUCCESS && replyCode != RDMA_REPLY_PARTIAL_CONTENT)
		{
			ERRLOGGER(Log_NORMAL, "rdmaGet failed. http=" << httpStatus <<
				"; x-amz-rdma-reply='" << reply << "'; replyCode=" << replyCode <<
				"; key=" << ctx.object << std::endl);
			return RDMA_ERROR;
		}

		if(resp->HasHeader("etag") )
			ctx.etag = stripQuotes(resp->GetHeader("etag").c_str() );

		// Trust the server's reported transferred byte count (can be < requested
		// for ranged/partial GETs).
		if(resp->HasHeader(AMZ_RDMA_BYTES_TRANSFERRED) )
		{
			try
			{
				const long long n =
					std::stoll(resp->GetHeader(AMZ_RDMA_BYTES_TRANSFERRED).c_str() );
				return n < 0 ? RDMA_ERROR : static_cast<ssize_t>(n);
			}
			catch(const std::exception&)
			{
				return RDMA_ERROR;
			}
		}

		return static_cast<ssize_t>(size);
	}
	catch(const std::exception& e)
	{
		ERRLOGGER(Log_NORMAL, "rdmaGet failed: " << e.what() << std::endl);
		return RDMA_ERROR;
	}
}

// ---------------------------------------------------------------------------
// Retry wrappers (token lifecycle + one transient retry). A token-mint failure
// is itself transient (cuObject NIC selection / registration hiccup), so it is
// retried rather than aborting on the first attempt.
// ---------------------------------------------------------------------------

ssize_t rdmaPutWithRetry(SharedCuObjClient& rdma, S3RdmaControlPlane& cp, S3RdmaClientCtx& ctx,
	void* buf, size_t size)
{
	ssize_t ret = RDMA_ERROR;

	for(int attempt = 0; attempt < RDMA_MAX_ATTEMPTS; attempt++)
	{
		char* token = rdma.getToken(buf, size, 0, CUOBJ_PUT);
		if(!token)
		{
			ret = RDMA_ERROR;
			continue; // transient mint failure: retry
		}

		ret = cp.rdmaPut(ctx, token, reinterpret_cast<uint64_t>(buf), size);
		rdma.putToken(token);

		if(ret > 0 || ret == RDMA_NOT_SUPPORTED)
			break;
	}

	return ret;
}

ssize_t rdmaGetWithRetry(SharedCuObjClient& rdma, S3RdmaControlPlane& cp, S3RdmaClientCtx& ctx,
	void* buf, size_t size, size_t offset)
{
	ssize_t ret = RDMA_ERROR;

	for(int attempt = 0; attempt < RDMA_MAX_ATTEMPTS; attempt++)
	{
		char* token = rdma.getToken(buf, size, 0, CUOBJ_GET);
		if(!token)
		{
			ret = RDMA_ERROR;
			continue; // transient mint failure: retry
		}

		ret = cp.rdmaGet(ctx, token, reinterpret_cast<uint64_t>(buf), size, offset);
		rdma.putToken(token);

		if(ret > 0 || ret == RDMA_NOT_SUPPORTED)
			break;
	}

	return ret;
}

#endif // S3_SUPPORT && CUOBJ_SUPPORT
