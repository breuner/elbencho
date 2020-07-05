#ifndef STATISTICS_H_
#define STATISTICS_H_

#include "ProgArgs.h"
#include "WorkerManager.h"
#include "WorkersSharedData.h"

class PhaseResults
{
	public:
		size_t firstFinishMS; // stonewall: time to completion of fastest worker
		size_t lastFinishMS; // time to completion of slowest worker

		LiveOps opsTotal; // processed by all workers
		LiveOps opsStoneWallTotal; // processed by all workers when stonewall was hit

		LiveOps opsPerSec; // total per sec for all workers by last finisher
		LiveOps opsStoneWallPerSec; // total per sec for all workers by 1st finisher

		LatencyHistogram iopsLatHisto; // sum of all histograms
		LatencyHistogram entriesLatHisto; // sum of all histograms
};

class Statistics
{
	public:
		Statistics(const ProgArgs& progArgs, WorkerManager& workerManager) :
			progArgs(progArgs), workerManager(workerManager),
			workersSharedData(workerManager.getWorkersSharedData() ),
			workerVec(workerManager.getWorkerVec() ) {}

		void printLiveCountdown();
		void printLiveStats();

		void printPhaseResultsTableHeader();
		void printPhaseResults();

		void getLiveOps(LiveOps& outLiveOps);
		void getLiveStatsAsPropertyTree(bpt::ptree& outTree);
		void getBenchResultAsPropertyTree(bpt::ptree& outTree);

	private:
		const ProgArgs& progArgs;
		WorkerManager& workerManager;
		WorkersSharedData& workersSharedData;
		WorkerVec& workerVec;
		bool consoleBufferingDisabled{false};
		const std::string phaseResultsFormatStr{"%|-9| %|-13| %|1| %|10| %|10|"};
		const std::string phaseResultsLeftFormatStr{"%|-9| %|-13| %|1| "}; // left side format str

		void disableConsoleBuffering();
		void resetConsoleBuffering();

		void printISODateToStringVec(StringVec& outLabelsVec, StringVec& outResultsVec);
		void printPhaseResultsTableHeaderToStream(std::ostream& outStream);
		bool generatePhaseResults(PhaseResults& phaseResults);
		void printPhaseResultsToStream(const PhaseResults& phaseResults, std::ostream& outStream);
		void printPhaseResultsToStringVec(const PhaseResults& phaseResults, StringVec& outLabelsVec,
			StringVec& outResultsVec);
		void printPhaseResultsLatencyToStream(const LatencyHistogram& latHisto,
			std::string latTypeStr, std::ostream& outStream);
		void printPhaseResultsLatencyToStringVec(const LatencyHistogram& latHisto,
			std::string latTypeStr, StringVec& outLabelsVec, StringVec& outResultsVec);

		void printLiveCountdownLine(unsigned long long waittimeSec);

		void printSingleLineLiveStatsLine(const char* phaseName, const char* phaseEntryType,
			LiveOps& liveOpsPerSec, LiveOps& liveOps, unsigned long long numWorkersLeft,
			unsigned long long elapsedSec);
		void deleteSingleLineLiveStatsLine();
		void printSingleLineLiveStats();

		void printWholeScreenLine(std::ostringstream& stream, unsigned lineLength,
			bool fillIfShorter);
		void printWholeScreenLiveStats();
};

#endif /* STATISTICS_H_ */
