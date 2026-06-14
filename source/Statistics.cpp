// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <algorithm>
#include <boost/format.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <chrono>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "Common.h"
#include "Coordinator.h"
#include "ProgException.h"
#include "Statistics.h"
#include "ftxui/dom/elements.hpp"
#include "toolkits/FileTk.h"
#include "toolkits/SignalTk.h"
#include "toolkits/TerminalTk.h"
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

#define PHASERESULTS_CONSOLE_SEPARATOR_LINE         "---"

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



Statistics::~Statistics()
{
	if(liveCSVFileFD != -1)
		close(liveCSVFileFD);
}

/**
 * Disable stdout line buffering. Intended for single line live stats and live countdown.
 *
 * @throw ProgException on error
 */
void Statistics::disableConsoleBuffering()
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
void Statistics::resetConsoleBuffering()
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
void Statistics::printLiveCountdown()
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
void Statistics::printLiveCountdownLine(unsigned long long waittimeSec)
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
void Statistics::printSingleLineLiveStatsLine(LiveResults& liveResults)
{
	const size_t hiddenControlCharsLen = strlen(CONTROLCHARS_CLEARLINE_AND_CARRIAGERETURN);

	const bool isRWMixPhase = (liveResults.newLiveOpsReadMix.numBytesDone ||
		liveResults.newLiveOpsReadMix.numEntriesDone);
	const bool isRWMixThreadsPhase = (isRWMixPhase && progArgs.hasUserSetRWMixReadThreads() );
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

	if(progArgs.getBenchPathType() == BenchPathType_DIR)
	{
		if(!isRWMixPhase)
			stream <<
				liveResults.liveOpsPerSec.numEntriesDone << " " <<
					liveResults.phaseEntryType << "/s; " <<
				liveResults.liveOpsPerSec.numBytesDone / throughputDivisor <<
                    " " << throughputUnitStr << "; ";
		else
			stream <<
				"wr=[" <<
					liveResults.liveOpsPerSec.numEntriesDone << " " <<
						liveResults.phaseEntryType << "/s; " <<
					liveResults.liveOpsPerSec.numBytesDone / throughputDivisor <<
                        " " << throughputUnitStr <<
				"] " <<
				"rd=[" <<
					(isRWMixThreadsPhase ?
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
		if(!isRWMixPhase)
			stream <<
				liveResults.liveOpsPerSec.numIOPSDone << " " << "IOPS; " <<
				liveResults.liveOpsPerSec.numBytesDone / throughputDivisor <<
                    " " << throughputUnitStr << "; " <<
				liveResults.newLiveOps.numBytesDone / (1024*1024) << " MiB; ";
		else
			stream <<
				"wr=[" <<
					liveResults.liveOpsPerSec.numIOPSDone << " " << "IOPS; " <<
					liveResults.liveOpsPerSec.numBytesDone / throughputDivisor <<
                        " " << throughputUnitStr << "; " <<
					liveResults.newLiveOps.numBytesDone / (1024*1024) << " MiB"
				"] " <<
				"rd=[" <<
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
void Statistics::loopSingleLineLiveStats(LiveResults* customLiveResults,
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
void Statistics::loopNoConsoleLiveStats()
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
 std::function<ftxui::Element()> Statistics::fullscreenGenerateFtxuiRenderer(
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
 std::function<bool(ftxui::Event event)> Statistics::fullscreenGenerateFtxuiEventHandler(
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
void Statistics::loopFullScreenLiveStats()
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
void Statistics::printFullScreenLiveStatsGlobalInfo(const LiveResults& liveResults,
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

	const bool isRWMixPhase = (liveResults.newLiveOpsReadMix.numBytesDone ||
		liveResults.newLiveOpsReadMix.numEntriesDone);
	const bool isRWMixThreadsPhase = (isRWMixPhase && progArgs.hasUserSetRWMixReadThreads() );

    VEC2D_SET_AUTOGROW(outHeaderStatsTxtTable, rowIdx, colIdx++, FULLSCREEN_HEADER_TITLE_LATENCY);

    std::ostringstream latStream;

	if(!isRWMixPhase)
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
	{ // rwmix
		latStream <<
			"IO ["
			"wr=" <<
			UnitTk::latencyUsToHumanStr(liveResults.liveLatency.avgIOLatMicroSecsSum) <<
			" "
			"rd=" <<
			UnitTk::latencyUsToHumanStr(liveResults.liveLatency.avgIOLatReadMixMicroSecsSum) <<
			"]";

		if(progArgs.getBenchPathType() == BenchPathType_DIR)
		{
			latStream << "  "; // double space as separator

			if(!isRWMixThreadsPhase)
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
					" ["
					"wr=" <<
					UnitTk::latencyUsToHumanStr(
						liveResults.liveLatency.avgEntriesLatMicroSecsSum) <<
					" "
					"rd=" <<
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
void Statistics::printFullScreenLiveStatsWorkerTable(const LiveResults& liveResults,
    std::vector<StringVec>& statsTableData)
{
	const bool isRWMixPhase = (liveResults.newLiveOpsReadMix.numBytesDone ||
		liveResults.newLiveOpsReadMix.numEntriesDone);
	const bool isRWMixThreadsPhase = (isRWMixPhase && progArgs.hasUserSetRWMixReadThreads() );
    const std::string throughputUnitTitleStr = progArgs.getShowThroughputBase10() ?
        FULLSCREEN_WORKERS_TITLE_THROUGHPUT_BASE10 : FULLSCREEN_WORKERS_TITLE_THROUGHPUT_BASE2;
    const uint64_t throughputDivisor = progArgs.getShowThroughputBase10() ?
        (1000*1000) : (1024*1024);

	const bool showDirStats = progArgs.getShowDirStats() &&
		(progArgs.getBenchPathType() == BenchPathType_DIR) &&
		progArgs.getTreeFilePath().empty() &&
		( (workersSharedData.currentBenchPhase == BenchPhase_CREATEFILES) ||
			(workersSharedData.currentBenchPhase == BenchPhase_READFILES) );

	const bool isNetBenchMode = (progArgs.getBenchMode() == BenchMode_NETBENCH);

    unsigned colIdx = 0; // current column index
    unsigned rowIdx = 0; // current row index

    // print table headline...

    VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++, FULLSCREEN_WORKERS_TITLE_RANK);
    VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++, FULLSCREEN_WORKERS_TITLE_COMPLETION_PCT);
    VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++, FULLSCREEN_WORKERS_TITLE_TRANSFERRED);
    VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++, throughputUnitTitleStr);
    VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++, FULLSCREEN_WORKERS_TITLE_IOPS);

    if(progArgs.getBenchPathType() == BenchPathType_DIR)
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

    std::string totalPercentDone = std::to_string(std::min(liveResults.percentDone, (size_t)100) );

    if(isNetBenchMode)
        totalPercentDone = "-";

    VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++, (isRWMixPhase ? "Write" : "Total") );
    VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++, totalPercentDone);
    VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++,
		std::to_string(liveResults.newLiveOps.numBytesDone / (1024*1024) ) );
    VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++,
		std::to_string(liveResults.liveOpsPerSec.numBytesDone / throughputDivisor) );
    VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++,
		std::to_string(liveResults.liveOpsPerSec.numIOPSDone) );

    if(progArgs.getBenchPathType() == BenchPathType_DIR)
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

    // print rwmix read line for all workers...
    if(isRWMixPhase)
    {
        std::string readPercentDone =
            std::to_string(std::min(liveResults.percentDoneReadMix, (size_t)100) );

        if(isNetBenchMode)
            readPercentDone = "-";

        VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++, "Read");
        VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++, readPercentDone);
        VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++,
            std::to_string(liveResults.newLiveOpsReadMix.numBytesDone / (1024*1024) ) );
        VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++,
            std::to_string(liveResults.liveOpsPerSecReadMix.numBytesDone / throughputDivisor) );
        VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++,
            std::to_string(liveResults.liveOpsPerSecReadMix.numIOPSDone) );

        if(progArgs.getBenchPathType() == BenchPathType_DIR)
        { // add columns for dir mode
            VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++, (isRWMixThreadsPhase ?
                std::to_string(liveResults.newLiveOpsReadMix.numEntriesDone) : "-") );
            VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++, (isRWMixThreadsPhase ?
                std::to_string(liveResults.liveOpsPerSecReadMix.numEntriesDone) : "-") );
        }

        if(showDirStats)
        { // add columns for dir stats in file write/read phase of dir mode
            size_t numFiles = progArgs.getNumFiles();

            VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++, (isRWMixThreadsPhase ?
                SAFE_DIV_BY_ZERO_STR(liveResults.newLiveOpsReadMix.numEntriesDone, numFiles) :
                "-") );
            VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++, (isRWMixThreadsPhase ?
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
		LiveOps workerDone; // total numbers
		LiveOps workerDonePerSec; // difference to last round

		workerVec[i]->getLiveOpsCombined(workerDone);
		workerVec[i]->getAndResetDiffStatsCombined(workerDonePerSec);

		(workerDonePerSec *= 1000) /= progArgs.getLiveStatsSleepMS();

		const char* netbenchServiceSuffixStr = ""; // only set in netbench mode
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

		if(isNetBenchMode)
		{ // special settings for netbench mode
			if(i < progArgs.getNumNetBenchServers() )
			{ // this is a netbench server
				workerPercentDoneStr = "-"; // this is a server, we only have pct done for clients
				netbenchServiceSuffixStr = " [server]";
			}
			else // this is a netbench client
				netbenchServiceSuffixStr = " [client]";
		}

        VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++, std::to_string(i) );
        VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++, workerPercentDoneStr);
        VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++,
			std::to_string(workerDone.numBytesDone / (1024*1024) ) );
        VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++,
            std::to_string(workerDonePerSec.numBytesDone / throughputDivisor) );
        VEC2D_SET_AUTOGROW(statsTableData, rowIdx, colIdx++,
			std::to_string(workerDonePerSec.numIOPSDone) );

        if(progArgs.getBenchPathType() == BenchPathType_DIR)
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
                (progArgs.getHostsVec()[i] + netbenchServiceSuffixStr + pingStr) );
        }

        // new row, reset column
        rowIdx++;
        colIdx = 0;
	}
}

