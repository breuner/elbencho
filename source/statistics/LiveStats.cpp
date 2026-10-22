// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <exception>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "Common.h"
#include "Coordinator.h"
#include "statistics/LiveStats.h"
#include "Logger.h"
#include "ProgException.h"
#include "statistics/ResultTerms.h"
#include "statistics/Statistics.h"
#include "ftxui/dom/elements.hpp"
#include "toolkits/FileTk.h"
#include "toolkits/SignalTk.h"
#include "toolkits/TerminalTk.h"
#include "toolkits/UnitTk.h"
#include "workers/RemoteWorker.h"
#include "workers/Worker.h"

#define FULLSCREEN_WORKERS_TITLE_PADDING_SIZE       1 // 1 right
#define FULLSCREEN_HEADER_TITLE_PADDING_SIZE        1 // 1 right

#define FULLSCREEN_HEADER_TITLE_PHASE               "Phase:"
#define FULLSCREEN_HEADER_TITLE_CPU                 "CPU%:"
#define FULLSCREEN_HEADER_TITLE_ACTIVE              "Active:"
#define FULLSCREEN_HEADER_TITLE_ELAPSED             "Elapsed:"
#define FULLSCREEN_HEADER_TITLE_LATENCY             "Latency:"

#define FULLSCREEN_WORKERS_TITLE_RANK               "Rank"
#define FULLSCREEN_WORKERS_TITLE_COMPLETION_PCT     "%"
#define FULLSCREEN_WORKERS_TITLE_TRANSFERRED        "DoneMiB"
#define FULLSCREEN_WORKERS_TITLE_THROUGHPUT_BASE2   "MiB/s"
#define FULLSCREEN_WORKERS_TITLE_THROUGHPUT_BASE10  "MB/s"
#define FULLSCREEN_WORKERS_TITLE_IOPS               "IOPS"
#define FULLSCREEN_WORKERS_TITLE_ENTRIES_DONE       liveResults.entryTypeUpperCase
#define FULLSCREEN_WORKERS_TITLE_ENTRIES_PER_SEC    (liveResults.entryTypeUpperCase + "/s")
#define FULLSCREEN_WORKERS_TITLE_DIRS_DONE          "Dirs"
#define FULLSCREEN_WORKERS_TITLE_DIRS_PER_SEC       "Dirs/s"
#define FULLSCREEN_WORKERS_TITLE_ACTIVE             "Act"
#define FULLSCREEN_WORKERS_TITLE_CPU                "CPU"
#define FULLSCREEN_WORKERS_TITLE_SERVICE            "Service"

/**
 * return "std::to_string(VAL_B ? (VAL_A / VAL_B) : 0)" to prevent div by zero.
 */
#define SAFE_DIV_BY_ZERO_STR(VAL_A, VAL_B)          std::to_string( \
                                                    (VAL_B) ? ( (VAL_A) / (VAL_B) ) : 0)

/**
 * Assign an element to a 2D std::vector by row and column index and automatically grow the 2D
 * vector for this element if needed.
 */
#define VEC2D_SET_AUTOGROW(VEC, ROW_IDX, COL_IDX, VALUE)   \
    do \
    { \
        const unsigned rowIdxCpy = ROW_IDX; /* to let caller use "i++" safely */ \
        const unsigned colIdxCpy = COL_IDX; /* to let caller use "i++" safely */ \
        \
        IF_UNLIKELY(VEC.size() < (rowIdxCpy+1) ) \
            VEC.resize(rowIdxCpy+1); \
        \
        IF_UNLIKELY(VEC[rowIdxCpy].size() < (colIdxCpy+1) ) \
            VEC[rowIdxCpy].resize(colIdxCpy+1); \
        \
        VEC[rowIdxCpy][colIdxCpy] = VALUE; \
    } while(false)



LiveStats::~LiveStats()
{
	if(liveCSVFileFD != -1)
		close(liveCSVFileFD);
}

/**
 * Disable stdout line buffering. Intended for single line live stats and live countdown.
 *
 * @throw ProgException on error
 */
void LiveStats::disableConsoleBuffering()
{
	if(consoleBufferingDisabled)
		return; // already disabled, so nothing to do

	if(progArgs.getDisableLiveStats() )
		return; // live stats disabled

	bool disableRes = TerminalTk::disableConsoleBuffering();

	if(!disableRes)
		throw ProgException(std::string("Failed to disable buffering of stdout for live stats. ") +
				"SysErr: " + strerror(errno) );

	consoleBufferingDisabled = true;
}

/**
 * Intended to re-enable buffering after it was temporarily disabled for single line live stats.
 *
 * @throw ProgException on error
 */
void LiveStats::resetConsoleBuffering()
{
	if(!consoleBufferingDisabled)
		return;

	bool resetRes = TerminalTk::resetConsoleBuffering();

	if(!resetRes)
		ErrLogger(Log_DEBUG) << "Failed to reset buffering of stdout after live stats. "
				"SysErr: " << strerror(errno);

	consoleBufferingDisabled = false;
}

/**
 * Print live countdown until starttime.
 */
void LiveStats::printLiveCountdown()
{
	if(progArgs.getDisableLiveStats() )
		return; // live stats disabled

	if(!TerminalTk::isStdoutTTY() )
		return; // live stats won't work

	if(!progArgs.getStartTime() )
		return;

	disableConsoleBuffering();

	while(time(NULL) < progArgs.getStartTime() )
	{
		time_t waittimeSec = progArgs.getStartTime() - time(NULL);

		printLiveCountdownLine(waittimeSec);

		if(waittimeSec > 5)
			sleep(1);
		else
			usleep(1000); // shorter timeout for better start sync in distributed mode
	}

	TerminalTk::clearConsoleLine();
	resetConsoleBuffering();
}

/**
 * Print live countdown line to console.
 *
 * @waittimeSec remaining countdown seconds.
 */
void LiveStats::printLiveCountdownLine(unsigned long long waittimeSec)
{
	// "%c[2K" (27) is the VT100 code to clear the current line.
	// "\r" moves cursor to beginning of line.

	printf(CONTROLCHARS_CLEARLINE_AND_CARRIAGERETURN
		"Waiting for start time: %llus", (unsigned long long)waittimeSec);
}

/**
 * Print a live stats line, ensuring that the printed line does not exceed the terminal row length.
 *
 * @liveOpsPerSec operations per second.
 * @liveOps absolute values since beginning of current benchmark phase.
 * @elapsedSec elapsed seconds since beginning of current benchmark phase.
 */
