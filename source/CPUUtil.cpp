#include <fstream>
#include <iostream>
#include <numeric>
#include <unistd.h>
#include <vector>
#include "CPUUtil.h"
#include "ProgException.h"
#include "workers/WorkersSharedData.h"

#define STAT_FILE				"/proc/stat"

#define STAT_IDX_USER_TIME		0 // index of user mode time col in STAT_FILE
#define STAT_IDX_USERNICE_TIME	1 // index of user mode nice prio time col in STAT_FILE
#define STAT_IDX_SYSTEM_TIME	2 // index of system/kernel mode time col in STAT_FILE
#define STAT_IDX_CPU_IDLE		3 // index of idle column in STAT_FILE
#define STAT_IDX_IO_WAIT		4 // index of iowait column in STAT_FILE

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

	/* (we have to read all fields to add them up and don't know how many fields the current
		kernel has, but we can at least reserve some reasonable number to avoid lots of reallocs
		in read loop) */
	cpuTimesVec.reserve(10); // just to avoid lots of allocs in loop

	// read cpu times from first line of STAT_FILE into cpuTimesVec

	std::ifstream procStatStream(STAT_FILE);
	procStatStream.ignore(STAT_LINE_SKIP_CHARS, ' '); // skip the "cpu" prefix in STAT_FILE
	for(size_t cpuTime; procStatStream >> cpuTime; cpuTimesVec.push_back(cpuTime) );

	// rotate current cpu times to last cpu times

	lastIdleCPUTime = currentIdleCPUTime;
	lastTotalCPUTime = currentTotalCPUTime;

	// update current cpu times

	IF_UNLIKELY(cpuTimesVec.size() < (STAT_IDX_CPU_IDLE+1) )
		throw ProgException("Unable to read CPU usage values from file: " STAT_FILE);

	IF_UNLIKELY(cpuTimesVec.size() < (STAT_IDX_IO_WAIT+1) )
	{ // this kernel doesn't have iowait (e.g. cygwin)
		currentIdleCPUTime = cpuTimesVec[STAT_IDX_CPU_IDLE];
		currentTotalCPUTime = std::accumulate(cpuTimesVec.begin(), cpuTimesVec.end(), 0);
	}
	else
	{ // this kernel has iowait
		currentIdleCPUTime = cpuTimesVec[STAT_IDX_CPU_IDLE] + cpuTimesVec[STAT_IDX_IO_WAIT];
		currentTotalCPUTime = std::accumulate(cpuTimesVec.begin(), cpuTimesVec.end(), 0);
	}

}