/**
 * Generate footer line for fullscreen live stats view.
 */
 std::string Statistics::printFullScreenLiveStatsFooter()
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
void Statistics::updateLiveStatsRemoteInfo(LiveResults& liveResults)
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
void Statistics::updateLiveStatsLiveOps(LiveResults& liveResults, size_t elapsedMS)
{
    // sanity check (to prevent div by zero): should never happen
    IF_UNLIKELY(!elapsedMS)
    {
        std::string backtraceStr = SignalTk::logBacktrace();
        throw ProgException(__func__ + std::string(" called with elapsedMS=0. ") +
            "This should never happen. "
            "Backtrace: \n" + backtraceStr);
    }

	getLiveOps(liveResults.newLiveOps, liveResults.newLiveOpsReadMix, liveResults.liveLatency);

	liveResults.liveOpsPerSec = liveResults.newLiveOps - liveResults.lastLiveOps;
	(liveResults.liveOpsPerSec *= 1000) /= elapsedMS;

	liveResults.lastLiveOps = liveResults.newLiveOps;

	liveResults.liveOpsPerSecReadMix =
		liveResults.newLiveOpsReadMix - liveResults.lastLiveOpsReadMix;
	(liveResults.liveOpsPerSecReadMix *= 1000) /= elapsedMS;

	liveResults.lastLiveOpsReadMix = liveResults.newLiveOpsReadMix;

	// if we have bytes in this phase, use them for percent done; otherwise use num entries
	if(liveResults.numBytesPerWorker)
	{
		liveResults.percentDone =
			(100 * liveResults.newLiveOps.numBytesDone) /
			(liveResults.numBytesPerWorker * workerVec.size() );
		liveResults.percentDoneReadMix =
			(100 * liveResults.newLiveOpsReadMix.numBytesDone) /
			(liveResults.numBytesPerWorker * workerVec.size() );
	}
	else
	if(liveResults.numEntriesPerWorker)
	{
		liveResults.percentDone =
			(100 * liveResults.newLiveOps.numEntriesDone) /
			(liveResults.numEntriesPerWorker * workerVec.size() );
		liveResults.percentDoneReadMix =
			(100 * liveResults.newLiveOpsReadMix.numEntriesDone) /
			(liveResults.numEntriesPerWorker * workerVec.size() );
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
void Statistics::printLiveStats()
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

		loopFullScreenLiveStats();

		return;
	}

single_line_stats:
    loopSingleLineLiveStats();
}

/**
 * Get current processed entries sum and bytes written/read of all workers.
 *
 * @param outLiveOps entries/ops/bytes done for all workers (does not need to be initialized to 0).
 * @param outLiveRWMixReadOps rwmix mode read entries/ops/bytes done for all workers (does not need
 * 		to be initialized to 0).
 * @param outLiveLatency this method will call divAllByNumValues(), so struct values are avg across
 * 		all workers.
 */
void Statistics::getLiveOps(LiveOps& outLiveOps, LiveOps& outLiveRWMixReadOps,
	LiveLatency& outLiveLatency)
{
	outLiveOps = {}; // set all members to zero
	outLiveRWMixReadOps = {}; // set all members to zero
	outLiveLatency = {}; // set all members to zero

	for(size_t i=0; i < workerVec.size(); i++)
	{
		workerVec[i]->getAndAddLiveOps(outLiveOps, outLiveRWMixReadOps);
		workerVec[i]->getAndAddLiveLatency(outLiveLatency);
	}
}

/**
 * Get live statistics for progress report of RemoteWorkers in service mode.
 */
void Statistics::getLiveStatsAsPropertyTreeForService(bpt::ptree& outTree)
{
	LiveOps liveOps;
	LiveOps liveOpsReadMix;
	LiveLatency liveLatency;

	getLiveOps(liveOps, liveOpsReadMix, liveLatency);

	std::chrono::seconds elapsedDurationSecs =
				std::chrono::duration_cast<std::chrono::seconds>
				(std::chrono::steady_clock::now() - workersSharedData.phaseStartT);
	size_t elapsedSecs = elapsedDurationSecs.count();

    std::unique_lock<std::mutex> lock(workersSharedData.mutex); // L O C K (scoped)

	outTree.put(XFER_STATS_BENCHID, workersSharedData.currentBenchID);
	outTree.put(XFER_STATS_BENCHPHASENAME,
		TranslatorTk::benchPhaseToPhaseName(workersSharedData.currentBenchPhase, &progArgs) );
	outTree.put(XFER_STATS_BENCHPHASECODE, workersSharedData.currentBenchPhase);
	outTree.put(XFER_STATS_NUMWORKERSDONE, workersSharedData.numWorkersDone);
	outTree.put(XFER_STATS_NUMWORKERSDONEWITHERR, workersSharedData.numWorkersDoneWithError);
	outTree.put(XFER_STATS_TRIGGERSTONEWALL,
		!workerVec.empty() && workerVec[0]->getStoneWallTriggered());

    lock.unlock(); // U N L O C K

	outTree.put(XFER_STATS_NUMENTRIESDONE, liveOps.numEntriesDone);
	outTree.put(XFER_STATS_NUMBYTESDONE, liveOps.numBytesDone);
	outTree.put(XFER_STATS_NUMIOPSDONE, liveOps.numIOPSDone);
	outTree.put(XFER_STATS_CPUUTIL, (unsigned) liveCpuUtil.getCPUUtilPercent() );
	outTree.put(XFER_STATS_ELAPSEDSECS, elapsedSecs);
	outTree.put(XFER_STATS_LAT_NUM_IOPS, liveLatency.numAvgIOLatValues);
	outTree.put(XFER_STATS_LAT_SUM_IOPS, liveLatency.avgIOLatMicroSecsSum);
	outTree.put(XFER_STATS_LAT_NUM_ENTRIES, liveLatency.numAvgEntriesLatValues);
	outTree.put(XFER_STATS_LAT_SUM_ENTRIES, liveLatency.avgEntriesLatMicroSecsSum);

	if( (workersSharedData.currentBenchPhase == BenchPhase_CREATEFILES) &&
		(progArgs.getRWMixReadPercent() || progArgs.getNumRWMixReadThreads() ||
			(progArgs.getBenchMode() == BenchMode_NETBENCH) ) )
	{
		outTree.put(XFER_STATS_NUMENTRIESDONE_RWMIXREAD, liveOpsReadMix.numEntriesDone);
		outTree.put(XFER_STATS_NUMBYTESDONE_RWMIXREAD, liveOpsReadMix.numBytesDone);
		outTree.put(XFER_STATS_NUMIOPSDONE_RWMIXREAD, liveOpsReadMix.numIOPSDone);

		outTree.put(XFER_STATS_LAT_NUM_IOPS_RWMIXREAD,
			liveLatency.numAvgIOLatReadMixValues);
		outTree.put(XFER_STATS_LAT_SUM_IOPS_RWMIXREAD,
			liveLatency.avgIOLatReadMixMicroSecsSum);
		outTree.put(XFER_STATS_LAT_NUM_ENTRIES_RWMIXREAD,
			liveLatency.numAvgEntriesLatReadMixValues);
		outTree.put(XFER_STATS_LAT_SUM_ENTRIES_RWMIXREAD,
			liveLatency.avgEntriesLatReadMixMicrosSecsSum);
	}

	outTree.put(XFER_STATS_ERRORHISTORY, LoggerBase::getErrHistory() );
}

/**
 * Print table header for phase results to stdout and also to results file (if specified by user).
 *
 * If a results file is set, current date and command line will also be printed to it.
 */
void Statistics::printPhaseResultsTableHeader()
{
	// print to console
	printPhaseResultsTableHeaderToStream(std::cout);

	// print to human-readable results file (if specified by user)
	if(!progArgs.getResFilePathTXT().empty() )
	{
		std::ofstream fileStream;

		fileStream.open(progArgs.getResFilePathTXT(), std::ofstream::app);

		if(!fileStream)
		{
			std::cerr << "ERROR: Opening results file failed: " << progArgs.getResFilePathTXT() <<
				std::endl;

			return;
		}

		// print current date

		std::time_t currentTime = std::time(NULL);

        struct tm localTimeInfo;
        localtime_r(&currentTime, &localTimeInfo);

		fileStream << "ISO DATE: " << std::put_time(&localTimeInfo, "%FT%T%z") << std::endl;

		// print user-defined label

		if(!progArgs.getBenchLabel().empty() )
			fileStream << "LABEL: " << progArgs.getBenchLabel() << std::endl;

		// print command line

		fileStream << "COMMAND LINE: ";

		for(int i=0; i < progArgs.getProgArgCount(); i++)
        {
            if(!strcmp(progArgs.getProgArgVec()[i], "--" ARG_S3ACCESSSECRET_LONG) )
            { // skip parameter for s3 secret
                i += 1;
                continue;
            }

            fileStream << "\"" << progArgs.getProgArgVec()[i] << "\" ";
        }

		fileStream << std::endl;

		// blank line
		fileStream << std::endl;


		printPhaseResultsTableHeaderToStream(fileStream);
		fileStream.close();

		if(!fileStream)
		{
			std::cerr << "ERROR: Writing to results file failed: " << progArgs.getResFilePathTXT() <<
				std::endl;

			return;
		}
	}

	// print to CSV results file (if specified by user)
	if(!progArgs.getResFilePathCSV().empty() && progArgs.getPrintCSVLabels() &&
		FileTk::checkFileEmpty(progArgs.getResFilePathCSV() ) )
	{
		std::ofstream fileStream;

		fileStream.open(progArgs.getResFilePathCSV(), std::ofstream::app);

		if(!fileStream)
		{
			std::cerr << "ERROR: Opening CSV results file failed: " << progArgs.getResFilePathCSV() <<
				std::endl;

			return;
		}

		StringVec labelsVec;
		StringVec resultsVec;
		PhaseResults zeroResults = {};

		printPhaseStartISODateToStringVec(labelsVec, resultsVec);
		progArgs.getAsStringVec(labelsVec, resultsVec);
		printPhaseResultsToStringVec(zeroResults, labelsVec, resultsVec);

		std::string labelsCSVStr = TranslatorTk::stringVecToString(labelsVec, ",");

		fileStream << labelsCSVStr << std::endl;

		fileStream.close();

		if(!fileStream)
		{
			std::cerr << "ERROR: Writing to CSV results file failed: " <<
				progArgs.getResFilePathTXT() << std::endl;

			return;
		}
	}
}

