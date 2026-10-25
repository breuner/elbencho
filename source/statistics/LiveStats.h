// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef LIVESTATS_H_
#define LIVESTATS_H_

#include <chrono>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/table.hpp>
#include <functional>
#include <string>
#include <vector>

#include "CPUUtil.h"
#include "Common.h"
#include "statistics/LiveLatency.h"
#include "ProgArgs.h"
#include "toolkits/TranslatorTk.h"
#include "workers/WorkerManager.h"
#include "workers/WorkersSharedData.h"

class Statistics;

/**
 * Phase values for live stats
 */
class LiveResults
{
	public:
        LiveResults(const ProgArgs& progArgs, const WorkerManager& workerManager,
            const WorkersSharedData& workersSharedData)
        {
            this->phaseName = TranslatorTk::benchPhaseToPhaseName(
                workersSharedData.currentBenchPhase, &progArgs);
            this->phaseEntryType = TranslatorTk::benchPhaseToPhaseEntryType(
                workersSharedData.currentBenchPhase, &progArgs);
            this->entryTypeUpperCase = TranslatorTk::benchPhaseToPhaseEntryType(
                workersSharedData.currentBenchPhase, &progArgs, true);

            workerManager.getPhaseNumEntriesAndBytes(
                this->numEntriesPerWorker, this->numBytesPerWorker);
        }

		std::string phaseName; // read, write, mkdirs etc
		std::string phaseEntryType; // files or dirs
		std::string entryTypeUpperCase; // phaseEntryType uppercase
		size_t numEntriesPerWorker; // total number for this phase
		uint64_t numBytesPerWorker; // total number for this phase

		size_t numWorkersDone;
		size_t numRemoteThreadsLeft; // only set in master mode
		size_t percentRemoteCPU; // avg percent cpu util of service hosts (only set in master mode)

		int winHeight; // current height of terminal window (set in fullscreen mode only)
		int winWidth; // current width of terminal window (set in fullscreen mode only)

		LiveOps lastLiveOps = {}; // live ops from last round for per-sec diff
		LiveOps lastLiveOpsReadMix = {}; // live ops from last round for per-sec diff
		LiveOps newLiveOps; // live ops from current round
		LiveOps newLiveOpsReadMix; // live ops from current round
		LiveOps liveOpsPerSec; // live ops per sec from diff of new and last live ops
		LiveOps liveOpsPerSecReadMix; // live ops per sec from diff of new and last live ops
		size_t percentDone; // total percent done based on bytes (if any) or num entries in phase
		size_t percentDoneReadMix; // total percent done based on bytes (if any) or entries in phase

		LiveLatency liveLatency; // avg latency across all workers for current live stats interval
};

/**
 * Rendering state info for ftxui fullscreen live stats GUI.
 */
class LiveStatsUIState
{
    public:
        bool switchToSingleLine = false; // true if user wants to continue without fullscreen
        int selectedRow = 1;
        int selectedColumn = 0;
        std::vector<StringVec> headerStatsTxtTable; // all live stats values as plain text
        std::vector<StringVec> workerStatsTxtTable; // all live stats values as plain text
        ftxui::Element cachedView; // to prevent expensive UI redraw for quick key press check
};

/**
 * Live statistics while a phase runs: the countdown to the start time, the single-line and the
 * fullscreen console stats and the live csv file. The counters come from Statistics, which also
 * decides which workers count. The row labels come from ResultTerms.
 */
class LiveStats
{
	public:
		LiveStats(const ProgArgs& progArgs, WorkerManager& workerManager, Statistics& statistics,
            CPUUtil& liveCpuUtil) :
			progArgs(progArgs), workerManager(workerManager),
			workersSharedData(workerManager.getWorkersSharedData() ),
			workerVec(workerManager.getWorkerVec() ), statistics(statistics),
            liveCpuUtil(liveCpuUtil) {}
		~LiveStats();

		void printLiveCountdown();
		void printLiveStats();

        const LiveOpsSampleVec& getSamples() const { return samples; }

	private:
		const ProgArgs& progArgs;
		WorkerManager& workerManager;
		WorkersSharedData& workersSharedData;
		WorkerVec& workerVec;
        Statistics& statistics;
		CPUUtil& liveCpuUtil; // owned by Statistics, also updated through the http service
		bool consoleBufferingDisabled{false};
		int liveCSVFileFD = -1; // fd for live stats csv file
        LiveOpsSampleVec samples; // the live stats intervals of the current phase ("--graph")

		void disableConsoleBuffering();
		void resetConsoleBuffering();

		void printLiveCountdownLine(unsigned long long waittimeSec);

		void printSingleLineLiveStatsLine(LiveResults& liveResults);
        void loopSingleLineLiveStats(LiveResults* customLiveResults = NULL,
            std::chrono::steady_clock::time_point* customLastRefreshT = NULL,
            std::chrono::steady_clock::time_point* customNextRefreshT = NULL);

		void printFullScreenLiveStatsGlobalInfo(const LiveResults& liveResults,
			std::vector<StringVec>& outHeaderStatsTxtTable);
		void printFullScreenLiveStatsWorkerTable(const LiveResults& liveResults,
            std::vector<StringVec>& statsTableData);
        std::string printFullScreenLiveStatsFooter();
        std::function<ftxui::Element()> fullscreenGenerateFtxuiRenderer(
            LiveResults& liveResults, LiveStatsUIState& uiState);
        std::function<bool(ftxui::Event event)> fullscreenGenerateFtxuiEventHandler(
            LiveStatsUIState& uiState);
		void loopFullScreenLiveStats();

		void loopNoConsoleLiveStats();

		void updateLiveStatsRemoteInfo(LiveResults& liveResults);
		void updateLiveStatsLiveOps(LiveResults& liveResults, size_t elapsedMS);

		void prepLiveCSVFile();
		void printLiveStatsCSV(const LiveResults& liveResults);

        bool checkIfVec2DLeftElemEquals(std::vector<StringVec>& vec, int row, int column,
            const char* value);
};

#endif /* LIVESTATS_H_ */
