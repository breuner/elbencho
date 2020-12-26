#ifndef LIVEOPS_H_
#define LIVEOPS_H_

#include <atomic>
#include "toolkits/UnitTk.h"

/**
 * Struct for live stats variables.
 */
struct LiveOps
{
	uint64_t numEntriesDone; // number of dirs/files done
	uint64_t numBytesDone; // number of bytes written/read
	uint64_t numIOPSDone; // number of write/read ops done

	void setToZero()
	{
		numEntriesDone = 0;
		numBytesDone = 0;
		numIOPSDone = 0;
	}

	/**
	 * Add current ops values to given outSumOps.
	 */
	void getAndAddOps(LiveOps& outSumOps) const
	{
		outSumOps.numEntriesDone += numEntriesDone;
		outSumOps.numBytesDone += numBytesDone;
		outSumOps.numIOPSDone += numIOPSDone;
	}

	/**
	 * Calculate per second values from total values based on given elapsed time.
	 *
	 * @elapsedUSec elapsed time as basis for per-sec calculation.
	 * @outLiveOps per-sec values.
	 */
	void getPerSecFromUSec(uint64_t elapsedUsec, LiveOps& outLiveOps)
	{
		outLiveOps.numEntriesDone = UnitTk::getPerSecFromUSec(numEntriesDone, elapsedUsec);
		outLiveOps.numBytesDone = UnitTk::getPerSecFromUSec(numBytesDone, elapsedUsec);
		outLiveOps.numIOPSDone = UnitTk::getPerSecFromUSec(numIOPSDone, elapsedUsec);
	}

	LiveOps operator-(const LiveOps& other) const
    {
		LiveOps result;

		result.numEntriesDone = numEntriesDone - other.numEntriesDone;
		result.numBytesDone = numBytesDone - other.numBytesDone;
		result.numIOPSDone = numIOPSDone - other.numIOPSDone;

        return result;
    }

	LiveOps& operator/=(const size_t& rhs)
	{
		numEntriesDone /= rhs;
		numBytesDone /= rhs;
		numIOPSDone /= rhs;

		return *this;
	}

	LiveOps& operator*=(const size_t& rhs)
	{
		numEntriesDone *= rhs;
		numBytesDone *= rhs;
		numIOPSDone *= rhs;

		return *this;
	}

};

/**
 * Struct for atomic live stats variables. (See struct LiveOps for meaning of members.)
 *
 * Note: Atomic "fast" means that the type can also be larger than desired size if that is faster
 * 		for the given CPU architecture.
 */
struct AtomicLiveOps
{
	std::atomic_uint_fast64_t numEntriesDone;
	std::atomic_uint_fast64_t numBytesDone;
	std::atomic_uint_fast64_t numIOPSDone;

	void setToZero()
	{
		numEntriesDone = 0;
		numBytesDone = 0;
		numIOPSDone = 0;
	}

	void getAsLiveOps(LiveOps& outLiveOps) const
	{
		outLiveOps.numEntriesDone = numEntriesDone;
		outLiveOps.numBytesDone = numBytesDone;
		outLiveOps.numIOPSDone = numIOPSDone;
	}

	/**
	 * Add current ops values to given outSumLiveOps.
	 */
	void getAndAddLiveOps(LiveOps& outSumLiveOps) const
	{
		outSumLiveOps.numEntriesDone += numEntriesDone;
		outSumLiveOps.numBytesDone += numBytesDone;
		outSumLiveOps.numIOPSDone += numIOPSDone;
	}
};


#endif /* LIVEOPS_H_ */
