#ifndef STATISTICS_H_
#define STATISTICS_H_

#include "CPUUtil.h"
#include "ProgArgs.h"
#include "workers/WorkerManager.h"
#include "workers/WorkersSharedData.h"

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
};

/**
 * For whole screen live stats
 */
class LiveResults
{
	public:
		std::string phaseName; // read, write, mkdirs etc
		std::string phaseEntryType; // files or dirs
		std::string entryTypeUpperCase; // phaseEntryType uppercase
		size_t numEntriesPerWorker; // total number for this phase
		uint64_t numBytesPerWorker; // total number for this phase

		size_t numWorkersDone;
		size_t numRemoteThreadsLeft; // only set in master mode
		size_t percentRemoteCPU; // avg percent cpu util of service hosts (only set in master mode)

		int winHeight; // current height of terminal window
		int winWidth; // current width of terminal window

		LiveOps lastLiveOps = {}; // live ops from last round for per-sec diff
		LiveOps lastLiveOpsReadMix = {}; // live ops from last round for per-sec diff
		LiveOps newLiveOps; // live ops from current round
		LiveOps newLiveOpsReadMix; // live ops from current round
		LiveOps liveOpsPerSec; // live ops per sec from diff of new and last live ops
		LiveOps liveOpsPerSecReadMix; // live ops per sec from diff of new and last live ops
		size_t percentDone; // total percent done based on bytes (if any) or num entries in phase
		size_t percentDoneReadMix; // total percent done based on bytes (if any) or entries in phase
};

class Statistics
{
	public:
		Statistics(const ProgArgs& progArgs, WorkerManager& workerManager) :
			progArgs(progArgs), workerManager(workerManager),
			workersSharedData(workerManager.getWorkersSharedData() ),
			workerVec(workerManager.getWorkerVec() ) {}
		~Statistics();

		void printLiveCountdown();
		void printLiveStats();

		void printPhaseResultsTableHeader();
		void printPhaseResults();

		void getLiveOps(LiveOps& outLiveOps, LiveOps& outLiveRWMixReadOps);
		void getLiveStatsAsPropertyTree(bpt::ptree& outTree);
		void getBenchResultAsPropertyTree(bpt::ptree& outTree);

		void printDryRunInfo();

	private:
		const ProgArgs& progArgs;
		WorkerManager& workerManager;
		WorkersSharedData& workersSharedData;
		WorkerVec& workerVec;
		bool consoleBufferingDisabled{false};
		const std::string phaseResultsFormatStr{"%|-9| %|-17|%|1| %|10| %|10|"};
		const std::string phaseResultsLeftFormatStr{"%|-9| %|-17|%|1| "}; // left side format str
		const std::string phaseResultsFooterStr = std::string(3, '-');
		CPUUtil liveCpuUtil; // updated by live stats loop or through http service live stat calls
		int liveCSVFileFD = -1; // fd for live stats csv file

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

		void printSingleLineLiveStatsLine(LiveResults& liveResults);
		void deleteSingleLineLiveStatsLine();
		void loopSingleLineLiveStats();

		void printFullScreenLiveStatsGlobalInfo(const LiveResults& liveResults);
		void printFullScreenLiveStatsWorkerTable(const LiveResults& liveResults);
		void printFullScreenLiveStatsLine(std::ostringstream& stream, unsigned lineLength,
			bool fillIfShorter);
		void loopFullScreenLiveStats();

		void loopNoConsoleLiveStats();

		void updateLiveStatsRemoteInfo(LiveResults& liveResults);
		void updateLiveStatsLiveOps(LiveResults& liveResults);

		void printDryRunPhaseInfo(BenchPhase benchPhase);

		void prepLiveCSVFile();
		void printLiveStatsCSV(const LiveResults& liveResults);

	// inliners
	public:
		/**
		 * To be called by HTTP service on start of a benchmark phase.
		 */
		void updateLiveCPUUtil()
		{
			liveCpuUtil.update();
		}

};

#endif /* STATISTICS_H_ */
