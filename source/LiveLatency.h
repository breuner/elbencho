#ifndef LIVELATENCY_H_
#define LIVELATENCY_H_

#include "Common.h"

/**
 * Struct for live latency variables.
 */
struct LiveLatency
{
	uint64_t numAvgIOLatValues; // number of values included in sum below (for avg calc)
	uint64_t avgIOLatMicroSecsSum; // sum of all numValues workers

	uint64_t numAvgIOLatReadMixValues; // number of values included in sum below (for avg calc)
	uint64_t avgIOLatReadMixMicroSecsSum; // sum of all numValues workers

	uint64_t numAvgEntriesLatValues; // number of values included in sum below (for avg calc)
	uint64_t avgEntriesLatMicroSecsSum; // sum of all numValues workers

	uint64_t numAvgEntriesLatReadMixValues; // number of values included in sum below (for avg calc)
	uint64_t avgEntriesLatReadMixMicrosSecsSum; // sum of all numValues workers

	void setToZero()
	{
		numAvgIOLatValues = 0;
		avgIOLatMicroSecsSum = 0;

		numAvgIOLatReadMixValues = 0;
		avgIOLatReadMixMicroSecsSum = 0;

		numAvgEntriesLatValues = 0;
		avgEntriesLatMicroSecsSum = 0;

		numAvgEntriesLatReadMixValues = 0;
		avgEntriesLatReadMixMicrosSecsSum = 0;
	}

	/**
	 * Add current lat values to given outSumLat.
	 */
	void getAndAddOps(LiveLatency& outSumLat) const
	{
		outSumLat.numAvgIOLatValues += numAvgIOLatValues;
		outSumLat.avgIOLatMicroSecsSum += avgIOLatMicroSecsSum;

		outSumLat.numAvgIOLatReadMixValues += numAvgIOLatReadMixValues;
		outSumLat.avgIOLatReadMixMicroSecsSum += avgIOLatReadMixMicroSecsSum;

		outSumLat.numAvgEntriesLatValues += numAvgEntriesLatValues;
		outSumLat.avgEntriesLatMicroSecsSum += avgEntriesLatMicroSecsSum;

		outSumLat.numAvgEntriesLatReadMixValues += numAvgEntriesLatReadMixValues;
		outSumLat.avgEntriesLatReadMixMicrosSecsSum += avgEntriesLatReadMixMicrosSecsSum;
	}

	/**
	 * Calculate average values by dividing all by number of summed of values.
	 */
	void divAllByNumValues()
	{
		if(numAvgIOLatValues)
		{
			avgIOLatMicroSecsSum /= numAvgIOLatValues;
			numAvgIOLatValues = 1;
		}

		if(numAvgIOLatReadMixValues)
		{
			avgIOLatReadMixMicroSecsSum /= numAvgIOLatReadMixValues;
			numAvgIOLatReadMixValues = 1;
		}

		if(numAvgEntriesLatValues)
		{
			avgEntriesLatMicroSecsSum /= numAvgEntriesLatValues;
			numAvgEntriesLatValues = 1;
		}

		if(numAvgEntriesLatReadMixValues)
		{
			avgEntriesLatReadMixMicrosSecsSum /= numAvgEntriesLatReadMixValues;
			numAvgEntriesLatReadMixValues = 1;
		}
	}

};




#endif /* LIVELATENCY_H_ */