void LiveStats::printSingleLineLiveStatsLine(LiveResults& liveResults)
{
	const size_t hiddenControlCharsLen = strlen(CONTROLCHARS_CLEARLINE_AND_CARRIAGERETURN);

    const ResultTerms terms = ResultTerms::forPhase(progArgs,
        liveResults.newLiveOpsReadMix.numBytesDone || liveResults.newLiveOpsReadMix.numEntriesDone);
	const bool useLiveStatsNewLine = progArgs.getUseBriefLiveStatsNewLine();
    std::string throughputUnitStr = progArgs.getShowThroughputBase10() ? "MB/s" : "MiB/s";
    uint64_t throughputDivisor = progArgs.getShowThroughputBase10() ? (1000*1000) : (1024*1024);

	std::chrono::seconds elapsedSec =
				std::chrono::duration_cast<std::chrono::seconds>
				(std::chrono::steady_clock::now() - workersSharedData.phaseStartT);

	// prepare line including control chars to clear line and do carriage return
	std::ostringstream stream;

	stream <<
		(useLiveStatsNewLine ? "" : CONTROLCHARS_CLEARLINE_AND_CARRIAGERETURN) <<
		liveResults.phaseName << ": ";

	if(terms.showEntries)
	{
		if(!terms.haveSecondary)
			stream <<
				liveResults.liveOpsPerSec.numEntriesDone << " " <<
					liveResults.phaseEntryType << "/s; " <<
				liveResults.liveOpsPerSec.numBytesDone / throughputDivisor <<
                    " " << throughputUnitStr << "; ";
		else
			stream <<
				terms.primaryShort << "=[" <<
					liveResults.liveOpsPerSec.numEntriesDone << " " <<
						liveResults.phaseEntryType << "/s; " <<
					liveResults.liveOpsPerSec.numBytesDone / throughputDivisor <<
                        " " << throughputUnitStr <<
				"] " <<
				terms.secondaryShort << "=[" <<
					(!terms.entriesPrimary.empty() ?
						std::to_string(liveResults.liveOpsPerSecReadMix.numEntriesDone) + " " +
							liveResults.phaseEntryType + "/s; " : "") <<
					liveResults.liveOpsPerSecReadMix.numBytesDone / throughputDivisor <<
                        " " << throughputUnitStr <<
				"]; ";

		const uint64_t numEntriesDoneCombined = liveResults.newLiveOps.numEntriesDone +
			liveResults.newLiveOpsReadMix.numEntriesDone;
		const uint64_t numBytesDoneCombined = liveResults.newLiveOps.numBytesDone +
			liveResults.newLiveOpsReadMix.numBytesDone;

		stream <<
			numEntriesDoneCombined << " " << liveResults.phaseEntryType << "; " <<
			numBytesDoneCombined / (1024*1024) << " MiB; ";
	}
	else // show IOPS instead of files/dirs per sec
	{
		if(!terms.haveSecondary)
			stream <<
				liveResults.liveOpsPerSec.numIOPSDone << " " << "IOPS; " <<
				liveResults.liveOpsPerSec.numBytesDone / throughputDivisor <<
                    " " << throughputUnitStr << "; " <<
				liveResults.newLiveOps.numBytesDone / (1024*1024) << " MiB; ";
		else
			stream <<
				terms.primaryShort << "=[" <<
					liveResults.liveOpsPerSec.numIOPSDone << " " << "IOPS; " <<
					liveResults.liveOpsPerSec.numBytesDone / throughputDivisor <<
                        " " << throughputUnitStr << "; " <<
					liveResults.newLiveOps.numBytesDone / (1024*1024) << " MiB"
				"] " <<
				terms.secondaryShort << "=[" <<
					liveResults.liveOpsPerSecReadMix.numIOPSDone << " " << "IOPS; " <<
					liveResults.liveOpsPerSecReadMix.numBytesDone / throughputDivisor <<
                        " " << throughputUnitStr << "; " <<
					liveResults.newLiveOpsReadMix.numBytesDone / (1024*1024) << " MiB"
				"]; ";
	}

	if(!progArgs.getHostsVec().empty() )
	{ // master mode
		stream <<
			liveResults.numRemoteThreadsLeft << " threads; " <<
			liveResults.percentRemoteCPU << "% CPU; ";
	}
	else
	{ // standalone mode
		stream <<
			(workerVec.size() - liveResults.numWorkersDone) << " threads; " <<
			(unsigned) liveCpuUtil.getCPUUtilPercent() << "% CPU; ";
	}

	stream <<
		UnitTk::elapsedSecToHumanStr(elapsedSec.count() );

	std::string lineStr(stream.str() );

	if(useLiveStatsNewLine)
	{ // add new line
		std::cerr << lineStr << std::endl;
	}
	else
	{ // update line in-place and don't exceed line length

		// check console size to handle console resize at runtime

		int terminalLineLen = TerminalTk::getTerminalLineLength();
		if(!terminalLineLen)
			return; // don't cancel program (by throwing) just because we can't show live stats

		// note: "-2" for "^C" printed when user presses ctrl+c
		unsigned usableLineLen = terminalLineLen + hiddenControlCharsLen - 2;

		if(lineStr.length() > usableLineLen)
			lineStr.resize(usableLineLen);

		std::cout << lineStr;
	}
}

/**
 * Print single line version of live stats until workers are done with phase.
 *
 * Note: the handover case from loopFullScreenLiveStats() is special because we need to calculate
 *      the next result not based on previously zero'ed live stats like we normally do at phase
 *      start but rather based on the last result that loopFullScreenLiveStats() got and
 *      correspondingly the next wake up time is also based on the last wake up at which
 *      loopFullScreenLiveStats() refreshed the given live stats.
 *
 * @param customLiveResults for handover from loopFullScreenLiveStats(), otherwise NULL.
 * @param customLastRefreshT for handover from loopFullScreenLiveStats(), otherwise NULL.
 * @param customNextRefreshT for handover from loopFullScreenLiveStats(), otherwise NULL.
 * @throw ProgException on error
 * @throw WorkerException if worker encountered error is detected
 */
void LiveStats::loopSingleLineLiveStats(LiveResults* customLiveResults,
    std::chrono::steady_clock::time_point* customLastRefreshT,
    std::chrono::steady_clock::time_point* customNextRefreshT)
{
    using SteadyClock = std::chrono::steady_clock;

    std::chrono::milliseconds statsRefreshIntervalMS(progArgs.getLiveStatsSleepMS() );
	SteadyClock::time_point lastStatsRefreshT = customLastRefreshT ?
        *customLastRefreshT : workersSharedData.phaseStartT;
	SteadyClock::time_point nextStatsRefreshT = customNextRefreshT ?
        *customNextRefreshT : (workersSharedData.phaseStartT + statsRefreshIntervalMS);
	const bool useLiveStatsNewLine = progArgs.getUseBriefLiveStatsNewLine();

	LiveResults liveResults(progArgs, workerManager, workersSharedData);

    if(customLiveResults)
        liveResults = *customLiveResults;

    if(!customLiveResults)
        liveCpuUtil.update(); // init (further updates in loop below)

    if(!useLiveStatsNewLine)
        disableConsoleBuffering();

    while(true)
	{
		std::unique_lock<std::mutex> lock(workersSharedData.mutex); // L O C K (scoped)

		bool workersDone = workersSharedData.condition.wait_until(
			lock, nextStatsRefreshT,
			[&, this]
			{ return workerManager.checkWorkersDoneUnlocked(&liveResults.numWorkersDone); } );
		if(workersDone)
			goto workers_done;

		lock.unlock(); // U N L O C K

		workerManager.checkPhaseTimeLimit(); // (interrupts workers if time limit exceeded)

        // live stats refresh (& init-once of fullscreen mode)...

        SteadyClock::time_point nowT = SteadyClock::now();

        if(nowT >= nextStatsRefreshT)
        {
            std::chrono::milliseconds elapsedRefreshMS =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    nowT - lastStatsRefreshT); // calc real elapsed time

            nextStatsRefreshT += statsRefreshIntervalMS; // prepare for next wait round

            IF_UNLIKELY(!elapsedRefreshMS.count() )
                continue; // skip this 0-elapsed round due to very laggy wake-up of stats thread

            lastStatsRefreshT = nowT;

            liveCpuUtil.update(); // update local cpu util
            updateLiveStatsRemoteInfo(liveResults); // update info for master mode
            updateLiveStatsLiveOps(liveResults, elapsedRefreshMS.count() ); // upd live ops & %done

            printLiveStatsCSV(liveResults); // live stats csv file

            printSingleLineLiveStatsLine(liveResults);
        }
	}

workers_done:

	if(!useLiveStatsNewLine)
	{
		TerminalTk::clearConsoleLine();
		resetConsoleBuffering();
	}
}

/**
 * Loop for live stats that are not on console (e.g. like live csv file stats).
 *
 * @throw ProgException on error
 * @throw WorkerException if worker encountered error is detected
 */
