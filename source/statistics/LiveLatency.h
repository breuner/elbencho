// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef LIVELATENCY_H_
#define LIVELATENCY_H_

#include <algorithm>

#include "Common.h"

/**
 * Latency counters of one live stats interval: the number of values and their sum (for the
 * average) and the maximum, for IO ops and entries, each also for the rwmix read set. Workers and
 * totals both keep the raw counters, so a total is the sum of its workers.
 */
struct LiveLatency
{
	uint64_t numAvgIOLatValues; // number of values included in sum below (for avg calc)
	uint64_t avgIOLatMicroSecsSum; // sum of all numValues workers
    uint64_t maxIOLatMicroSecs;

	uint64_t numAvgIOLatReadMixValues; // number of values included in sum below (for avg calc)
	uint64_t avgIOLatReadMixMicroSecsSum; // sum of all numValues workers
    uint64_t maxIOLatReadMixMicroSecs;

	uint64_t numAvgEntriesLatValues; // number of values included in sum below (for avg calc)
	uint64_t avgEntriesLatMicroSecsSum; // sum of all numValues workers
    uint64_t maxEntriesLatMicroSecs;

	uint64_t numAvgEntriesLatReadMixValues; // number of values included in sum below (for avg calc)
	uint64_t avgEntriesLatReadMixMicrosSecsSum; // sum of all numValues workers
    uint64_t maxEntriesLatReadMixMicroSecs;

	void setToZero()
	{
	    *this = {};
	}

	/**
	 * Add current lat values to given outSumLat.
	 */
	void getAndAddOps(LiveLatency& outSumLat) const
	{
		outSumLat.numAvgIOLatValues += numAvgIOLatValues;
		outSumLat.avgIOLatMicroSecsSum += avgIOLatMicroSecsSum;
        outSumLat.maxIOLatMicroSecs = std::max(outSumLat.maxIOLatMicroSecs, maxIOLatMicroSecs);

		outSumLat.numAvgIOLatReadMixValues += numAvgIOLatReadMixValues;
		outSumLat.avgIOLatReadMixMicroSecsSum += avgIOLatReadMixMicroSecsSum;
        outSumLat.maxIOLatReadMixMicroSecs =
            std::max(outSumLat.maxIOLatReadMixMicroSecs, maxIOLatReadMixMicroSecs);

		outSumLat.numAvgEntriesLatValues += numAvgEntriesLatValues;
		outSumLat.avgEntriesLatMicroSecsSum += avgEntriesLatMicroSecsSum;
        outSumLat.maxEntriesLatMicroSecs =
            std::max(outSumLat.maxEntriesLatMicroSecs, maxEntriesLatMicroSecs);

		outSumLat.numAvgEntriesLatReadMixValues += numAvgEntriesLatReadMixValues;
		outSumLat.avgEntriesLatReadMixMicrosSecsSum += avgEntriesLatReadMixMicrosSecsSum;
        outSumLat.maxEntriesLatReadMixMicroSecs =
            std::max(outSumLat.maxEntriesLatReadMixMicroSecs, maxEntriesLatReadMixMicroSecs);
	}

    // averages (0 if the interval had no values)
    uint64_t getAvgIOLatMicroSecs() const
        { return numAvgIOLatValues ? (avgIOLatMicroSecsSum / numAvgIOLatValues) : 0; }
    uint64_t getAvgIOLatReadMixMicroSecs() const
        { return numAvgIOLatReadMixValues ?
            (avgIOLatReadMixMicroSecsSum / numAvgIOLatReadMixValues) : 0; }
    uint64_t getAvgEntriesLatMicroSecs() const
        { return numAvgEntriesLatValues ?
            (avgEntriesLatMicroSecsSum / numAvgEntriesLatValues) : 0; }
    uint64_t getAvgEntriesLatReadMixMicroSecs() const
        { return numAvgEntriesLatReadMixValues ?
            (avgEntriesLatReadMixMicrosSecsSum / numAvgEntriesLatReadMixValues) : 0; }
};

#endif /* LIVELATENCY_H_ */