/**
 * Get ISO date & time to string vector, e.g. to later use it for CSV output.
 */
void Statistics::printPhaseStartISODateToStringVec(StringVec& outLabelsVec,
    StringVec& outResultsVec)
{
    time_t time = std::chrono::system_clock::to_time_t(workersSharedData.phaseStartLocalT);
    auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        workersSharedData.phaseStartLocalT.time_since_epoch()).count() % 1000;

    struct tm localTimeInfo;
    localtime_r(&time, &localTimeInfo);

    std::stringstream dateStream;
    dateStream << std::put_time(&localTimeInfo, "%FT%T") << "."
        << std::setfill('0') << std::setw(3) << milliseconds
        << std::put_time(&localTimeInfo, "%z");

    outLabelsVec.push_back("ISO date");
    outResultsVec.push_back(dateStream.str() );
}

/**
 * Print table header for phase results.
 *
 * @outstream where to print results to.
 */
void Statistics::printPhaseResultsTableHeaderToStream(std::ostream& outStream)
{
	outStream << boost::format(phaseResultsFormatStr)
		% "OPERATION"
		% "RESULT TYPE"
		% ""
		% "FIRST DONE"
		% "LAST DONE"
		<< std::endl;
	outStream << boost::format(phaseResultsFormatStr)
		% "==========="
		% "================"
		% ""
		% "=========="
		% "========="
		<< std::endl;
}

/**
 * Print statistics to console and also to file (if set by user) after all workers completed a
 * phase.
 */
void Statistics::printPhaseResults()
{
	PhaseResults phaseResults = {}; // zero init

    bool genRes = generatePhaseResults(phaseResults);

    if(!genRes)
        std::cout << "Phase: " << TranslatorTk::benchPhaseToPhaseName(
            workersSharedData.currentBenchPhase, &progArgs) << ": "
            "Skipping stats print due to unavailable worker results." << std::endl <<
            PHASERESULTS_CONSOLE_SEPARATOR_LINE << std::endl;
    else
        printPhaseResultsToStream(phaseResults, std::cout); // print to console


    // print to results file (if specified by user)
    if(!progArgs.getResFilePathTXT().empty() )
	{
		std::ofstream fileStream;

		fileStream.open(progArgs.getResFilePathTXT(), std::ofstream::app);

		if(!fileStream)
		{
			std::cerr << "ERROR: Opening results file failed: " << progArgs.getResFilePathTXT() <<
				std::endl;

			return;
		}

		if(!genRes)
			fileStream << "Skipping stats print due to unavailable worker results." << std::endl;
		else
			printPhaseResultsToStream(phaseResults, fileStream);

		fileStream << std::endl;
	}

	// print to results CSV file (if specified by user)
	if(genRes && !progArgs.getResFilePathCSV().empty() )
	{
		std::ofstream fileStream;

		fileStream.open(progArgs.getResFilePathCSV(), std::ofstream::app);

		if(!fileStream)
		{
			std::cerr << "ERROR: Opening results CSV file failed: " << progArgs.getResFilePathCSV() <<
				std::endl;

			return;
		}

        StringVec labelsVec;
        StringVec resultsVec;

        printPhaseStartISODateToStringVec(labelsVec, resultsVec);
        progArgs.getAsStringVec(labelsVec, resultsVec);
        printPhaseResultsToStringVec(phaseResults, labelsVec, resultsVec);

        std::string resultsCSVStr = TranslatorTk::stringVecToString(resultsVec, ",");

        fileStream << resultsCSVStr << std::endl;
	}

    // print to results JSON file (if specified by user)
    if(genRes && !progArgs.getResFilePathJSON().empty() )
        printPhaseResultsAsJSON(phaseResults);
}

/**
 * Gather statistics after all workers completed a phase.
 *
 * @phaseResults will contain gathered phase results if true is returned.
 * @return false if stats couldn't be generated due to unavailable worker results.
 */
bool Statistics::generatePhaseResults(PhaseResults& phaseResults)
{
	// check if results uninitialized. can happen if called in service mode before 1st run.
	IF_UNLIKELY(workerVec.empty() )
		return false;

	// (first worker might not have gotten work, so we can only init while we iterate over workers)
	bool firstAndLastFinishInitialized = false;

	// sum up total values
	for(Worker* worker : workerVec)
	{
		if(worker->getElapsedUSecVec().empty() )
		{
			if(worker->getWorkerGotPhaseWork() )
			{
				ERRLOGGER(Log_NORMAL, "generate phase results: "
					"Worker triggers stonewall, but has no elapsed time. " <<
					"WorkerRank: " << worker->getRank() << std::endl);

				return false;
			}

			// worker doesn't trigger stonewall, so it's ok to not have elapsed time

			LOGGER(Log_DEBUG, "generate phase results: "
				"Worker has no elapsed time, also doesn't trigger stonewall. " <<
				"WorkerRank: " << worker->getRank() << std::endl);

			continue;
		}

		for(uint64_t elapsedUSec : worker->getElapsedUSecVec() )
		{
		    // init first/last elapsed milliseconds
		    if(!firstAndLastFinishInitialized)
            {
		        LOGGER(Log_DEBUG, "gen phase res: init first&last finished. "
		            "WorkerRank: " << worker->getRank() << std::endl);

                phaseResults.firstFinishUSec = elapsedUSec; // stonewall: time to compl of fastest
                phaseResults.lastFinishUSec = elapsedUSec; // time to completion of slowest worker

                firstAndLastFinishInitialized = true;
		        continue;
            }

			if(elapsedUSec < phaseResults.firstFinishUSec)
				phaseResults.firstFinishUSec = elapsedUSec;

			if(elapsedUSec > phaseResults.lastFinishUSec)
				phaseResults.lastFinishUSec = elapsedUSec;
		}

		worker->getAndAddLiveOps(phaseResults.opsTotal, phaseResults.opsTotalReadMix);
		worker->getAndAddStoneWallOps(phaseResults.opsStoneWallTotal,
			phaseResults.opsStoneWallTotalReadMix);
		phaseResults.iopsLatHisto += worker->getIOPSLatencyHistogram();
		phaseResults.iopsLatHistoReadMix += worker->getIOPSLatencyHistogramReadMix();
		phaseResults.entriesLatHisto += worker->getEntriesLatencyHistogram();
		phaseResults.entriesLatHistoReadMix += worker->getEntriesLatencyHistogramReadMix();

	} // end of for loop

	if(!firstAndLastFinishInitialized)
	{
        LOGGER(Log_DEBUG, "gen phase res: No worker provided elapsedUSec." << std::endl);
	    return false;
	}

	// total per sec for all workers by 1st finisher
	if(phaseResults.firstFinishUSec)
	{
		phaseResults.opsStoneWallTotal.getPerSecFromUSec(
			phaseResults.firstFinishUSec, phaseResults.opsStoneWallPerSec);
	}

	// rwmix read total per sec for all workers by 1st finisher
	if(phaseResults.firstFinishUSec && phaseResults.opsTotalReadMix.numIOPSDone)
	{
		phaseResults.opsStoneWallTotalReadMix.getPerSecFromUSec(
			phaseResults.firstFinishUSec, phaseResults.opsStoneWallPerSecReadMix);
	}

	// total per sec for all workers by last finisher
	if(phaseResults.lastFinishUSec)
	{
		phaseResults.opsTotal.getPerSecFromUSec(
			phaseResults.lastFinishUSec, phaseResults.opsPerSec);
	}

	// rwmix read total per sec for all workers by last finisher
	if(phaseResults.lastFinishUSec && phaseResults.opsTotalReadMix.numIOPSDone)
	{
		phaseResults.opsTotalReadMix.getPerSecFromUSec(
			phaseResults.lastFinishUSec, phaseResults.opsPerSecReadMix);
	}

	// cpu utilization
	if(progArgs.getHostsVec().empty() )
	{ // local mode => use vals from workersSharedData
		phaseResults.cpuUtilStoneWallPercent =
			workersSharedData.cpuUtilFirstDone.getCPUUtilPercent();
		phaseResults.cpuUtilPercent =
			workersSharedData.cpuUtilLastDone.getCPUUtilPercent();
	}
	else
	{ // master mode => calc average from remote values
		phaseResults.cpuUtilStoneWallPercent = 0;
		phaseResults.cpuUtilPercent = 0;

		for(Worker* worker : workerVec)
		{
			RemoteWorker* remoteWorker =  static_cast<RemoteWorker*>(worker);
			phaseResults.cpuUtilStoneWallPercent += remoteWorker->getCPUUtilStoneWall();
			phaseResults.cpuUtilPercent += remoteWorker->getCPUUtilLastDone();
		}

		phaseResults.cpuUtilStoneWallPercent /= workerVec.size();
		phaseResults.cpuUtilPercent /= workerVec.size();
	}

	return true;
}

/**
 * Print phase result statistics to given stream.
 *
 * @outStream where to print results to.
 */