void LiveStats::loopNoConsoleLiveStats()
{
    using SteadyClock = std::chrono::steady_clock;

    std::chrono::milliseconds statsRefreshIntervalMS(progArgs.getLiveStatsSleepMS() );
	SteadyClock::time_point lastStatsRefreshT = workersSharedData.phaseStartT;
	SteadyClock::time_point nextStatsRefreshT =
        workersSharedData.phaseStartT + statsRefreshIntervalMS;

	LiveResults liveResults(progArgs, workerManager, workersSharedData);

	liveCpuUtil.update(); // init (further updates in loop below)

	while(true)
	{
		std::unique_lock<std::mutex> lock(workersSharedData.mutex); // L O C K (scoped)

		bool workersDone = workersSharedData.condition.wait_until(
			lock, nextStatsRefreshT,
			[&, this]
			{ return workerManager.checkWorkersDoneUnlocked(&liveResults.numWorkersDone); } );
		if(workersDone)
			goto workers_done;

		lock.unlock(); // U N L O C K

		workerManager.checkPhaseTimeLimit(); // (interrupts workers if time limit exceeded)

        // live stats refresh (& init-once of fullscreen mode)...

        SteadyClock::time_point nowT = SteadyClock::now();

        if(nowT >= nextStatsRefreshT)
        {
            std::chrono::milliseconds elapsedRefreshMS =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    nowT - lastStatsRefreshT); // calc real elapsed time

            nextStatsRefreshT += statsRefreshIntervalMS; // prepare for next wait round

            IF_UNLIKELY(!elapsedRefreshMS.count() )
                continue; // skip this 0-elapsed round due to very laggy wake-up of stats thread

            lastStatsRefreshT = nowT;

            liveCpuUtil.update(); // update local cpu util
            updateLiveStatsRemoteInfo(liveResults); // update info for master mode
            updateLiveStatsLiveOps(liveResults, elapsedRefreshMS.count() ); // upd live ops & %done

            printLiveStatsCSV(liveResults); // live stats csv file
        }
	}

workers_done:

	return;
}

/**
 * Generate ftxui renderer lambda for fullscreen live stats.
 */
 std::function<ftxui::Element()> LiveStats::fullscreenGenerateFtxuiRenderer(
    LiveResults& liveResults, LiveStatsUIState& uiState)
{
    using namespace ftxui;

    std::vector<StringVec>& headerStatsTxtTable = uiState.headerStatsTxtTable;
    std::vector<StringVec>& workerStatsTxtTable = uiState.workerStatsTxtTable;

    // note: no variable defintions here because of lifetime; we are returning a lambda below.

    return [&]()
    {
        const std::string throughputUnitTitleStr = progArgs.getShowThroughputBase10() ?
            FULLSCREEN_WORKERS_TITLE_THROUGHPUT_BASE10 : FULLSCREEN_WORKERS_TITLE_THROUGHPUT_BASE2;

        // short path if no update requested yet
        // (forced update happens through cachedView.reset() )
        if(uiState.cachedView)
            return uiState.cachedView;

        // render global info header lines...

        std::vector<Element> headerRows;

        for(int rowIdx = 0; rowIdx < (int)headerStatsTxtTable.size(); rowIdx++)
        {
            std::vector<Element> headerColumns; // Cells for the current row
            for(int colIdx = 0; colIdx < (int)headerStatsTxtTable[rowIdx].size(); colIdx++)
            {
                auto cell_content = text(headerStatsTxtTable[rowIdx][colIdx]);

                // even column numbers are keys/names, odd numbers are values
                if( (colIdx % 2) == 0)
                { // "key/name" column
                    cell_content = cell_content | bold;
                    cell_content = hbox( {cell_content, text(" ") } );
                }
                else
                { // "value" column
                    cell_content = hbox( {cell_content, text("  ") } ) | align_right;

                    // min cell width
                    if(checkIfVec2DLeftElemEquals(headerStatsTxtTable, rowIdx, colIdx,
                        FULLSCREEN_HEADER_TITLE_CPU) )
                        cell_content = cell_content | size(WIDTH, GREATER_THAN,
                            3 + FULLSCREEN_HEADER_TITLE_PADDING_SIZE);
                }

                headerColumns.push_back(std::move(cell_content) );
            }

            // wrap the columns into a horizontal box (one row)
            headerRows.push_back(hbox(std::move(headerColumns) ) );
        }

        // render worker stats table...

        std::vector<std::vector<Element> > workerElements;
        for(int rowIdx = 0; rowIdx < (int)workerStatsTxtTable.size(); rowIdx++)
        {
            std::vector<Element> row;
            for(int colIdx = 0; colIdx < (int)workerStatsTxtTable[rowIdx].size(); colIdx++)
            {
                auto cell_content = text(workerStatsTxtTable[rowIdx][colIdx]);

                // add padding on both sides
                cell_content = hbox( { cell_content, text(" ") } );

                // focus highlight
                if( (rowIdx == uiState.selectedRow) && (colIdx == uiState.selectedColumn) )
                    cell_content = cell_content | focus;

                // right-align
                if(workerStatsTxtTable[0][colIdx] != FULLSCREEN_WORKERS_TITLE_SERVICE)
                    cell_content = cell_content | align_right;

                // apply min cell width based on column title...

                unsigned minCellWidth = 0;

                if(workerStatsTxtTable[0][colIdx] == FULLSCREEN_WORKERS_TITLE_RANK)
                    minCellWidth = 5;
                else
                if(workerStatsTxtTable[0][colIdx] == FULLSCREEN_WORKERS_TITLE_COMPLETION_PCT)
                    minCellWidth = 3;
                else
                if(workerStatsTxtTable[0][colIdx] == FULLSCREEN_WORKERS_TITLE_TRANSFERRED)
                    minCellWidth = 10;
                else
                if(workerStatsTxtTable[0][colIdx] == throughputUnitTitleStr)
                    minCellWidth = 10;
                else
                if(workerStatsTxtTable[0][colIdx] == FULLSCREEN_WORKERS_TITLE_IOPS)
                    minCellWidth = 10;
                else
                if(workerStatsTxtTable[0][colIdx] == FULLSCREEN_WORKERS_TITLE_ENTRIES_DONE)
                    minCellWidth = 10;
                else
                if(workerStatsTxtTable[0][colIdx] == FULLSCREEN_WORKERS_TITLE_ENTRIES_PER_SEC)
                    minCellWidth = 10;
                else
                if(workerStatsTxtTable[0][colIdx] == FULLSCREEN_WORKERS_TITLE_DIRS_DONE)
                    minCellWidth = 10;
                else
                if(workerStatsTxtTable[0][colIdx] == FULLSCREEN_WORKERS_TITLE_DIRS_PER_SEC)
                    minCellWidth = 10;
                else
                if(workerStatsTxtTable[0][colIdx] == FULLSCREEN_WORKERS_TITLE_ACTIVE)
                    minCellWidth = 4;
                else
                if(workerStatsTxtTable[0][colIdx] == FULLSCREEN_WORKERS_TITLE_CPU)
                    minCellWidth = 3;

                if(minCellWidth)
                    cell_content = cell_content | size(WIDTH, GREATER_THAN,
                        minCellWidth + FULLSCREEN_WORKERS_TITLE_PADDING_SIZE);

                row.push_back(std::move(cell_content) );
            }

            /* add empty flex element on right side to each row to have inverted header
                extend to the far right side of the terminal */
            row.push_back(text("") | flex);

            workerElements.push_back(std::move(row) );
        }

        auto workerTable = Table(std::move(workerElements) );

        // format special rows in worker table
        IF_LIKELY(!workerStatsTxtTable.empty() )
        {
            workerTable.SelectRow(0).Decorate(inverted);
            workerTable.SelectRow(uiState.selectedRow).Decorate(bold);
        }

        // render footer...

        std::string footerText = printFullScreenLiveStatsFooter();

        // note: "flex" for "workerTable" makes the table span the full vertical space
        uiState.cachedView = vbox(
        {
            vbox(std::move(headerRows) ),
            separator(),
            vscroll_indicator( xframe( yframe( workerTable.Render() ) ) ) | flex,
            separator(),
            text(std::move(footerText) ) | center
        } );

        return uiState.cachedView;
    };
}

