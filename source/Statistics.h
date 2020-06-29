#ifndef STATISTICS_H_
#define STATISTICS_H_

#include "ProgArgs.h"
#include "WorkerManager.h"
#include "WorkersSharedData.h"


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

		void printPhaseResultsTableHeaderToStream(std::ostream& outstream);
		void printPhaseResultsToStream(std::ostream& outstream);
		void printPhaseResultsLatency(LatencyHistogram& latHisto, std::string latTypeStr,
			std::ostream& outstream);

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