void Statistics::printPhaseResultsToStream(const PhaseResults& phaseResults,
	std::ostream& outStream)
{
	std::string phaseName =
		TranslatorTk::benchPhaseToPhaseName(workersSharedData.currentBenchPhase, &progArgs);
	std::string entryTypeUpperCase = TranslatorTk::benchPhaseToPhaseEntryType(
        workersSharedData.currentBenchPhase, &progArgs, true);
	std::string entryTypeLowerCase = TranslatorTk::benchPhaseToPhaseEntryType(
        workersSharedData.currentBenchPhase, &progArgs, false);
    std::string throughputUnitStr = progArgs.getShowThroughputBase10() ? "MB/s" : "MiB/s";
    uint64_t throughputDivisor = progArgs.getShowThroughputBase10() ? (1000*1000) : (1024*1024);

	const bool isRWMixPhase = (phaseResults.opsTotalReadMix.numBytesDone ||
		phaseResults.opsTotalReadMix.numEntriesDone);
	const bool isRWMixThreadsPhase = (isRWMixPhase && progArgs.hasUserSetRWMixReadThreads() );

	const bool showDirStats = progArgs.getShowDirStats() &&
		(progArgs.getBenchPathType() == BenchPathType_DIR) &&
		progArgs.getTreeFilePath().empty() &&
		( (workersSharedData.currentBenchPhase == BenchPhase_CREATEFILES) ||
			(workersSharedData.currentBenchPhase == BenchPhase_READFILES) );

	// elapsed time
	outStream << boost::format(Statistics::phaseResultsFormatStr)
		% phaseName
		% "Elapsed time"
		% ":"
		% UnitTk::elapsedMSToHumanStr(phaseResults.firstFinishUSec / 1000)
		% UnitTk::elapsedMSToHumanStr(phaseResults.lastFinishUSec / 1000)
		<< std::endl;

	// entries (dirs/files) per second
	if(phaseResults.opsTotal.numEntriesDone)
	{
		outStream << boost::format(Statistics::phaseResultsFormatStr)
			% ""
			% (isRWMixThreadsPhase ?
				(entryTypeUpperCase + "/s write") : (entryTypeUpperCase + "/s") )
			% ":"
			% phaseResults.opsStoneWallPerSec.numEntriesDone
			% phaseResults.opsPerSec.numEntriesDone
			<< std::endl;
	}

	// dirs per second in file read/write phase of dir mode
	if(showDirStats && phaseResults.opsTotal.numEntriesDone)
	{
		outStream << boost::format(Statistics::phaseResultsFormatStr)
			% ""
			% (isRWMixThreadsPhase ?
				("Dirs/s write") : ("Dirs/s") )
			% ":"
			% (phaseResults.opsStoneWallPerSec.numEntriesDone / progArgs.getNumFiles() )
			% (phaseResults.opsPerSec.numEntriesDone / progArgs.getNumFiles() )
			<< std::endl;
	}

    // rwmix read entries (dirs/files) per second
    if(phaseResults.opsTotalReadMix.numEntriesDone)
    {
        outStream << boost::format(Statistics::phaseResultsFormatStr)
            % ""
            % (entryTypeUpperCase + "/s read")
            % ":"
            % phaseResults.opsStoneWallPerSecReadMix.numEntriesDone
            % phaseResults.opsPerSecReadMix.numEntriesDone
            << std::endl;

        uint64_t entriesPerSecTotalFirstDone = phaseResults.opsStoneWallPerSec.numEntriesDone +
            phaseResults.opsStoneWallPerSecReadMix.numEntriesDone;
        uint64_t entriesPerSecTotalLastDone = phaseResults.opsPerSec.numEntriesDone +
            phaseResults.opsPerSecReadMix.numEntriesDone;

        outStream << boost::format(Statistics::phaseResultsFormatStr)
            % ""
            % (entryTypeUpperCase + "/s total")
            % ":"
            % entriesPerSecTotalFirstDone
            % entriesPerSecTotalLastDone
            << std::endl;
    }

	// dirs per second in rwmix file read phase of dir mode
	if(showDirStats && phaseResults.opsTotalReadMix.numEntriesDone)
	{
		outStream << boost::format(Statistics::phaseResultsFormatStr)
			% ""
			% ("Dirs/s read")
			% ":"
			% (phaseResults.opsStoneWallPerSecReadMix.numEntriesDone / progArgs.getNumFiles() )
			% (phaseResults.opsPerSecReadMix.numEntriesDone / progArgs.getNumFiles() )
			<< std::endl;
	}

    // IOPS
    if(phaseResults.opsTotal.numIOPSDone)
    {
        /* print iops only if path is bdev/file; or in dir mode when each file consists of more than
            one block read/write (because otherwise iops is equal to files/s) */
        if( (progArgs.getBenchPathType() != BenchPathType_DIR) ||
            (progArgs.getBlockSize() != progArgs.getFileSize() ) ||
            (!phaseResults.opsTotal.numEntriesDone) )
            outStream << boost::format(Statistics::phaseResultsFormatStr)
                % ""
                % (isRWMixPhase ? "IOPS write" : "IOPS")
                % ":"
                % phaseResults.opsStoneWallPerSec.numIOPSDone
                % phaseResults.opsPerSec.numIOPSDone
                << std::endl;
    }

    // IOPS rwmix read
    if(phaseResults.opsTotalReadMix.numIOPSDone)
    {
        /* print iops only if path is bdev/file; or in dir mode when each file consists of more than
            one block read/write (because otherwise iops is equal to files/s) */
        if( (progArgs.getBenchPathType() != BenchPathType_DIR) ||
            (progArgs.getBlockSize() != progArgs.getFileSize() ) ||
            (!phaseResults.opsTotalReadMix.numEntriesDone) )
        {
            outStream << boost::format(Statistics::phaseResultsFormatStr)
                % ""
                % "IOPS read"
                % ":"
                % phaseResults.opsStoneWallPerSecReadMix.numIOPSDone
                % phaseResults.opsPerSecReadMix.numIOPSDone
                << std::endl;

            uint64_t iopsTotalFirstDone = phaseResults.opsStoneWallPerSec.numIOPSDone +
                phaseResults.opsStoneWallPerSecReadMix.numIOPSDone;
            uint64_t iopsTotalLastDone = phaseResults.opsPerSec.numIOPSDone +
                phaseResults.opsPerSecReadMix.numIOPSDone;

            outStream << boost::format(Statistics::phaseResultsFormatStr)
                % ""
                % "IOPS total"
                % ":"
                % iopsTotalFirstDone
                % iopsTotalLastDone
                << std::endl;
        }
    }

    // bytes per second total for all workers
    if(phaseResults.opsTotal.numBytesDone)
    {
        outStream << boost::format(Statistics::phaseResultsFormatStr)
            % ""
            % (isRWMixPhase ? (throughputUnitStr + " write") : ("Throughput " + throughputUnitStr) )
            % ":"
            % (phaseResults.opsStoneWallPerSec.numBytesDone / throughputDivisor)
            % (phaseResults.opsPerSec.numBytesDone / throughputDivisor)
            << std::endl;
    }

    // rwmix read bytes per second total for all workers
    if(phaseResults.opsTotalReadMix.numBytesDone)
    {
        outStream << boost::format(Statistics::phaseResultsFormatStr)
            % ""
            % (throughputUnitStr + " read")
            % ":"
            % (phaseResults.opsStoneWallPerSecReadMix.numBytesDone / throughputDivisor)
            % (phaseResults.opsPerSecReadMix.numBytesDone / throughputDivisor)
            << std::endl;

        uint64_t totalBytesPerSecFirstDone = phaseResults.opsStoneWallPerSec.numBytesDone +
            phaseResults.opsStoneWallPerSecReadMix.numBytesDone;
        uint64_t totalBytesPerSecLastDone = phaseResults.opsPerSec.numBytesDone +
            phaseResults.opsPerSecReadMix.numBytesDone;

        outStream << boost::format(Statistics::phaseResultsFormatStr)
            % ""
            % (throughputUnitStr + " total")
            % ":"
            % (totalBytesPerSecFirstDone / throughputDivisor)
            % (totalBytesPerSecLastDone / throughputDivisor)
            << std::endl;
    }

	// sum of bytes read/written
	if(phaseResults.opsTotal.numBytesDone)
	{
		outStream << boost::format(Statistics::phaseResultsFormatStr)
			% ""
			% (isRWMixPhase ? "MiB write" : "Total MiB")
			% ":"
			% (phaseResults.opsStoneWallTotal.numBytesDone / (1024*1024) )
			% (phaseResults.opsTotal.numBytesDone / (1024*1024) )
			<< std::endl;
	}

	// rwmix read sum of bytes
	if(phaseResults.opsTotalReadMix.numBytesDone)
	{
		outStream << boost::format(Statistics::phaseResultsFormatStr)
			% ""
			% "MiB read"
			% ":"
			% (phaseResults.opsStoneWallTotalReadMix.numBytesDone / (1024*1024) )
			% (phaseResults.opsTotalReadMix.numBytesDone / (1024*1024) )
			<< std::endl;
	}

	// entries (dirs/files) processed
	if(phaseResults.opsTotal.numEntriesDone)
	{
		outStream << boost::format(Statistics::phaseResultsFormatStr)
			% ""
			% (isRWMixThreadsPhase ? entryTypeUpperCase + " write" : entryTypeUpperCase + " total")
			% ":"
			% phaseResults.opsStoneWallTotal.numEntriesDone
			% phaseResults.opsTotal.numEntriesDone
			<< std::endl;
	}

	// dirs processed in file read/write phase of dir mode
	if(showDirStats && phaseResults.opsTotal.numEntriesDone)
	{
		outStream << boost::format(Statistics::phaseResultsFormatStr)
			% ""
			% (isRWMixThreadsPhase ? "Dirs write" : "Dirs total")
			% ":"
			% (phaseResults.opsStoneWallTotal.numEntriesDone / progArgs.getNumFiles() )
			% (phaseResults.opsTotal.numEntriesDone / progArgs.getNumFiles() )
			<< std::endl;
	}

	// rwmix read entries (dirs/files) processed
	if(phaseResults.opsTotalReadMix.numEntriesDone)
	{
		outStream << boost::format(Statistics::phaseResultsFormatStr)
			% ""
			% (entryTypeUpperCase + " read")
			% ":"
			% phaseResults.opsStoneWallTotalReadMix.numEntriesDone
			% phaseResults.opsTotalReadMix.numEntriesDone
			<< std::endl;
	}

	// dis processed in rwmix file read phase of dir mode
	if(showDirStats && phaseResults.opsTotalReadMix.numEntriesDone)
	{
		outStream << boost::format(Statistics::phaseResultsFormatStr)
			% ""
			% ("Dirs read")
			% ":"
			% (phaseResults.opsStoneWallTotalReadMix.numEntriesDone / progArgs.getNumFiles() )
			% (phaseResults.opsTotalReadMix.numEntriesDone / progArgs.getNumFiles() )
			<< std::endl;
	}

	// IOs (number of blocks read/written)
	if(phaseResults.opsTotal.numIOPSDone)
	{
		if(progArgs.getLogLevel() > Log_NORMAL)
			outStream << boost::format(Statistics::phaseResultsFormatStr)
				% ""
				% (isRWMixPhase ? "IOs write" : "IOs total")
				% ":"
				% phaseResults.opsStoneWallTotal.numIOPSDone
				% phaseResults.opsTotal.numIOPSDone
				<< std::endl;
	}

	// rwmix read IOs (number of blocks read)
	if(phaseResults.opsTotalReadMix.numIOPSDone)
	{
		if(progArgs.getLogLevel() > Log_NORMAL)
			outStream << boost::format(Statistics::phaseResultsFormatStr)
				% ""
				% "IOs read"
				% ":"
				% phaseResults.opsStoneWallTotalReadMix.numIOPSDone
				% phaseResults.opsTotalReadMix.numIOPSDone
				<< std::endl;
	}

	// cpu utilization
	if(progArgs.getShowCPUUtilization() )
	{
		outStream << boost::format(Statistics::phaseResultsFormatStr)
			% ""
			% "CPU util %"
			% ":"
			% std::to_string( (unsigned) phaseResults.cpuUtilStoneWallPercent)
			% std::to_string( (unsigned) phaseResults.cpuUtilPercent)
			<< std::endl;
	}

	// print individual elapsed time results for each worker
	if(progArgs.getShowAllElapsed() )
	{
		outStream << boost::format(Statistics::phaseResultsLeftFormatStr)
			% ""
			% "Time ms each"
			% ":";

		outStream << "[ ";

		// print elapsed milliseconds of each I/O thread
		for(Worker* worker : workerVec)
		{
			for(uint64_t elapsedUSec : worker->getElapsedUSecVec() )
				outStream << (elapsedUSec / 1000) << " ";
		}

		outStream << "]" << std::endl;
	}

	// print hosts sorted from fastest to slowest (if this was a distributed run)
	if(progArgs.getShowServicesElapsed() && !progArgs.getHostsVec().empty() )
	{
		outStream << boost::format(Statistics::phaseResultsLeftFormatStr)
			% ""
			% "Svc compl. time"
			% ":";

		outStream << "[ ";

		typedef std::multimap<uint64_t, std::string> CompletionTimeMultiMap;
		typedef CompletionTimeMultiMap::value_type CompletionTimeMultiMapVal;
		CompletionTimeMultiMap completionTimeMap; // key is time ms, val is svc name

		// build map by slowest finisher of each service instance
		for(Worker* worker : workerVec)
		{
			RemoteWorker* remoteWorker = static_cast<RemoteWorker*>(worker);
			uint64_t slowestThreadUSec = 0;

			// find slowest thread of this service instance
			for(uint64_t elapsedUSec : worker->getElapsedUSecVec() )
			{
				if(elapsedUSec > slowestThreadUSec)
					slowestThreadUSec = elapsedUSec;
			}

			completionTimeMap.insert(
				CompletionTimeMultiMapVal(slowestThreadUSec/1000, remoteWorker->getHost() ) );
		}

		// services are now ordered by completion time of their slowest thread through the map

		// print ordered list of services ascending by slowest thread completion time
		for(const CompletionTimeMultiMapVal& mapVal : completionTimeMap)
			outStream << mapVal.second << "=" << UnitTk::elapsedMSToHumanStr(mapVal.first) << " ";

		outStream << "]" << std::endl;
	}

	// entries & iops latency results
	printPhaseResultsLatencyToStream(phaseResults.entriesLatHisto,
		entryTypeUpperCase + (isRWMixThreadsPhase ? " wr" : ""), outStream);
	printPhaseResultsLatencyToStream(phaseResults.entriesLatHistoReadMix,
		entryTypeUpperCase + " rd", outStream);

	printPhaseResultsLatencyToStream(phaseResults.iopsLatHisto,
		"IO" + std::string(isRWMixPhase ? " wr" : ""), outStream);
	printPhaseResultsLatencyToStream(phaseResults.iopsLatHistoReadMix,
		"IO rd", outStream);

	// warn in case of invalid results
	if( (phaseResults.firstFinishUSec == 0) && !progArgs.getIgnore0USecErrors() )
	{
		/* very fast completion, so notify user about possibly useless results (due to accuracy,
			but also because we show 0 op/s to avoid division by 0 */

		outStream << "WARNING: Fastest worker thread completed in less than 1 microsecond, "
			"so results might not be useful (some op/s are shown as 0). You might want to try a "
			"larger data set. Otherwise, option '--" ARG_IGNORE0USECERR_LONG "' disables this "
			"message.)" << std::endl;
	}

	// print horizontal separator between phases
	outStream << PHASERESULTS_CONSOLE_SEPARATOR_LINE << std::endl;
}