/**
 * Generate ftxui event handler for key press events in fullscreen live stats mode.
 */
 std::function<bool(ftxui::Event event)> LiveStats::fullscreenGenerateFtxuiEventHandler(
    LiveStatsUIState& uiState)
{
    using namespace ftxui;

    return [&](Event event)
    {
        int maxRow = (int)uiState.workerStatsTxtTable.size() - 1;
        int maxCol = uiState.workerStatsTxtTable.empty() ?
            0 : ( (int)uiState.workerStatsTxtTable[0].size() - 1);

        if(event == Event::CtrlC)
        { // fullscreen ftxui catches Ctrl+C as Event::CtrlC (so no SIGINT for this)
            Coordinator::handleInterruptSignal(SIGINT);
            uiState.cachedView.reset();
            return true;
        }
        else
        if( event == Event::Escape )
        {
            uiState.switchToSingleLine = true;
            uiState.cachedView.reset();
            return true;
        }
        else
        if( event == Event::Character( 'q' ) || event == Event::Character( 'Q' ) )
        {
            Coordinator::handleInterruptSignal(SIGINT);
            uiState.cachedView.reset();
            return true;
        }
        else
        if( event == Event::ArrowUp )
        {
            uiState.selectedRow = std::max( 1, uiState.selectedRow - 1 );
            uiState.cachedView.reset();
            return true;
        }
        else
        if( event == Event::ArrowDown )
        {
            uiState.selectedRow = std::min( maxRow, uiState.selectedRow + 1 );
            uiState.cachedView.reset();
            return true;
        }
        else
        if( event == Event::ArrowLeft )
        {
            uiState.selectedColumn = std::max( 0, uiState.selectedColumn - 1 );
            uiState.cachedView.reset();
            return true;
        }
        else
        if( event == Event::ArrowRight )
        {
            uiState.selectedColumn = std::min( maxCol, uiState.selectedColumn + 1 );
            uiState.cachedView.reset();
            return true;
        }
        else
        if( event == Event::PageUp )
        {
            uiState.selectedRow = std::max( 1, uiState.selectedRow - 10 );
            uiState.cachedView.reset();
            return true;
        }
        else
        if( event == Event::PageDown )
        {
            uiState.selectedRow = std::min( maxRow, uiState.selectedRow + 10 );
            uiState.cachedView.reset();
            return true;
        }
        else
        if( event == Event::Home )
        {
            uiState.selectedRow = 1;
            uiState.selectedColumn = 0;
            uiState.cachedView.reset();
            return true;
        }
        else
        if( event == Event::End )
        {
            uiState.selectedRow = maxRow;
            uiState.selectedColumn = maxCol;
            uiState.cachedView.reset();
            return true;
        }

        return false;
    };
}

/**
 * Print whole screen version of live stats until workers are done with phase.
 *
 * @throw ProgException on error
 * @throw WorkerException if worker encountered error is detected
 */
void LiveStats::loopFullScreenLiveStats()
{
    using namespace ftxui;
    using SteadyClock = std::chrono::steady_clock;

	std::chrono::milliseconds statsRefreshIntervalMS(progArgs.getLiveStatsSleepMS() );
    std::chrono::milliseconds keyCheckIntervalMS(200); // key press event check
	SteadyClock::time_point lastStatsRefreshT = workersSharedData.phaseStartT;
	SteadyClock::time_point nextStatsRefreshT =
        workersSharedData.phaseStartT + statsRefreshIntervalMS;
	bool ftxuiInitialized = false;
    LiveStatsUIState uiState;
    std::unique_ptr<ScreenInteractive> screen;
    std::unique_ptr<Loop> ftxuiMainLoop;

	LiveResults liveResults(progArgs, workerManager, workersSharedData);

	liveCpuUtil.update(); // init (further updates in loop below)

    // live stats update loop...
    while(true)
	{
        SteadyClock::time_point nextKeyCheckT = SteadyClock::now() + keyCheckIntervalMS;
        SteadyClock::time_point nextWakeupT = std::min(nextKeyCheckT, nextStatsRefreshT);

        IF_UNLIKELY(!ftxuiInitialized)
            nextWakeupT = nextStatsRefreshT; // don't handle keys if UI not initialized yet

		std::unique_lock<std::mutex> lock(workersSharedData.mutex); // L O C K (scoped)

		try
		{
			bool workersDone = workersSharedData.condition.wait_until(
				lock, nextWakeupT,
				[&, this]
				{ return workerManager.checkWorkersDoneUnlocked(&liveResults.numWorkersDone); } );
			if(workersDone)
				goto workers_done;
		}
		catch(std::exception& e)
		{
			if(ftxuiInitialized)
				screen->Exit();

			throw;
		}

		lock.unlock(); // U N L O C K


        workerManager.checkPhaseTimeLimit(); // (interrupts workers if time limit exceeded)

        // live stats refresh (& init-once of fullscreen mode)...

        SteadyClock::time_point nowT = SteadyClock::now();

        if(nowT >= nextStatsRefreshT)
        {
            std::chrono::milliseconds elapsedRefreshMS =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    nowT - lastStatsRefreshT); // calc real elapsed time

            nextStatsRefreshT += statsRefreshIntervalMS; // prepare for next wait round

            IF_UNLIKELY(!elapsedRefreshMS.count() )
                continue; // skip this 0-elapsed round due to very laggy wake-up of stats thread

            lastStatsRefreshT = nowT;

            uiState.cachedView.reset(); // force re-render of view

            liveCpuUtil.update(); // update local cpu util
            updateLiveStatsRemoteInfo(liveResults); // update info for master mode
            updateLiveStatsLiveOps(liveResults, elapsedRefreshMS.count() ); // upd live ops & %done

            // live stats csv file update
            printLiveStatsCSV(liveResults);

            // print global info table
            printFullScreenLiveStatsGlobalInfo(liveResults, uiState.headerStatsTxtTable);

            // print table of per-worker results
            printFullScreenLiveStatsWorkerTable(liveResults, uiState.workerStatsTxtTable);

            // delayed ftxui fullscreen init
            // (this is here instead of before loop to avoid empty screen for first wait interval)
            IF_UNLIKELY(!ftxuiInitialized)
            { // run the init-once steps of ftxui fullscreen mode

                // wipe input buffer before ftxui init
                // (this is because e.g. <ESC> before ftxui init would lead to invalid events)
                tcflush(STDIN_FILENO, TCIFLUSH);

                try
                {
                    /* note: we have to use a ScreenInteractive constructor based on "new" to bypass
                        the deleted move constructor of ScreenInteractive (based on C++17's
                        guaranteed copy elision). */
                    screen = std::unique_ptr<ScreenInteractive>(
                        new ScreenInteractive(ScreenInteractive::Fullscreen() ) );
                }
                catch(std::exception& e)
                {
                    std::cerr << "NOTE: fullscreen init for live statistics failed. "
                        "Falling back to \"--" ARG_BRIEFLIVESTATS_LONG "\"." << std::endl;

                        loopSingleLineLiveStats(&liveResults, &lastStatsRefreshT,
                            &nextStatsRefreshT);

                    return;
                }

                ftxuiInitialized = true;

                // disable mouse tracking so that mouse can be used to select and copy text
                screen->TrackMouse(false);

                // renderer for UI based on header stats and worker stats plain text tables
                auto renderer = Renderer(fullscreenGenerateFtxuiRenderer(liveResults, uiState) );

                // event handler for navigation keys
                auto component = CatchEvent(renderer,
                    fullscreenGenerateFtxuiEventHandler(uiState) );

                // custom loop to render screen updates exactly when we want to (via RunOnce() )
                ftxuiMainLoop = std::make_unique<Loop>(screen.get(), component);

                // ftxui registers its own signal handler, so we reset this back to our handler here
                SignalTk::registerFaultSignalHandlers(progArgs);
                Coordinator::registerInterruptSignalHandlers();

            } // end of !ftxuiInitialized

        } // end of live stats update

        // update the UI...

        IF_LIKELY(ftxuiInitialized)
        {
            // push custom event into the ftxui queue to prevent blocking of RunOnce()
            screen->Post(Event::Custom);

            // process ftxui event queue and render the terminal frame
            ftxuiMainLoop->RunOnce();

            // ctrl+c or fatal error => stop workers
            if(ftxuiMainLoop->HasQuitted() )
                workerManager.interruptAndNotifyWorkers();

            if(uiState.switchToSingleLine || ftxuiMainLoop->HasQuitted() )
                break;
        }
    }


