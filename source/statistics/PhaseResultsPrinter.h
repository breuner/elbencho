// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef PHASERESULTSPRINTER_H_
#define PHASERESULTSPRINTER_H_

#include <ostream>
#include <string>

#include "Common.h"
#include "statistics/LatencyHistogram.h"
#include "statistics/LiveOps.h"
#include "ProgArgs.h"
#include "workers/WorkerManager.h"
#include "workers/WorkersSharedData.h"

/**
 * Per-service rate extremes of a phase (for "--svcminmax"). Empty hosts mean "not set".
 */
struct ServiceMinMax
{
    uint64_t minPerSec{0};
    uint64_t avgPerSec{0};
    uint64_t maxPerSec{0};
    std::string minHost;
    std::string maxHost;
};

/**
 * The aggregated results of a completed benchmark phase.
 */
class PhaseResults
{
	public:
		uint64_t firstFinishUSec; // stonewall: time to completion of fastest worker
		uint64_t lastFinishUSec; // time to completion of slowest worker

		LiveOps opsTotal; // processed by all workers
		LiveOps opsTotalReadMix; // rwmix read processed by all workers
		LiveOps opsStoneWallTotal; // processed by all workers when stonewall was hit
		LiveOps opsStoneWallTotalReadMix; // rwmix read proc'ed by all workers when stonewall hit

		LiveOps opsPerSec; // total per sec for all workers by last finisher
		LiveOps opsPerSecReadMix; // rwmix read total per sec for all workers by last finisher
		LiveOps opsStoneWallPerSec; // total per sec for all workers by 1st finisher
		LiveOps opsStoneWallPerSecReadMix; // rwmix read total per sec for all workers by 1st fin

		float cpuUtilPercent; // cpu utilization until last finisher
		float cpuUtilStoneWallPercent; // cpu utilization until first finisher

		LatencyHistogram iopsLatHisto; // sum of all histograms
		LatencyHistogram iopsLatHistoReadMix; // rwmix read sum of all histograms
		LatencyHistogram entriesLatHisto; // sum of all histograms
		LatencyHistogram entriesLatHistoReadMix; // rwmix read sum of all histograms

        // per-service min/avg/max by own completion time (netbench: the clients)
        ServiceMinMax svcIOPS;
        ServiceMinMax svcBytesPerSec;
        ServiceMinMax svcServersIOPS; // netbench only: the servers
        ServiceMinMax svcServersBytesPerSec;
};

/**
 * Prints the results of a completed phase: the table on the console and in the results file,
 * the csv file line and the json file line. The row labels come from ResultTerms, so this knows
 * nothing about the benchmark modes.
 */
class PhaseResultsPrinter
{
	public:
		PhaseResultsPrinter(const ProgArgs& progArgs, WorkersSharedData& workersSharedData,
            WorkerVec& workerVec) :
			progArgs(progArgs), workersSharedData(workersSharedData), workerVec(workerVec) {}

		void printPhaseResultsTableHeader();
		void printPhaseResults(bool genRes, const PhaseResults& phaseResults,
            const LiveOpsSampleVec& graphSamples);

	private:
		const ProgArgs& progArgs;
		WorkersSharedData& workersSharedData;
		WorkerVec& workerVec;
		const std::string phaseResultsFormatStr{"%|-11| %|-17|%|1| %|11| %|11|"}; // note line below
		const std::string phaseResultsLeftFormatStr{"%|-11| %|-17|%|1| "}; // left side format str
		static constexpr size_t operationColWidth = 11; // first column of the format strings
		const std::string phaseResultsFooterStr = std::string(3, '-');

		void printPhaseStartISODateToStringVec(StringVec& outLabelsVec, StringVec& outResultsVec);
		void printPhaseResultsTableHeaderToStream(std::ostream& outStream);
		void printPhaseResultsToStream(const PhaseResults& phaseResults, std::ostream& outStream,
            const LiveOpsSampleVec* graphSamples = NULL);
        void printPhaseResultsGraphToStream(const PhaseResults& phaseResults,
            const LiveOpsSampleVec& samples, bool useColors, std::ostream& outStream);
		void printPhaseResultsToStringVec(const PhaseResults& phaseResults, StringVec& outLabelsVec,
			StringVec& outResultsVec);
		void printPhaseResultsLatencyToStream(const LatencyHistogram& latHisto,
			std::string latTypeStr, std::ostream& outStream);
		void printPhaseResultsLatencyToStringVec(const LatencyHistogram& latHisto,
			std::string latTypeStr, StringVec& outLabelsVec, StringVec& outResultsVec);
		void printPhaseResultsAsJSON(const PhaseResults& phaseResults);
};

#endif /* PHASERESULTSPRINTER_H_ */