/**
 * Print phase result statistics to StringVec, e.g. for the StringVec to be turned into CSV.
 *
 * This can be called with all phaseResults set to 0 if caller is only interested in outLabelsVec.
 */
void Statistics::printPhaseResultsToStringVec(const PhaseResults& phaseResults,
	StringVec& outLabelsVec, StringVec& outResultsVec)
{
	std::string phaseName =
		TranslatorTk::benchPhaseToPhaseName(workersSharedData.currentBenchPhase, &progArgs);

	outLabelsVec.push_back("operation");
	outResultsVec.push_back(phaseName);

	// elapsed time

	outLabelsVec.push_back("time ms [first]");
	outResultsVec.push_back(std::to_string(phaseResults.firstFinishUSec / 1000) );

	outLabelsVec.push_back("time ms [last]");
	outResultsVec.push_back(std::to_string(phaseResults.lastFinishUSec / 1000) );

	// entries per second

	outLabelsVec.push_back("entries/s [first]");
	outResultsVec.push_back(!phaseResults.opsTotal.numEntriesDone ?
		"" : std::to_string(phaseResults.opsStoneWallPerSec.numEntriesDone) );

	outLabelsVec.push_back("entries/s [last]");
	outResultsVec.push_back(!phaseResults.opsTotal.numEntriesDone ?
		"" : std::to_string(phaseResults.opsPerSec.numEntriesDone) );

	// IOPS

	outLabelsVec.push_back("IOPS [first]");
	outResultsVec.push_back(!phaseResults.opsTotal.numIOPSDone ?
		"" : std::to_string(phaseResults.opsStoneWallPerSec.numIOPSDone) );

	outLabelsVec.push_back("IOPS [last]");
	outResultsVec.push_back(!phaseResults.opsTotal.numIOPSDone ?
		"" : std::to_string(phaseResults.opsPerSec.numIOPSDone) );

	// MiB/s

	outLabelsVec.push_back("MiB/s [first]");
	outResultsVec.push_back(!phaseResults.opsTotal.numBytesDone ?
		"" : std::to_string(phaseResults.opsStoneWallPerSec.numBytesDone / (1024*1024) ) );

	outLabelsVec.push_back("MiB/s [last]");
	outResultsVec.push_back(!phaseResults.opsTotal.numBytesDone ?
		"" : std::to_string(phaseResults.opsPerSec.numBytesDone / (1024*1024) ) );

	// cpu utilization

	outLabelsVec.push_back("CPU% [first]");
	outResultsVec.push_back(std::to_string( (unsigned)phaseResults.cpuUtilStoneWallPercent) );

	outLabelsVec.push_back("CPU% [last]");
	outResultsVec.push_back(std::to_string( (unsigned)phaseResults.cpuUtilPercent) );

	// entries

	outLabelsVec.push_back("entries [first]");
	outResultsVec.push_back(!phaseResults.opsTotal.numEntriesDone ?
		"" : std::to_string(phaseResults.opsStoneWallTotal.numEntriesDone) );

	outLabelsVec.push_back("entries [last]");
	outResultsVec.push_back(!phaseResults.opsTotal.numEntriesDone ?
		"" : std::to_string(phaseResults.opsTotal.numEntriesDone) );

	// MiB

	outLabelsVec.push_back("MiB [first]");
	outResultsVec.push_back(!phaseResults.opsTotal.numBytesDone ?
		"" : std::to_string(phaseResults.opsStoneWallTotal.numBytesDone / (1024*1024) ) );

	outLabelsVec.push_back("MiB [last]");
	outResultsVec.push_back(!phaseResults.opsTotal.numBytesDone ?
		"" : std::to_string(phaseResults.opsTotal.numBytesDone / (1024*1024) ) );

	// entries & iops latency results

	printPhaseResultsLatencyToStringVec(phaseResults.entriesLatHisto, "Ent",
		outLabelsVec, outResultsVec);
	printPhaseResultsLatencyToStringVec(phaseResults.iopsLatHisto, "IO",
		outLabelsVec, outResultsVec);

	// rwmix read entries per second

	outLabelsVec.push_back("rwmix read entries/s [first]");
	outResultsVec.push_back(!phaseResults.opsTotalReadMix.numEntriesDone ?
		"" : std::to_string(phaseResults.opsStoneWallPerSecReadMix.numEntriesDone) );

	outLabelsVec.push_back("rwmix read entries/s [last]");
	outResultsVec.push_back(!phaseResults.opsTotalReadMix.numEntriesDone ?
		"" : std::to_string(phaseResults.opsPerSecReadMix.numEntriesDone) );

	// rwmix read IOPS

	outLabelsVec.push_back("rwmix read IOPS [first]");
	outResultsVec.push_back(!phaseResults.opsTotalReadMix.numIOPSDone ?
		"" : std::to_string(phaseResults.opsStoneWallPerSecReadMix.numIOPSDone) );

	outLabelsVec.push_back("rwmix read IOPS [last]");
	outResultsVec.push_back(!phaseResults.opsTotalReadMix.numIOPSDone ?
		"" : std::to_string(phaseResults.opsPerSecReadMix.numIOPSDone) );

	// rwmix read MiB/s

	outLabelsVec.push_back("rwmix read MiB/s [first]");
	outResultsVec.push_back(!phaseResults.opsTotalReadMix.numBytesDone ?
		"" : std::to_string(phaseResults.opsStoneWallPerSecReadMix.numBytesDone / (1024*1024) ) );

	outLabelsVec.push_back("rwmix read MiB/s [last]");
	outResultsVec.push_back(!phaseResults.opsTotalReadMix.numBytesDone ?
		"" : std::to_string(phaseResults.opsPerSecReadMix.numBytesDone / (1024*1024) ) );

	// rwmix read entries

	outLabelsVec.push_back("rwmix read entries [first]");
	outResultsVec.push_back(!phaseResults.opsTotalReadMix.numEntriesDone ?
		"" : std::to_string(phaseResults.opsStoneWallTotalReadMix.numEntriesDone) );

	outLabelsVec.push_back("rwmix read entries [last]");
	outResultsVec.push_back(!phaseResults.opsTotalReadMix.numEntriesDone ?
		"" : std::to_string(phaseResults.opsTotalReadMix.numEntriesDone) );

	// rwmix read MiB

	outLabelsVec.push_back("rwmix read MiB [first]");
	outResultsVec.push_back(!phaseResults.opsTotalReadMix.numBytesDone ?
		"" : std::to_string(phaseResults.opsStoneWallTotalReadMix.numBytesDone / (1024*1024) ) );

	outLabelsVec.push_back("rwmix read MiB [last]");
	outResultsVec.push_back(!phaseResults.opsTotalReadMix.numBytesDone ?
		"" : std::to_string(phaseResults.opsTotalReadMix.numBytesDone / (1024*1024) ) );

	// rwmix read entries & iops latency results

	printPhaseResultsLatencyToStringVec(phaseResults.entriesLatHistoReadMix, "rwmix read Ent",
		outLabelsVec, outResultsVec);
	printPhaseResultsLatencyToStringVec(phaseResults.iopsLatHistoReadMix, "rwmix read IO",
		outLabelsVec, outResultsVec);

	// elbencho version

	outLabelsVec.push_back("version");
	outResultsVec.push_back(EXE_VERSION);

	// command line

	std::ostringstream cmdStream;
	std::string cmdString;

    for(int i=0; i < progArgs.getProgArgCount(); i++)
    {
        if(!strcmp(progArgs.getProgArgVec()[i], "--" ARG_S3ACCESSSECRET_LONG) )
        { // skip over s3 secret
            i += 1;
            continue;
        }

        cmdStream << "\"" << progArgs.getProgArgVec()[i] << "\" ";
    }

	cmdString = cmdStream.str();
	std::replace(cmdString.begin(), cmdString.end(), ',', ' '); // replace all commas with spaces

	outLabelsVec.push_back("command");
	outResultsVec.push_back(cmdString);
}