workers_done:

    // cleanup (& enter single line mode if requested by user)
    if(ftxuiInitialized)
    {
        screen->Exit();
        ftxuiMainLoop.reset();
        screen.reset();
        ftxuiInitialized = false;

        if(uiState.switchToSingleLine)
            loopSingleLineLiveStats(&liveResults, &lastStatsRefreshT, &nextStatsRefreshT);
    }
}

/**
 * Print global info lines of fullscreen live stats.
 */
void LiveStats::printFullScreenLiveStatsGlobalInfo(const LiveResults& liveResults,
    std::vector<StringVec>& outHeaderStatsTxtTable)
{
    unsigned colIdx = 0; // current column index
    unsigned rowIdx = 0; // current row index

    const size_t timeLimitSecs = progArgs.getTimeLimitSecs();
    std::chrono::seconds elapsedSec =
                std::chrono::duration_cast<std::chrono::seconds>
                (std::chrono::steady_clock::now() - workersSharedData.phaseStartT);
    std::string elapsedTimeStr = UnitTk::elapsedSecToHumanStr(elapsedSec.count() );

    if(timeLimitSecs)
        elapsedTimeStr += " / " + UnitTk::elapsedSecToHumanStr(timeLimitSecs);

    VEC2D_SET_AUTOGROW(outHeaderStatsTxtTable, rowIdx, colIdx++, FULLSCREEN_HEADER_TITLE_PHASE);
    VEC2D_SET_AUTOGROW(outHeaderStatsTxtTable, rowIdx, colIdx++, liveResults.phaseName);
    VEC2D_SET_AUTOGROW(outHeaderStatsTxtTable, rowIdx, colIdx++, FULLSCREEN_HEADER_TITLE_CPU);
    VEC2D_SET_AUTOGROW(outHeaderStatsTxtTable, rowIdx, colIdx++,
        std::to_string( (unsigned) liveCpuUtil.getCPUUtilPercent() ) );
    VEC2D_SET_AUTOGROW(outHeaderStatsTxtTable, rowIdx, colIdx++, FULLSCREEN_HEADER_TITLE_ACTIVE);
    VEC2D_SET_AUTOGROW(outHeaderStatsTxtTable, rowIdx, colIdx++,
        std::to_string((workerVec.size() - liveResults.numWorkersDone) ) );
    VEC2D_SET_AUTOGROW(outHeaderStatsTxtTable, rowIdx, colIdx++, FULLSCREEN_HEADER_TITLE_ELAPSED);
    VEC2D_SET_AUTOGROW(outHeaderStatsTxtTable, rowIdx, colIdx++, elapsedTimeStr);

	if(!progArgs.getShowLatency() )
		return;

    // new row, reset column
    rowIdx++;
    colIdx = 0;

	// latency

    const ResultTerms terms = ResultTerms::forPhase(progArgs,
        liveResults.newLiveOpsReadMix.numBytesDone || liveResults.newLiveOpsReadMix.numEntriesDone);

    VEC2D_SET_AUTOGROW(outHeaderStatsTxtTable, rowIdx, colIdx++, FULLSCREEN_HEADER_TITLE_LATENCY);

    std::ostringstream latStream;

	if(!terms.haveSecondary)
	{
		if(progArgs.getBenchPathType() != BenchPathType_DIR)
			latStream << UnitTk::latencyUsToHumanStr(liveResults.liveLatency.avgIOLatMicroSecsSum);
		else
		{ // BenchPathType_DIR
			latStream <<
				"IO=" <<
				UnitTk::latencyUsToHumanStr(liveResults.liveLatency.avgIOLatMicroSecsSum) <<
				" " <<
				liveResults.entryTypeUpperCase << "=" <<
				UnitTk::latencyUsToHumanStr(liveResults.liveLatency.avgEntriesLatMicroSecsSum);
		}
	}
	else
	{ // rwmix or netbench
		latStream <<
			"IO [" << terms.primaryShort << "=" <<
			UnitTk::latencyUsToHumanStr(liveResults.liveLatency.avgIOLatMicroSecsSum) <<
			" " << terms.secondaryShort << "=" <<
			UnitTk::latencyUsToHumanStr(liveResults.liveLatency.avgIOLatReadMixMicroSecsSum) <<
			"]";

		if(progArgs.getBenchPathType() == BenchPathType_DIR)
		{
			latStream << "  "; // double space as separator

			if(terms.entriesPrimary.empty() )
			{
				latStream <<
					liveResults.entryTypeUpperCase << "=" <<
					UnitTk::latencyUsToHumanStr(
						liveResults.liveLatency.avgEntriesLatMicroSecsSum);
			}
			else
			{
				latStream <<
					liveResults.entryTypeUpperCase <<
					" [" << terms.primaryShort << "=" <<
					UnitTk::latencyUsToHumanStr(
						liveResults.liveLatency.avgEntriesLatMicroSecsSum) <<
					" " << terms.secondaryShort << "=" <<
					UnitTk::latencyUsToHumanStr(
						liveResults.liveLatency.avgEntriesLatReadMixMicrosSecsSum) <<
					"]";
			}
		}
	}

    VEC2D_SET_AUTOGROW(outHeaderStatsTxtTable, rowIdx, colIdx++, latStream.str() );
}

/**
 * Print table of per-worker results for fullscreen live stats (plus headline and total line).
 */
