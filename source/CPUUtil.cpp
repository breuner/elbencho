#include <fstream>
#include <iostream>
#include <numeric>
#include <unistd.h>
#include <vector>
#include "CPUUtil.h"
#include "ProgException.h"
#include "workers/WorkersSharedData.h"

#define STAT_FILE				"/proc/stat"
#define STAT_CPU_IDLE_IDX		3 // index of idle column in STAT_FILE
#define STAT_IO_WAIT_IDX		4 // index of iowait column in STAT_FILE
#define STAT_LINE_SKIP_CHARS	5 // num chars to skip for first column of cpu line in STAT_FILE


/**
 * Update internal cpu times counters. Call this at the start and end of the interval for which the
 * CPU utilization should be calculated.
 *
 * @throw ProgException if not enough values could be read from STAT_FILE.
 */
void CPUUtil::update()
{
	SizeTVec cpuTimesVec;

	// read cpu times from first line of STAT_FILE into cpuTimesVec

	std::ifstream procStatStream(STAT_FILE);
	procStatStream.ignore(STAT_LINE_SKIP_CHARS, ' '); // skip the "cpu" prefix in STAT_FILE
	for (size_t cpuTime; procStatStream >> cpuTime; cpuTimesVec.push_back(cpuTime) );

	IF_UNLIKELY(cpuTimesVec.size() < (STAT_IO_WAIT_IDX+1) )
		throw ProgException("Unable to read CPU usage values from file: " STAT_FILE);

	// rotate current cpu times to last cpu times

	lastIdleCPUTime = currentIdleCPUTime;
	lastTotalCPUTime = currentTotalCPUTime;

	// update current cpu times

	currentIdleCPUTime = cpuTimesVec[STAT_CPU_IDLE_IDX] + cpuTimesVec[STAT_IO_WAIT_IDX];
	currentTotalCPUTime = std::accumulate(cpuTimesVec.begin(), cpuTimesVec.end(), 0);
}