/**
 * Print latency results as sub-task of printPhaseResults().
 *
 * @latHisto the latency histogram for which to print the results.
 * @latTypeStr short latency type description, e.g. for entries or iops.
 * @outstream where to print results to.
 */
void Statistics::printPhaseResultsLatencyToStream(const LatencyHistogram& latHisto,
	std::string latTypeStr, std::ostream& outStream)
{
	std::string phaseName =
		TranslatorTk::benchPhaseToPhaseName(workersSharedData.currentBenchPhase, &progArgs);

	// print IO latency min/avg/max
	if(progArgs.getShowLatency() && latHisto.getNumStoredValues() )
	{
		// individual results header (note: keep format in sync with general table format string)
		outStream << boost::format(Statistics::phaseResultsLeftFormatStr)
			% ""
			% (latTypeStr + " latency")
			% ":";

		outStream <<
			"[ " <<
			"min=" << UnitTk::latencyUsToHumanStr(latHisto.getMinMicroSecLat() ) << " "
			"avg=" << UnitTk::latencyUsToHumanStr(latHisto.getAverageMicroSec() ) << " "
			"max=" << UnitTk::latencyUsToHumanStr(latHisto.getMaxMicroSecLat() ) <<
			" ]" << std::endl;
	}

	// print IO latency percentiles
	if(progArgs.getShowLatencyPercentiles() && latHisto.getNumStoredValues() )
	{
		// individual results header (note: keep format in sync with general table format string)
		outStream << boost::format(Statistics::phaseResultsLeftFormatStr)
			% ""
			% (latTypeStr + " lat % us")
			% ":";

		outStream << "[ ";

		if(latHisto.getHistogramExceeded() )
			outStream << "Histogram exceeded";
		else
		{
			outStream <<
				"1%<=" << latHisto.getPercentileStr(1) << " "
				"50%<=" << latHisto.getPercentileStr(50) << " "
				"75%<=" << latHisto.getPercentileStr(75) << " "
				"99%<=" << latHisto.getPercentileStr(99);

			// print configurable amount of percentile nines (99.9%, 99.99%, ...)
			std::string ninesStr = "99.";
			for(unsigned short numDecimals=1;
				numDecimals <= progArgs.getNumLatencyPercentile9s();
				numDecimals++)
			{
				ninesStr += "9"; // append next decimal 9
				double percentage = std::stod(ninesStr);

				outStream << " " << std::setprecision(numDecimals+3) << // +3 for "99."
					percentage << "%<=" << latHisto.getPercentileStr(percentage);
			}
		}

		outStream << " ]" << std::endl;
	}

	// print IO latency histogram
	if(progArgs.getShowLatencyHistogram() && latHisto.getNumStoredValues() )
	{
		// individual results header (note: keep format in sync with general table format string)
		outStream << boost::format(Statistics::phaseResultsLeftFormatStr)
			% ""
			% (latTypeStr + " lat hist")
			% ":";

		outStream << "[ ";

		outStream << latHisto.getHistogramStr();

		outStream << " ]" << std::endl;
	}
}

/**
 * Print latency results to StringVec, e.g. for the StringVec to be turned into CSV.
 *
 * This can be called with all latHisto set to 0 if caller is only interested in outLabelsVec.
 *
 * @latHisto the latency histogram for which to print the results.
 * @latTypeStr short latency type description, e.g. for entries or iops.
 */
void Statistics::printPhaseResultsLatencyToStringVec(const LatencyHistogram& latHisto,
	std::string latTypeStr, StringVec& outLabelsVec, StringVec& outResultsVec)
{
	// latency min/avg/max

	outLabelsVec.push_back(latTypeStr + " lat us [min]");
	outResultsVec.push_back(!latHisto.getNumStoredValues() ?
		"" : std::to_string(latHisto.getMinMicroSecLat() ) );

	outLabelsVec.push_back(latTypeStr + " lat us [avg]");
	outResultsVec.push_back(!latHisto.getNumStoredValues() ?
		"" : std::to_string(latHisto.getAverageMicroSec() ) );

	outLabelsVec.push_back(latTypeStr + " lat us [max]");
	outResultsVec.push_back(!latHisto.getNumStoredValues() ?
		"" : std::to_string(latHisto.getMaxMicroSecLat() ) );
}