void LiveStats::printFullScreenLiveStatsWorkerTable(const LiveResults& liveResults,
    std::vector<StringVec>& statsTableData)
{
    const ResultTerms terms = ResultTerms::forPhase(progArgs,
        liveResults.newLiveOpsReadMix.numBytesDone || liveResults.newLiveOpsReadMix.numEntriesDone);
    const NetBenchProgArgs& netBenchArgs = progArgs.getNetBenchArgs();
    const std::string throughputUnitTitleStr = progArgs.getShowThroughputBase10() ?
        FULLSCREEN_WORKERS_TITLE_THROUGHPUT_BASE10 : FULLSCREEN_WORKERS_TITLE_THROUGHPUT_BASE2;
    const uint64_t throughputDivisor = progArgs.getShowThroughputBase10() ?
        (1000*1000) : (1024*1024);

	const bool showDirStats = progArgs.getShowDirStats() &&
		(progArgs.getBenchPathType() == BenchPathType_DIR) &&
		progArgs.getTreeFilePath().empty() &&
		( (workersSharedData.currentBenchPhase == BenchPhase_CREATEFILES) ||
			(workersSharedData.currentBenchPhase == BenchPhase_READFILES) );

    unsigned colIdx = 0; // current column index
    unsigned rowIdx = 0; // current row index

    // print table headline...

    VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++, FULLSCREEN_WORKERS_TITLE_RANK);
    VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++, FULLSCREEN_WORKERS_TITLE_COMPLETION_PCT);
    VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++, FULLSCREEN_WORKERS_TITLE_TRANSFERRED);
    VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++, throughputUnitTitleStr);
    VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++, FULLSCREEN_WORKERS_TITLE_IOPS);

    if(terms.showEntries)
    { // add columns for dir mode
        VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++, liveResults.entryTypeUpperCase);
        VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++,
            (liveResults.entryTypeUpperCase + "/s") );
    }

    if(showDirStats)
    { // add columns for dir stats in file write/read phase of dir mode
        VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++, FULLSCREEN_WORKERS_TITLE_DIRS_DONE);
        VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++, FULLSCREEN_WORKERS_TITLE_DIRS_PER_SEC);
    }

    if(!progArgs.getHostsVec().empty() )
    { // add columns for remote mode
        VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++, FULLSCREEN_WORKERS_TITLE_ACTIVE);
        VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++, FULLSCREEN_WORKERS_TITLE_CPU);
        VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++, FULLSCREEN_WORKERS_TITLE_SERVICE);
    }

    // new row, reset column
    rowIdx++;
    colIdx = 0;

    // print total for all workers...

    VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++, terms.primaryRow() );
    VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++,
        std::to_string(std::min(liveResults.percentDone, (size_t)100) ) );
    VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++,
		std::to_string(liveResults.newLiveOps.numBytesDone / (1024*1024) ) );
    VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++,
		std::to_string(liveResults.liveOpsPerSec.numBytesDone / throughputDivisor) );
    VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++,
		std::to_string(liveResults.liveOpsPerSec.numIOPSDone) );

    if(terms.showEntries)
    { // add columns for dir mode
        VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++,
			std::to_string(liveResults.newLiveOps.numEntriesDone) );
        VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++,
			std::to_string(liveResults.liveOpsPerSec.numEntriesDone) );
    }

    if(showDirStats)
    { // add columns for dir stats in file write/read phase of dir mode
        VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++,
            SAFE_DIV_BY_ZERO_STR(liveResults.newLiveOps.numEntriesDone, progArgs.getNumFiles() ) );
        VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++,
            SAFE_DIV_BY_ZERO_STR(
                liveResults.liveOpsPerSec.numEntriesDone, progArgs.getNumFiles() ) );
    }

    if(!progArgs.getHostsVec().empty() )
    { // add columns for remote mode
        VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++,
			std::to_string(liveResults.numRemoteThreadsLeft) );
        VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++,
			std::to_string(liveResults.percentRemoteCPU) );
        VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++, "");
    }

    // new row, reset column
    rowIdx++;
    colIdx = 0;

    // print rwmix read (or netbench recv) line for all workers...
    if(terms.haveSecondary)
    {
        VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++, terms.secondaryRow() );
        VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++,
            std::to_string(std::min(liveResults.percentDoneReadMix, (size_t)100) ) );
        VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++,
            std::to_string(liveResults.newLiveOpsReadMix.numBytesDone / (1024*1024) ) );
        VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++,
            std::to_string(liveResults.liveOpsPerSecReadMix.numBytesDone / throughputDivisor) );
        VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++,
            std::to_string(liveResults.liveOpsPerSecReadMix.numIOPSDone) );

        const bool haveReadEntries = !terms.entriesPrimary.empty();

        if(terms.showEntries)
        { // add columns for dir mode
            VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++, (haveReadEntries ?
                std::to_string(liveResults.newLiveOpsReadMix.numEntriesDone) : "-") );
            VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++, (haveReadEntries ?
                std::to_string(liveResults.liveOpsPerSecReadMix.numEntriesDone) : "-") );
        }

        if(showDirStats)
        { // add columns for dir stats in file write/read phase of dir mode
            size_t numFiles = progArgs.getNumFiles();

            VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++, (haveReadEntries ?
                SAFE_DIV_BY_ZERO_STR(liveResults.newLiveOpsReadMix.numEntriesDone, numFiles) :
                "-") );
            VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++, (haveReadEntries ?
                SAFE_DIV_BY_ZERO_STR(liveResults.liveOpsPerSecReadMix.numEntriesDone, numFiles) :
                "-") );
        }

        if(!progArgs.getHostsVec().empty() )
        { // add columns for remote mode
            VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++, "-");
            VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++, "-");
            VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++, "");
        }

        // new row, reset column
        rowIdx++;
        colIdx = 0;
    }

	// print individual worker result lines...

	for(size_t i=0; i < workerVec.size(); i++)
	{
		LiveOps workerDone, workerDoneReadMix; // total numbers
		LiveOps workerDonePerSec, workerDonePerSecReadMix; // difference to last round

		workerVec[i]->getLiveOps(workerDone, workerDoneReadMix);
		workerVec[i]->getAndResetDiffStats(workerDonePerSec, workerDonePerSecReadMix);
        terms.combine(workerDone, workerDoneReadMix);
        terms.combine(workerDonePerSec, workerDonePerSecReadMix);

		(workerDonePerSec *= 1000) /= progArgs.getLiveStatsSleepMS();

		size_t workerPercentDoneNum = 0;
		std::string workerPercentDoneStr = "-";

		// if we have bytes in this phase, use them for percent done; otherwise use num entries
		// (note: phases like sync and drop_caches have neither bytes nor entries)
		if(liveResults.numBytesPerWorker)
			workerPercentDoneNum =
				(100 * workerDone.numBytesDone) / liveResults.numBytesPerWorker;
		else
		if(liveResults.numEntriesPerWorker)
			workerPercentDoneNum =
				(100 * workerDone.numEntriesDone) / liveResults.numEntriesPerWorker;

		workerPercentDoneNum = std::min(workerPercentDoneNum, (size_t)100);
		workerPercentDoneStr = std::to_string(workerPercentDoneNum);

        if(netBenchArgs.isServerService(i) )
            workerPercentDoneStr = "-"; // (only clients have an expected amount of bytes)

        VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++, std::to_string(i) );
        VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++, workerPercentDoneStr);
        VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++,
			std::to_string(workerDone.numBytesDone / (1024*1024) ) );
        VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++,
            std::to_string(workerDonePerSec.numBytesDone / throughputDivisor) );
        VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++,
			std::to_string(workerDonePerSec.numIOPSDone) );

        if(terms.showEntries)
        { // add columns for dir mode
            VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++,
				std::to_string(workerDone.numEntriesDone) );
            VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++,
				std::to_string(workerDonePerSec.numEntriesDone) );
        }

        if(showDirStats)
        { // add columns for dir stats in file write/read phase of dir mode
            VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++,
                SAFE_DIV_BY_ZERO_STR(workerDone.numEntriesDone, progArgs.getNumFiles() ) );
            VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++,
                SAFE_DIV_BY_ZERO_STR(workerDonePerSec.numEntriesDone, progArgs.getNumFiles() ) );
        }

        if(!progArgs.getHostsVec().empty() )
        { // add columns for remote mode
            RemoteWorker* remoteWorker = static_cast<RemoteWorker*>(workerVec[i]);
            std::string pingStr = !progArgs.getSvcShowPing() ? std::string() :
                " (ping: " + std::to_string(remoteWorker->getPingMicroSecs() ) + "us)";

            VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++,
                std::to_string(progArgs.getNumThreads() - remoteWorker->getNumWorkersDone() ) );
            VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++,
				std::to_string(remoteWorker->getCPUUtilLive() ) );
            VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++,
                (progArgs.getHostsVec()[i] + netBenchArgs.getServiceRoleSuffix(i) + pingStr) );
        }

        // new row, reset column
        rowIdx++;
        colIdx = 0;
	}
}

/**
 * Generate footer line for fullscreen live stats view.
 */
 std::string LiveStats::printFullScreenLiveStatsFooter()
 {
    std::ostringstream stream;

    std::time_t currentTime = std::time(NULL);

    struct tm localTimeInfo;
    localtime_r(&currentTime, &localTimeInfo);

    stream << "[ <ESC>: Exit Fullscreen <CTRL+C>: Cancel \u2191\u2193: Browse ] | " <<
        std::put_time(&localTimeInfo, "%X %Y-%m-%d") << " | "
        EXE_NAME << " v" EXE_VERSION;

    return stream.str();
 }

/**
 * In master mode, update number of remote threads left in liveResults and avg cpu; otherwise do
 * nothing.
 */
void LiveStats::updateLiveStatsRemoteInfo(LiveResults& liveResults)
{
	if(progArgs.getHostsVec().empty() )
		return; // nothing to do if not in master mode

	size_t numRemoteThreadsTotal = workerVec.size() * progArgs.getNumThreads();
	liveResults.numRemoteThreadsLeft = numRemoteThreadsTotal;

	liveResults.percentRemoteCPU = 0;

	for(Worker* worker : workerVec)
	{
		RemoteWorker* remoteWorker = static_cast<RemoteWorker*>(worker);
		liveResults.numRemoteThreadsLeft -= remoteWorker->getNumWorkersDone();
		liveResults.percentRemoteCPU += remoteWorker->getCPUUtilLive();
	}

	liveResults.percentRemoteCPU /= workerVec.size();
}

