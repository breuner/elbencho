// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef S3REQUESTOPS_H_
#define S3REQUESTOPS_H_

#include <atomic>
#include "toolkits/UnitTk.h"

/**
 * Counters for S3 HTTP/API request types (for --s3-rps reporting).
 * Each field counts completed (or initiated, for async) S3 API round-trips, not keys/bytes.
 */
struct S3RequestOps
{
	uint64_t numGet; // GET: GetObject, ACL/tagging reads, ListParts, etc.
	uint64_t numPut; // PUT: PutObject, UploadPart, CreateBucket, ACL/tagging writes, etc.
	uint64_t numHead; // HEAD: HeadObject, HeadBucket
	uint64_t numPost; // POST: Create/Complete/Abort MPU, DeleteObjects
	uint64_t numDelete; // DELETE: DeleteObject, DeleteBucket, tagging deletes
	uint64_t numList; // LIST: ListObjectsV2 API calls (not key count)

	void setToZero()
	{
		numGet = 0;
		numPut = 0;
		numHead = 0;
		numPost = 0;
		numDelete = 0;
		numList = 0;
	}

	uint64_t getTotal() const
	{
		return numGet + numPut + numHead + numPost + numDelete + numList;
	}

	void getAndAddOps(S3RequestOps& outSumOps) const
	{
		outSumOps.numGet += numGet;
		outSumOps.numPut += numPut;
		outSumOps.numHead += numHead;
		outSumOps.numPost += numPost;
		outSumOps.numDelete += numDelete;
		outSumOps.numList += numList;
	}

	/**
	 * Calculate per-second values from totals based on elapsed microseconds.
	 */
	void getPerSecFromUSec(uint64_t elapsedUsec, S3RequestOps& outPerSec) const
	{
		outPerSec.numGet = UnitTk::getPerSecFromUSec(numGet, elapsedUsec);
		outPerSec.numPut = UnitTk::getPerSecFromUSec(numPut, elapsedUsec);
		outPerSec.numHead = UnitTk::getPerSecFromUSec(numHead, elapsedUsec);
		outPerSec.numPost = UnitTk::getPerSecFromUSec(numPost, elapsedUsec);
		outPerSec.numDelete = UnitTk::getPerSecFromUSec(numDelete, elapsedUsec);
		outPerSec.numList = UnitTk::getPerSecFromUSec(numList, elapsedUsec);
	}

	S3RequestOps operator-(const S3RequestOps& other) const
	{
		S3RequestOps result;

		result.numGet = numGet - other.numGet;
		result.numPut = numPut - other.numPut;
		result.numHead = numHead - other.numHead;
		result.numPost = numPost - other.numPost;
		result.numDelete = numDelete - other.numDelete;
		result.numList = numList - other.numList;

		return result;
	}

	S3RequestOps& operator/=(const size_t& rhs)
	{
		numGet /= rhs;
		numPut /= rhs;
		numHead /= rhs;
		numPost /= rhs;
		numDelete /= rhs;
		numList /= rhs;

		return *this;
	}

	S3RequestOps& operator*=(const size_t& rhs)
	{
		numGet *= rhs;
		numPut *= rhs;
		numHead *= rhs;
		numPost *= rhs;
		numDelete *= rhs;
		numList *= rhs;

		return *this;
	}
};

/**
 * Atomic counterpart of S3RequestOps for per-worker live counters.
 */
struct AtomicS3RequestOps
{
	std::atomic_uint_fast64_t numGet;
	std::atomic_uint_fast64_t numPut;
	std::atomic_uint_fast64_t numHead;
	std::atomic_uint_fast64_t numPost;
	std::atomic_uint_fast64_t numDelete;
	std::atomic_uint_fast64_t numList;

	void setToZero()
	{
		numGet = 0;
		numPut = 0;
		numHead = 0;
		numPost = 0;
		numDelete = 0;
		numList = 0;
	}

	void getAsS3RequestOps(S3RequestOps& outOps) const
	{
		outOps.numGet = numGet;
		outOps.numPut = numPut;
		outOps.numHead = numHead;
		outOps.numPost = numPost;
		outOps.numDelete = numDelete;
		outOps.numList = numList;
	}

	void getAndAddOps(S3RequestOps& outSumOps) const
	{
		outSumOps.numGet += numGet;
		outSumOps.numPut += numPut;
		outSumOps.numHead += numHead;
		outSumOps.numPost += numPost;
		outSumOps.numDelete += numDelete;
		outSumOps.numList += numList;
	}
};

#endif /* S3REQUESTOPS_H_ */
