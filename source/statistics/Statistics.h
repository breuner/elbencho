// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef STATISTICS_H_
#define STATISTICS_H_

#include <string>

#include "CPUUtil.h"
#include "Common.h"
#include "statistics/LiveLatency.h"
#include "statistics/LiveStats.h"
#include "statistics/PhaseResultsPrinter.h"
#include "ProgArgs.h"
#include "workers/WorkerManager.h"
#include "workers/WorkersSharedData.h"

/**
 * Aggregation of the worker counters: live values during a phase, the results of a completed
 * phase, and both as property trees for the service HTTP API. The console and file output is
 * done by LiveStats and PhaseResultsPrinter.
 */
class Statistics
{
	public:
		Statistics(const ProgArgs& progArgs, WorkerManager& workerManager) :
			progArgs(progArgs), workerManager(workerManager),
			workersSharedData(workerManager.getWorkersSharedData() ),
			workerVec(workerManager.getWorkerVec() ),
            liveStats(progArgs, workerManager, *this, liveCpuUtil),
            printer(progArgs, workersSharedData, workerVec) {}

		void printPhaseResults();

		void getLiveOps(LiveOps& outLiveOps, LiveOps& outLiveRWMixReadOps,
			LiveLatency& outLiveLatency);
        bool isAggregated(size_t workerIdx) const;
        size_t countAggregatedWorkers() const;
		void getLiveStatsAsPropertyTreeForService(bpt::ptree& outTree);
		void getBenchResultAsPropertyTreeForService(bpt::ptree& outTree);

		void printDryRunInfo();

	private:
		const ProgArgs& progArgs;
		WorkerManager& workerManager;
		WorkersSharedData& workersSharedData;
		WorkerVec& workerVec;
		CPUUtil liveCpuUtil; // updated by live stats loop or through http service live stat calls
        LiveStats liveStats;
        PhaseResultsPrinter printer;

		bool generatePhaseResults(PhaseResults& phaseResults);
		void generateServicesMinMax(PhaseResults& phaseResults);
		void printDryRunPhaseInfo(BenchPhase benchPhase);

	// inliners
	public:
		void printLiveCountdown() { liveStats.printLiveCountdown(); }
		void printLiveStats() { liveStats.printLiveStats(); }
		void printPhaseResultsTableHeader() { printer.printPhaseResultsTableHeader(); }

		/**
		 * To be called by HTTP service on start of a benchmark phase.
		 */
		void updateLiveCPUUtil()
		{
			liveCpuUtil.update();
		}
};

#endif /* STATISTICS_H_ */