/**
 * Update liveOps and percentDone of liveResults for current round of live stats.
 *
 * @param elapsedMS elapsed time since last update, usually this is
 *      progArgs.getLiveStatsSleepMS() plus some potential small thread wake up delay.
 */
void LiveStats::updateLiveStatsLiveOps(LiveResults& liveResults, size_t elapsedMS)
{
    // sanity check (to prevent div by zero): should never happen
    IF_UNLIKELY(!elapsedMS)
    {
        std::string backtraceStr = SignalTk::logBacktrace();
        throw ProgException(__func__ + std::string(" called with elapsedMS=0. ") +
            "This should never happen. "
            "Backtrace: \n" + backtraceStr);
    }

	statistics.getLiveOps(liveResults.newLiveOps, liveResults.newLiveOpsReadMix, liveResults.liveLatency);

	liveResults.liveOpsPerSec = liveResults.newLiveOps - liveResults.lastLiveOps;
	(liveResults.liveOpsPerSec *= 1000) /= elapsedMS;

	liveResults.lastLiveOps = liveResults.newLiveOps;

	liveResults.liveOpsPerSecReadMix =
		liveResults.newLiveOpsReadMix - liveResults.lastLiveOpsReadMix;
	(liveResults.liveOpsPerSecReadMix *= 1000) /= elapsedMS;

	liveResults.lastLiveOpsReadMix = liveResults.newLiveOpsReadMix;

    const size_t numWorkers = statistics.countAggregatedWorkers();

	// if we have bytes in this phase, use them for percent done; otherwise use num entries
	if(liveResults.numBytesPerWorker && numWorkers)
	{
		liveResults.percentDone =
			(100 * liveResults.newLiveOps.numBytesDone) /
			(liveResults.numBytesPerWorker * numWorkers);
		liveResults.percentDoneReadMix =
			(100 * liveResults.newLiveOpsReadMix.numBytesDone) /
			(liveResults.numBytesPerWorker * numWorkers);
	}
	else
	if(liveResults.numEntriesPerWorker && numWorkers)
	{
		liveResults.percentDone =
			(100 * liveResults.newLiveOps.numEntriesDone) /
			(liveResults.numEntriesPerWorker * numWorkers);
		liveResults.percentDoneReadMix =
			(100 * liveResults.newLiveOpsReadMix.numEntriesDone) /
			(liveResults.numEntriesPerWorker * numWorkers);
	}
	else
	{
		liveResults.percentDone = 0; // no % available in phases like "sync" or "dropcaches"
		liveResults.percentDoneReadMix = 0; // no % available in phases like "sync" or "dropcaches"
	}

	// calc latency average values
	liveResults.liveLatency.divAllByNumValues();
}

/**
 * Print live statistics on console until workers are done with phase.
 *
 * @throw ProgException on error
 * @throw WorkerException if worker encountered error is detected
 */
void LiveStats::printLiveStats()
{
    bool showConsoleStats = !progArgs.getDisableLiveStats() &&
        (TerminalTk::isStdoutTTY() || progArgs.getUseBriefLiveStatsNewLine() );

    prepLiveCSVFile();

    if(!showConsoleStats)
    {
        if(liveCSVFileFD == -1)
            return; // nohting to do here

        loopNoConsoleLiveStats();

        return;
    }

	if(!progArgs.getUseBriefLiveStats() )
	{
		if(TerminalTk::isScreenSessionWithoutAltscreen() )
		{ // GNU screen without "altscreen" can't restore console contents after fullscreen
			LOGGER(Log_NORMAL, "NOTE: Disabling fullscreen live stats because of GNU screen "
				"session without altscreen support. Restart GNU screen with \"altscreen on\" "
				"in your \"~/.screenrc\" (or add \"--live1\" to prevent this message)." <<
				std::endl);

			goto single_line_stats;
		}

        if(TerminalTk::isStandardWindowsConsole() )
        { // ftxui can't process key press events correctly in standard Windows console
            LOGGER(Log_NORMAL, "NOTE: Fullscreen live stats not supported in Windows console. "
                "(Adding \"--live1\" prevents this message)." <<
                std::endl);

            goto single_line_stats;
        }

		loopFullScreenLiveStats();

		return;
	}

single_line_stats:
    loopSingleLineLiveStats();
}

/**
 * Open and prepare csv file for live statistics.
 * This is  a no-op if user didn't set a live stats csv file.
 *
 * @throw ProgException on error, e.g. unable to open file.
 */
void LiveStats::prepLiveCSVFile()
{
	if(liveCSVFileFD != -1)
		return; // fd already prepared in previous phase

	if(progArgs.getLiveCSVFilePath().empty() )
		return; // nothing to do

	bool printCSVHeaders = true;

	if(progArgs.getLiveCSVFilePath() == ARG_LIVECSV_STDOUT)
	{ // live csv to stdout
	    liveCSVFileFD = dup(progArgs.getStdoutDupFD() );

        if(liveCSVFileFD == -1)
            throw ProgException(std::string("Unable to duplicate stdout file descriptor. ") +
                "SysErr: " + strerror(errno) );
	}
	else
	{ // output to regular file
        liveCSVFileFD = open(progArgs.getLiveCSVFilePath().c_str(),
            O_WRONLY | O_APPEND | O_CREAT, MKFILE_MODE);

        if(liveCSVFileFD == -1)
            throw ProgException("Unable to open live stats csv file: " +
                progArgs.getLiveCSVFilePath() );

        // print headers only if file is empty
        printCSVHeaders = FileTk::checkFileEmpty(progArgs.getLiveCSVFilePath() );
	}

	// print CSV table headers
	if(printCSVHeaders)
	{
		std::ostringstream stream;

		// print table headline...

		stream <<
		    "ISO Date,"
			"Label,"
			"Phase,"
			"RuntimeMS,"
			"Rank,"
			"MixType,"
			"Done%,"
			"DoneBytes,"
			"MiB/s,"
			"IOPS,"
			"Entries,"
			"Entries/s,"
			"Lat Ent us,"
			"Lat IO us,"
			"Active,"
			"CPU,"
			"Service," << std::endl;

		size_t streamLen = stream.tellp();

		int writeRes = write(liveCSVFileFD, stream.str().c_str(), streamLen);
		if(writeRes <= 0)
			throw ProgException("Unable to write labels to stats csv file: " +
				progArgs.getLiveCSVFilePath() );
	}

}

/**
 * Print total/aggregate and (if selected by user) also per-worker stats to file.
 * This is a no-op if no file for live csv stats was specified.
 *
 * @throw ProgException on file write error
 */
