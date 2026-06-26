// SPDX-FileCopyrightText: 2020-2025 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef TOOLKITS_S3RDMAPROTOCOL_H_
#define TOOLKITS_S3RDMAPROTOCOL_H_

// S3-over-RDMA wire protocol helpers (the "x-amz-rdma-*" header convention used
// by NVIDIA cuObject). Kept free of any AWS SDK or cuObject dependency so the
// protocol logic stays independently testable.

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <string>
#include <system_error>
#include <sys/types.h>

namespace S3Rdma
{
	// S3 RDMA protocol headers (compatible with NVIDIA aws-c-s3 / cuObject).
	inline constexpr const char* AMZ_RDMA_TOKEN = "x-amz-rdma-token";
	inline constexpr const char* AMZ_RDMA_REPLY = "x-amz-rdma-reply";
	inline constexpr const char* AMZ_RDMA_BYTES_TRANSFERRED = "x-amz-rdma-bytes-transferred";

	// SigV4 payload hash sentinel for body-less RDMA control-plane requests.
	inline constexpr const char* UNSIGNED_PAYLOAD = "UNSIGNED-PAYLOAD";

	// RDMA reply status codes (carried in x-amz-rdma-reply, aligned with HTTP codes).
	inline constexpr int RDMA_REPLY_SUCCESS = 200; // transfer completed (PUT/GET)
	inline constexpr int RDMA_REPLY_NO_CONTENT = 204; // transfer completed, no content (PUT)
	inline constexpr int RDMA_REPLY_PARTIAL_CONTENT = 206; // partial transfer (ranged GET)
	inline constexpr int RDMA_REPLY_NOT_IMPLEMENTED = 501; // server declined RDMA (hard error)

	// Return-code sentinels shared by rdmaPut/rdmaGet (negative => caller errors; no
	// HTTP fallback). >0 is the number of bytes transferred on success.
	inline constexpr ssize_t RDMA_NOT_SUPPORTED = -2; // server declined RDMA
	inline constexpr ssize_t RDMA_ERROR = -1; // transport / unexpected failure

	// One transient retry: a fresh token mint + control-plane attempt recovers from
	// a transient cuObject token-acquisition or transport hiccup.
	inline constexpr int RDMA_MAX_ATTEMPTS = 2;

	// Aggressive control-plane timeouts (seconds) so a transport stall surfaces fast
	// and the retry path can take over.
	inline constexpr long RDMA_CONNECT_TIMEOUT_SECS = 5;
	inline constexpr long RDMA_TIMEOUT_SECS = 10;

	/**
	 * Format the value of the x-amz-rdma-token header.
	 *
	 * Wire format: "<descriptor>:<start_addr_hex>:<size_hex>" where the two trailing
	 * fields are 16-digit zero-padded lowercase hex. The descriptor is the token
	 * string returned by cuObjClient::cuMemObjGetRDMAToken().
	 */
	inline std::string formatRdmaToken(const char* descriptor, uint64_t bufAddr, uint64_t size)
	{
		char out[512];
		std::snprintf(out, sizeof(out), "%s:%016lx:%016lx",
			descriptor ? descriptor : "",
			static_cast<unsigned long>(bufAddr),
			static_cast<unsigned long>(size) );
		return std::string(out);
	}

	/**
	 * Map the server's x-amz-rdma-reply header value to a transfer outcome.
	 *
	 *   >0  reply code (200/204/206): treat as RDMA success
	 *    0  unparsable non-empty value: treat as failure by the caller
	 *   -2  reply is "501" OR absent/empty: server declined RDMA
	 *
	 * A GET success carries x-amz-rdma-reply: 200/206; its absence (a non-RDMA
	 * server never sets it) is read as a decline. PUT success is determined
	 * separately by HTTP 200 + ETag.
	 */
	inline int parseRdmaReply(const std::string& reply)
	{
		if(reply.empty() || reply == "501")
			return static_cast<int>(RDMA_NOT_SUPPORTED);

		// Require the ENTIRE value to be a valid integer; from_chars rejects
		// trailing junk ("200xyz" -> 200) that could mask a malformed reply.
		int value = 0;
		const char* begin = reply.data();
		const char* end = begin + reply.size();
		auto [parsedEnd, ec] = std::from_chars(begin, end, value);
		if(ec != std::errc{} || parsedEnd != end)
			return 0; // malformed -> caller treats as failure

		return value;
	}

} // namespace S3Rdma

#endif // TOOLKITS_S3RDMAPROTOCOL_H_