void Statistics::printPhaseResultsAsJSON(const PhaseResults& phaseResults)
{
    bpt::ptree ptree;

    bpt::ptree configSubtree;

    bpt::ptree firstDoneSubtree;
    bpt::ptree firstDoneSubtreeReadMix;
    bpt::ptree lastDoneSubtree;
    bpt::ptree lastDoneSubtreeReadMix;

    bpt::ptree lastDoneLatencySubtree;
    bpt::ptree entriesLatencySubtree;
    bpt::ptree entriesLatencySubtreeReadMix;
    bpt::ptree iopsLatencySubtree;
    bpt::ptree iopsLatencySubtreeReadMix;

    std::string phaseName =
        TranslatorTk::benchPhaseToPhaseName(workersSharedData.currentBenchPhase, &progArgs);

    // phase name

    ptree.put("phase_type", phaseName);

    // bench ID

    ptree.put("phase_id", workersSharedData.currentBenchID);

    // label

    if(!progArgs.getBenchLabel().empty() )
        ptree.put("label", progArgs.getBenchLabel() );

    // start time

    time_t time = std::chrono::system_clock::to_time_t(workersSharedData.phaseStartLocalT);
    auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        workersSharedData.phaseStartLocalT.time_since_epoch()).count() % 1000;

    struct tm localTimeInfo;
    localtime_r(&time, &localTimeInfo);

    std::stringstream dateStream;
    dateStream << std::put_time(&localTimeInfo, "%FT%T") << "."
        << std::setfill('0') << std::setw(3) << milliseconds
        << std::put_time(&localTimeInfo, "%z");

    ptree.put("iso_start_date", dateStream.str() );

    // config subtree...

    configSubtree.put("path_type", TranslatorTk::benchPathTypeToStr(
        progArgs.getBenchPathType(), &progArgs) );

    configSubtree.put("paths", progArgs.getBenchPaths().size() );

    configSubtree.put("hosts", progArgs.getHostsVec().empty() ?
        1 :  progArgs.getHostsVec().size() );

    configSubtree.put("threads", progArgs.getNumThreads() );

    if(progArgs.getBenchPathType() == BenchPathType_DIR)
    {
        if(progArgs.getTreeFilePath().empty() )
        {
            configSubtree.put("dirs", progArgs.getNumDirs() );
            configSubtree.put("files", progArgs.getNumFiles() );
        }
        else
        { // custom tree
            configSubtree.put("custom_tree_dirs",
                progArgs.getCustomTreeDirs().getNumPaths() );
            configSubtree.put("custom_tree_files_shared",
                progArgs.getCustomTreeFilesShared().getNumPaths() );
            configSubtree.put("files_not_shared",
                progArgs.getCustomTreeFilesNonShared().getNumPaths() );
        }
    }

    configSubtree.put("file_size", progArgs.getFileSize() );
    configSubtree.put("block_size", progArgs.getBlockSize() );
    configSubtree.put("direct_io", progArgs.getUseDirectIO() );
    configSubtree.put("random_offsets", progArgs.getUseRandomOffsets() );

    if(progArgs.getUseRandomOffsets() )
        configSubtree.put("random_aligned", !progArgs.getUseRandomUnaligned() );

    configSubtree.put("io_depth", progArgs.getIODepth() );

    if(!progArgs.getHostsVec().empty() )
        configSubtree.put("shared_service_paths", progArgs.getIsServicePathShared() );

    if( (progArgs.getBenchMode() != BenchMode_S3) &&
        (progArgs.getBenchPathType() != BenchPathType_BLOCKDEV) )
        configSubtree.put("truncate_files", progArgs.getDoTruncate() );

    // elbencho version

    configSubtree.put("version", EXE_VERSION);

    // command line

    std::ostringstream cmdStream;
    std::string cmdString;

    for(int i=0; i < progArgs.getProgArgCount(); i++)
    {
        if(!strcmp(progArgs.getProgArgVec()[i], "--" ARG_S3ACCESSSECRET_LONG) )
        { // skip over s3 secret
            i += 1;
            continue;
        }

        cmdStream << "\"" << progArgs.getProgArgVec()[i] << "\" ";
    }

    cmdString = cmdStream.str();
    std::replace(cmdString.begin(), cmdString.end(), ',', ' '); // replace all commas with spaces

    configSubtree.put("command", cmdString);


    // elasped time (first done & last done)

    firstDoneSubtree.put("elapsed_time_ms", phaseResults.firstFinishUSec / 1000);
    lastDoneSubtree.put("elapsed_time_ms", phaseResults.lastFinishUSec / 1000);

    // lambda to fill first done & last done subtrees with per-second values
    std::function addPerSecResultsToSubtree = [](const LiveOps& opsTotal,
        const LiveOps& inOpsPerSec, bpt::ptree& outTree)
    {
        // entries per second

        if(opsTotal.numEntriesDone)
            outTree.put("entries/s", inOpsPerSec.numEntriesDone);

        // IOPS

        if(opsTotal.numIOPSDone)
            outTree.put("iops", inOpsPerSec.numIOPSDone);

        // Bytes/s

        if(opsTotal.numBytesDone)
            outTree.put("bytes/s", inOpsPerSec.numBytesDone);

    }; // end of addPerSecResultsToSubtree lambda

    // lambda to fill first done & last done subtrees with absolute/total values
    std::function addTotalResultsToSubtree = [](const LiveOps& opsTotal,
        const LiveOps& inOpsTotal, bpt::ptree& outTree)
    {
        // entries

        if(opsTotal.numEntriesDone)
            outTree.put("entries", inOpsTotal.numEntriesDone);

        // Bytes transferred

        if(opsTotal.numBytesDone)
            outTree.put("bytes", inOpsTotal.numBytesDone);

    }; // end of addTotalResultsToSubtree lambda

    // first done subtree per-sec counters

    addPerSecResultsToSubtree(phaseResults.opsTotal,
        phaseResults.opsStoneWallPerSec, firstDoneSubtree);
    addPerSecResultsToSubtree(phaseResults.opsTotalReadMix,
        phaseResults.opsStoneWallPerSecReadMix, firstDoneSubtreeReadMix);

    // first done subtree total counters

    addTotalResultsToSubtree(phaseResults.opsTotal,
        phaseResults.opsStoneWallTotal, firstDoneSubtree);
    addTotalResultsToSubtree(phaseResults.opsTotalReadMix,
        phaseResults.opsStoneWallTotalReadMix, firstDoneSubtreeReadMix);

    // last done subtree per-sec counters

    addPerSecResultsToSubtree(phaseResults.opsTotal,
        phaseResults.opsPerSec, lastDoneSubtree);
    addPerSecResultsToSubtree(phaseResults.opsTotalReadMix,
        phaseResults.opsPerSecReadMix, lastDoneSubtreeReadMix);

    // last done subtree total counters

    addTotalResultsToSubtree(phaseResults.opsTotal,
        phaseResults.opsTotal, lastDoneSubtree);
    addTotalResultsToSubtree(phaseResults.opsTotalReadMix,
        phaseResults.opsTotalReadMix, lastDoneSubtreeReadMix);

    // copy rwmix read subtrees into first & last done subtrees

    if(firstDoneSubtreeReadMix.size() )
        firstDoneSubtree.put_child("rwmix_read", firstDoneSubtreeReadMix);

    if(lastDoneSubtreeReadMix.size() )
        lastDoneSubtree.put_child("rwmix_read", lastDoneSubtreeReadMix);

    // cpu utilization

    firstDoneSubtree.put("cpu%", (unsigned)phaseResults.cpuUtilStoneWallPercent);
    lastDoneSubtree.put("cpu%", (unsigned)phaseResults.cpuUtilPercent);

    // entries & iops latency results

    // lambda to fill latency
    std::function addLatencyResultsToSubtree = [&progArgs = progArgs](
        const LatencyHistogram& latHisto, bpt::ptree& outTree)
    {
        if(!progArgs.getShowLatency() || !latHisto.getNumStoredValues() )
            return;

        outTree.put("min_us", latHisto.getMinMicroSecLat() );
        outTree.put("avg_us", latHisto.getAverageMicroSec() );
        outTree.put("max_us", latHisto.getMaxMicroSecLat() );
    }; // end of addLatencyResultsToSubtree lambda

    addLatencyResultsToSubtree(phaseResults.entriesLatHisto, entriesLatencySubtree);
    addLatencyResultsToSubtree(phaseResults.entriesLatHistoReadMix, entriesLatencySubtreeReadMix);
    addLatencyResultsToSubtree(phaseResults.iopsLatHisto, iopsLatencySubtree);
    addLatencyResultsToSubtree(phaseResults.iopsLatHistoReadMix, iopsLatencySubtreeReadMix);

    // copy latency subtrees into main tree

    if(entriesLatencySubtreeReadMix.size() )
        entriesLatencySubtree.put_child("rwmix_read", entriesLatencySubtreeReadMix);

    if(iopsLatencySubtreeReadMix.size() )
        iopsLatencySubtree.put_child("rwmix_read", iopsLatencySubtreeReadMix);

    if(entriesLatencySubtree.size() )
        lastDoneLatencySubtree.put_child("entries", entriesLatencySubtree);

    if(iopsLatencySubtree.size() )
        lastDoneLatencySubtree.put_child("IO", iopsLatencySubtree);

    // latency histograms

    if(progArgs.getShowLatencyHistogram() )
    {
        if(phaseResults.entriesLatHisto.getNumStoredValues() )
            phaseResults.entriesLatHisto.getAsPropertyTreeForJSONFile(lastDoneLatencySubtree,
                "entries.histogram");

        if(phaseResults.entriesLatHistoReadMix.getNumStoredValues() )
            phaseResults.entriesLatHistoReadMix.getAsPropertyTreeForJSONFile(lastDoneLatencySubtree,
                "entries.histogram.rwmix_read");

        if(phaseResults.iopsLatHisto.getNumStoredValues() )
            phaseResults.iopsLatHisto.getAsPropertyTreeForJSONFile(lastDoneLatencySubtree,
                "IO.histogram");

        if(phaseResults.iopsLatHistoReadMix.getNumStoredValues() )
            phaseResults.iopsLatHistoReadMix.getAsPropertyTreeForJSONFile(lastDoneLatencySubtree,
                "IO.histogram.rwmix_read");
    }

    if(lastDoneLatencySubtree.size() )
        lastDoneSubtree.put_child("latency", lastDoneLatencySubtree);


    // copy first level subtrees into main tree

    ptree.put_child("config", configSubtree);
    ptree.put_child("first_done", firstDoneSubtree);
    ptree.put_child("last_done", lastDoneSubtree);


    // print json

    try
    {
        std::ofstream fileStream;

        fileStream.open(progArgs.getResFilePathJSON(), std::ofstream::app);

        if(!fileStream)
        {
            std::cerr << "ERROR: Opening results JSON file failed: " << progArgs.getResFilePathJSON() <<
                std::endl;

            return;
        }

        bpt::json_parser::write_json(fileStream, ptree, false); // throws on error
    }
    catch(const std::exception& e)
    {
        throw ProgException("Error writing JSON result file. "
            "File: " + progArgs.getResFilePathJSON() + "; " +
            "Error: " + e.what() );
    }
}

/**
 * Get results of a completed benchmark phase.
 */