void LiveStats::printLiveStatsCSV(const LiveResults& liveResults)
{
	if(liveCSVFileFD == -1)
		return; // output file not open => nothing to do

    auto now = std::chrono::system_clock::now();
    time_t time = std::chrono::system_clock::to_time_t(now);
    auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000;

    struct tm localTimeInfo;
    localtime_r(&time, &localTimeInfo);

    std::stringstream dateStream;
    dateStream << std::put_time(&localTimeInfo, "%FT%T") << "."
        << std::setfill('0') << std::setw(3) << milliseconds
        << std::put_time(&localTimeInfo, "%z");

	std::chrono::milliseconds elapsedMS =
				std::chrono::duration_cast<std::chrono::milliseconds>
				(std::chrono::steady_clock::now() - workersSharedData.phaseStartT);

    // (the column values "Write"/"Read" are part of the file format, so they stay for all modes)
    const ResultTerms terms = ResultTerms::forPhase(progArgs,
        liveResults.newLiveOpsReadMix.numBytesDone || liveResults.newLiveOpsReadMix.numEntriesDone);
    const bool isRWMixPhase = terms.haveSecondary;
    const bool isRWMixThreadsPhase = !terms.entriesPrimary.empty();

	const bool isDirBenchPath = terms.showEntries;
	size_t numActiveWorkers = progArgs.getHostsVec().empty() ?
		(workerVec.size() - liveResults.numWorkersDone) : liveResults.numRemoteThreadsLeft;
	size_t cpuUtil = progArgs.getHostsVec().empty() ?
			liveCpuUtil.getCPUUtilPercent() : liveResults.percentRemoteCPU;

	std::ostringstream stream;

	// print total for all workers...

	stream <<
	    dateStream.str() << "," <<
		progArgs.getBenchLabelNoCommas() << "," <<
		liveResults.phaseName << "," <<
		elapsedMS.count() << "," <<
		"Total" << "," <<
		(isRWMixPhase ? "Write" : "") << "," <<
		std::min(liveResults.percentDone, (size_t)100) << "," <<
		liveResults.newLiveOps.numBytesDone << "," <<
		(liveResults.liveOpsPerSec.numBytesDone / (1024*1024) ) << "," <<
		liveResults.liveOpsPerSec.numIOPSDone << "," <<
		(isDirBenchPath ? liveResults.newLiveOps.numEntriesDone : 0) << "," <<
		(isDirBenchPath ? liveResults.liveOpsPerSec.numEntriesDone : 0) << "," <<
		liveResults.liveLatency.avgEntriesLatMicroSecsSum << "," <<
		liveResults.liveLatency.avgIOLatMicroSecsSum << "," <<
		numActiveWorkers << "," <<
		cpuUtil << ","
		"" << ","; // service

	stream << std::endl;

	// print rwmix total read line...
	if(isRWMixPhase)
	{
		uint64_t numEntriesDone = isRWMixThreadsPhase ?
				liveResults.newLiveOpsReadMix.numEntriesDone :
				liveResults.newLiveOps.numEntriesDone;
		uint64_t numEntriesDonePerSec = isRWMixThreadsPhase ?
				liveResults.liveOpsPerSecReadMix.numEntriesDone :
				liveResults.liveOpsPerSec.numEntriesDone;

		stream <<
	        dateStream.str() << "," <<
			progArgs.getBenchLabelNoCommas() << "," <<
			liveResults.phaseName << "," <<
			elapsedMS.count() << "," <<
			"Total" << "," <<
			"Read" << "," <<
			std::min(liveResults.percentDoneReadMix, (size_t)100) << "," <<
			(liveResults.newLiveOpsReadMix.numBytesDone) << "," <<
			(liveResults.liveOpsPerSecReadMix.numBytesDone / (1024*1024) ) << "," <<
			liveResults.liveOpsPerSecReadMix.numIOPSDone << "," <<
			(isDirBenchPath ? numEntriesDone : 0) << "," <<
			(isDirBenchPath ? numEntriesDonePerSec : 0) << "," <<
			liveResults.liveLatency.avgEntriesLatReadMixMicrosSecsSum << "," <<
			liveResults.liveLatency.avgIOLatReadMixMicroSecsSum << "," <<
			numActiveWorkers << "," <<
			cpuUtil << ","
			"" << ","; // service

		stream << std::endl;
	}

	// write to file
	if(!progArgs.getUseExtendedLiveCSV() )
	{ // no individual worker results requested => write and return
		size_t streamLen = stream.tellp();
		ssize_t writeRes = write(liveCSVFileFD, stream.str().c_str(), streamLen);

		if(writeRes) {} // only exists to mute compiler warning about unused write() result

		return;
	}

	// print individual worker result lines...

	/* note: we can't print per-sec stats for workers, because worker::getAndResetDiffStats does a
		reset which would interfere with the same reset for on-screen live stats. */

	for(size_t i=0; i < workerVec.size(); i++)
	{
		LiveOps workerDone; // total (rwmix write) numbers
		LiveOps workerDoneReadMix; // rwmix read numbers

		workerVec[i]->getLiveOps(workerDone, workerDoneReadMix);

		size_t workerPercentDone = 0;

		// if we have bytes in this phase, use them for percent done; otherwise use num entries
		// (note: phases like sync and drop_caches have neither bytes nor entries)
		if(liveResults.numBytesPerWorker)
			workerPercentDone = (100 * workerDone.numBytesDone) / liveResults.numBytesPerWorker;
		else
		if(liveResults.numEntriesPerWorker)
			workerPercentDone = (100 * workerDone.numEntriesDone) / liveResults.numEntriesPerWorker;

		stream <<
		    dateStream.str() << "," <<
			progArgs.getBenchLabelNoCommas() << "," <<
			liveResults.phaseName << "," <<
			elapsedMS.count() << "," <<
			i << "," <<
			(isRWMixPhase ? "Write" : "") << "," <<
			std::min(workerPercentDone, (size_t)100) << "," <<
			(workerDone.numBytesDone) << "," <<
			"" << "," << // bytes/s
			"" << "," << // iops
			(isDirBenchPath ? workerDone.numEntriesDone : 0) << "," <<
			"" << "," // entries/s
			"" << "," // entries lat
			"" << ","; // io lat

		// add columns for remote mode
		if(progArgs.getHostsVec().empty() )
		{ // no values to add in standalone mode
			stream << ",,,";
		}
		else
		{
			RemoteWorker* remoteWorker = static_cast<RemoteWorker*>(workerVec[i]);
			stream <<
				(progArgs.getNumThreads() - remoteWorker->getNumWorkersDone() ) << "," <<
				remoteWorker->getCPUUtilLive() << "," <<
				progArgs.getHostsVec()[i] << ",";
		}

		stream << std::endl;

		// print rwmix read worker result line
		if(isRWMixPhase)
		{
			uint64_t numEntriesDone = isRWMixThreadsPhase ?
				workerDoneReadMix.numEntriesDone : workerDone.numEntriesDone;

			size_t workerPercentDone = 0;

			// if we have bytes in this phase, use them for percent done; otherwise use num entries
			// (note: phases like sync and drop_caches have neither bytes nor entries)
			if(liveResults.numBytesPerWorker)
				workerPercentDone = (100 * workerDoneReadMix.numBytesDone) /
					liveResults.numBytesPerWorker;
			else
			if(liveResults.numEntriesPerWorker)
				workerPercentDone = (100 * numEntriesDone) /
					liveResults.numEntriesPerWorker;

			stream <<
			    dateStream.str() << "," <<
				progArgs.getBenchLabelNoCommas() << "," <<
				liveResults.phaseName << "," <<
				elapsedMS.count() << "," <<
				i << "," <<
				(isRWMixPhase ? "Read" : "") << "," <<
				std::min(workerPercentDone, (size_t)100) << "," <<
				(workerDoneReadMix.numBytesDone) << "," <<
				"" << "," << // bytes/s
				"" << "," << // iops
				(isDirBenchPath ? numEntriesDone : 0) << "," <<
				"" << "," // entries/s
				"" << "," // entries lat
				"" << ","; // io lat

			// add columns for remote mode
			if(progArgs.getHostsVec().empty() )
			{ // no values to add in standalone mode
				stream << ",,,";
			}
			else
			{
				RemoteWorker* remoteWorker = static_cast<RemoteWorker*>(workerVec[i]);
				stream <<
					(progArgs.getNumThreads() - remoteWorker->getNumWorkersDone() ) << "," <<
					remoteWorker->getCPUUtilLive() << "," <<
					progArgs.getHostsVec()[i] << ",";
			}

			stream << std::endl;

		} // end of rwmix worker read result

	} // end of workers loop

	// write to file
	size_t streamLen = stream.tellp();
	ssize_t writeRes = write(liveCSVFileFD, stream.str().c_str(), streamLen);

	if(writeRes) {} // only exists to mute compiler warning about unused write() result
}

/**
 * Check if the value left (i.e. column - 1) of the current element equals the given string value.
 *
 * @return true if equal, false otherwise.
 */
bool LiveStats::checkIfVec2DLeftElemEquals(std::vector<StringVec>& vec, int row, int column,
    const char* value)
{
    IF_UNLIKELY( (int)vec.size() < (row + 1) )
        return false; // vec doesn't have enough rows

    IF_UNLIKELY( (int)vec[row].size() < (column + 1) )
        return false; // row doesn't have enough columns

    IF_UNLIKELY(column == 0)
        return false;

    return (vec[row][column-1] == value);
}