void Statistics::getBenchResultAsPropertyTreeForService(bpt::ptree& outTree)
{
	LiveOps liveOps;
	LiveOps liveOpsReadMix;
	LiveLatency liveLatency;
	LatencyHistogram iopsLatHisto; // sum of all histograms
	LatencyHistogram iopsLatHistoReadMix; // sum of all histograms
	LatencyHistogram entriesLatHisto; // sum of all histograms
	LatencyHistogram entriesLatHistoReadMix; // sum of all histograms

	getLiveOps(liveOps, liveOpsReadMix, liveLatency);

    std::unique_lock<std::mutex> lock(workersSharedData.mutex); // L O C K

	outTree.put(XFER_STATS_BENCHID, workersSharedData.currentBenchID);
	outTree.put(XFER_STATS_BENCHPHASENAME,
		TranslatorTk::benchPhaseToPhaseName(workersSharedData.currentBenchPhase, &progArgs) );
	outTree.put(XFER_STATS_BENCHPHASECODE, workersSharedData.currentBenchPhase);
	outTree.put(XFER_STATS_NUMWORKERSDONE, workersSharedData.numWorkersDone);
	outTree.put(XFER_STATS_NUMWORKERSDONEWITHERR, workersSharedData.numWorkersDoneWithError);

    lock.unlock(); // U N L O C K

	outTree.put(XFER_STATS_NUMENTRIESDONE, liveOps.numEntriesDone);
	outTree.put(XFER_STATS_NUMBYTESDONE, liveOps.numBytesDone);
	outTree.put(XFER_STATS_NUMIOPSDONE, liveOps.numIOPSDone);
	outTree.put(XFER_STATS_CPUUTIL_STONEWALL,
		(unsigned) workersSharedData.cpuUtilFirstDone.getCPUUtilPercent() );
	outTree.put(XFER_STATS_CPUUTIL,
		(unsigned) workersSharedData.cpuUtilLastDone.getCPUUtilPercent() );

	bool triggerStonewall = false; // true if at least one worker has this set to true

	for(Worker* worker : workerVec)
	{
		bool workerTriggersStonewall = worker->getWorkerGotPhaseWork();

		if(workerTriggersStonewall)
		{
			triggerStonewall = true;

			// add finishElapsedUSec of each worker
			for(uint64_t elapsedUSec : worker->getElapsedUSecVec() )
				outTree.add(XFER_STATS_ELAPSEDUSECLIST_ITEM, elapsedUSec);
		}

		iopsLatHisto += worker->getIOPSLatencyHistogram();
		entriesLatHisto += worker->getEntriesLatencyHistogram();

		if( (workersSharedData.currentBenchPhase == BenchPhase_CREATEFILES) &&
			(progArgs.getRWMixReadPercent() || progArgs.getNumRWMixReadThreads() ||
				(progArgs.getBenchMode() == BenchMode_NETBENCH) ) )
		{
			iopsLatHistoReadMix += worker->getIOPSLatencyHistogramReadMix();
			entriesLatHistoReadMix += worker->getEntriesLatencyHistogramReadMix();
		}
	}

	outTree.put(XFER_STATS_TRIGGERSTONEWALL, triggerStonewall);

	iopsLatHisto.getAsPropertyTreeForService(outTree, XFER_STATS_LAT_PREFIX_IOPS);
	entriesLatHisto.getAsPropertyTreeForService(outTree, XFER_STATS_LAT_PREFIX_ENTRIES);

	if( (workersSharedData.currentBenchPhase == BenchPhase_CREATEFILES) &&
		(progArgs.getRWMixReadPercent() || progArgs.getNumRWMixReadThreads() ||
			(progArgs.getBenchMode() == BenchMode_NETBENCH) ) )
	{
		outTree.put(XFER_STATS_NUMENTRIESDONE_RWMIXREAD, liveOpsReadMix.numEntriesDone);
		outTree.put(XFER_STATS_NUMBYTESDONE_RWMIXREAD, liveOpsReadMix.numBytesDone);
		outTree.put(XFER_STATS_NUMIOPSDONE_RWMIXREAD, liveOpsReadMix.numIOPSDone);

		iopsLatHistoReadMix.getAsPropertyTreeForService(outTree, XFER_STATS_LAT_PREFIX_IOPS_RWMIXREAD);
		entriesLatHistoReadMix.getAsPropertyTreeForService(outTree, XFER_STATS_LAT_PREFIX_ENTRIES_RWMIXREAD);
	}

	outTree.put(XFER_STATS_ERRORHISTORY, LoggerBase::getErrHistory() );
}

/**
 * Print dry run info (number of entries and dataset size) for each of the user-given phases.
 */
void Statistics::printDryRunInfo()
{
	if(progArgs.getBenchMode() == BenchMode_NETBENCH)
	{
		printDryRunInfoNetBench();
		return;
	}

	if(progArgs.getRunCreateDirsPhase() )
		printDryRunPhaseInfo(BenchPhase_CREATEDIRS);

	if(progArgs.getRunDeleteDirsPhase() )
		printDryRunPhaseInfo(BenchPhase_DELETEDIRS);

	if(progArgs.getRunCreateFilesPhase() )
		printDryRunPhaseInfo(BenchPhase_CREATEFILES);

	if(progArgs.getRunReadPhase() )
		printDryRunPhaseInfo(BenchPhase_READFILES);

	if(progArgs.getRunDeleteFilesPhase() )
		printDryRunPhaseInfo(BenchPhase_DELETEFILES);

	if(progArgs.getRunStatFilesPhase() )
		printDryRunPhaseInfo(BenchPhase_STATFILES);

	if(progArgs.getRunS3AclPut() )
		printDryRunPhaseInfo(BenchPhase_PUTOBJACL);

	if(progArgs.getRunS3AclGet() )
		printDryRunPhaseInfo(BenchPhase_GETOBJACL);

	if(progArgs.getRunS3BucketAclPut() )
		printDryRunPhaseInfo(BenchPhase_PUTBUCKETACL);

	if(progArgs.getRunS3BucketAclGet() )
		printDryRunPhaseInfo(BenchPhase_GETBUCKETACL);
}

/**
 * Print dry run info (num entries and dataset size) for a particular phase.
 */
void Statistics::printDryRunPhaseInfo(BenchPhase benchPhase)
{
	size_t numEntriesPerThread;
	uint64_t numBytesPerThread;

	WorkerManager::getPhaseNumEntriesAndBytes(progArgs, benchPhase, progArgs.getBenchPathType(),
		numEntriesPerThread, numBytesPerThread);

	std::string perUnitStr = progArgs.getHostsVec().empty() ?
		"thread" : "service";

	uint64_t totalMultiplier = progArgs.getHostsVec().empty() ?
		progArgs.getNumThreads() : progArgs.getHostsVec().size();

	uint64_t numEntriesTotal = numEntriesPerThread * totalMultiplier;
	uint64_t numBytesTotal = numBytesPerThread * totalMultiplier;

	std::string benchPhaseStr = TranslatorTk::benchPhaseToPhaseName(benchPhase, &progArgs);

	std::cout << "Phase: " << benchPhaseStr << std::endl;
	std::cout << "* Entries per " << perUnitStr << ": " << numEntriesPerThread << " | " <<
		(numEntriesPerThread / 1000) << " K" " | " <<
		(numEntriesPerThread / (1000*1000) ) << " M" << std::endl;
	std::cout << "* Entries total:      " << numEntriesTotal << " | " <<
		(numEntriesTotal / 1000) << " K" " | " <<
		(numEntriesTotal / (1000*1000) ) << " M" << std::endl;

	// show bytes info only for create/write & read phases
	if( (benchPhase != BenchPhase_CREATEFILES) && (benchPhase != BenchPhase_READFILES) )
		return;

	std::cout << "* Bytes per " << perUnitStr << ":   " << numBytesPerThread << " | " <<
		(numBytesPerThread / (1024*1024) ) << " MiB" " | " <<
		(numBytesPerThread / (1024*1024*1024) ) << " GiB" << std::endl;
	std::cout << "* Bytes total:        " << numBytesTotal << " | " <<
		(numBytesTotal / (1024*1024) ) << " MiB" " | " <<
		(numBytesTotal / (1024*1024*1024) ) << " GiB" << std::endl;
}

/**
 * Print dry run info for netbench mode.
 *
 * Note: Keep this in sync with WorkerManager::getPhaseNumEntriesAndBytes()
 */
void Statistics::printDryRunInfoNetBench()
{
	uint64_t numBytesSendPerClientThread = progArgs.getFileSize();
	uint64_t numBytesSendPerClient = progArgs.getNumThreads() * numBytesSendPerClientThread;
	uint64_t numBytesSendTotal =
		(progArgs.getHostsVec().size() - progArgs.getNumNetBenchServers() ) * numBytesSendPerClient;

	size_t numBlocks = progArgs.getFileSize() / progArgs.getBlockSize();

	uint64_t numBytesRecvPerClientThread = numBlocks * progArgs.getNetBenchRespSize();
	uint64_t numBytesRecvPerClient = progArgs.getNumThreads() * numBytesRecvPerClientThread;
	uint64_t numBytesRecvTotal =
		(progArgs.getHostsVec().size() - progArgs.getNumNetBenchServers() ) * numBytesRecvPerClient;

	std::string benchPhaseStr =
		TranslatorTk::benchPhaseToPhaseName(BenchPhase_CREATEFILES, &progArgs);

	std::cout << "* Bytes per client thread:" << std::endl;
	std::cout << "  * Send:   " << numBytesSendPerClientThread << " | " <<
		(numBytesSendPerClientThread / (1024*1024) ) << " MiB" " | " <<
		(numBytesSendPerClientThread / (1024*1024*1024) ) << " GiB" << std::endl;
	std::cout << "  * Recv:   " << numBytesRecvPerClientThread << " | " <<
		(numBytesRecvPerClientThread / (1024*1024) ) << " MiB" " | " <<
		(numBytesRecvPerClientThread / (1024*1024*1024) ) << " GiB" << std::endl;

	std::cout << "* Bytes per client service:" << std::endl;
	std::cout << "  * Send:    " << numBytesSendPerClient << " | " <<
		(numBytesSendPerClient / (1024*1024) ) << " MiB" " | " <<
		(numBytesSendPerClient / (1024*1024*1024) ) << " GiB" << std::endl;
	std::cout << "  * Recv:    " << numBytesRecvPerClient << " | " <<
		(numBytesRecvPerClient / (1024*1024) ) << " MiB" " | " <<
		(numBytesRecvPerClient / (1024*1024*1024) ) << " GiB" << std::endl;

	std::cout << "* Bytes total for all clients:" << std::endl;
	std::cout << "  * Send:    " << numBytesSendTotal << " | " <<
		(numBytesSendTotal / (1024*1024) ) << " MiB" " | " <<
		(numBytesSendTotal / (1024*1024*1024) ) << " GiB" << std::endl;
	std::cout << "  * Recv:    " << numBytesRecvTotal << " | " <<
		(numBytesRecvTotal / (1024*1024) ) << " MiB" " | " <<
		(numBytesRecvTotal / (1024*1024*1024) ) << " GiB" << std::endl;
}


/**
 * Open and prepare csv file for live statistics.
 * This is  a no-op if user didn't set a live stats csv file.
 *
 * @throw ProgException on error, e.g. unable to open file.
 */
void Statistics::prepLiveCSVFile()
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
void Statistics::printLiveStatsCSV(const LiveResults& liveResults)
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

	const bool isRWMixPhase = (liveResults.newLiveOpsReadMix.numBytesDone ||
		liveResults.newLiveOpsReadMix.numEntriesDone);
	const bool isRWMixThreadsPhase = (isRWMixPhase && progArgs.hasUserSetRWMixReadThreads() );

	bool isDirBenchPath = (progArgs.getBenchPathType() == BenchPathType_DIR);
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
bool Statistics::checkIfVec2DLeftElemEquals(std::vector<StringVec>& vec, int row, int column,
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