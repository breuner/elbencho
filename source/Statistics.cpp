#include <boost/format.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <chrono>
#include "ProgException.h"
#include "Statistics.h"
#include "toolkits/FileTk.h"
#include "toolkits/TerminalTk.h"
#include "toolkits/TranslatorTk.h"
#include "workers/RemoteWorker.h"
#include "workers/Worker.h"
#include "workers/WorkerException.h"

#ifdef NCURSES_SUPPORT
	#include <ncurses.h>
	#undef OK // defined by ncurses.h, but conflicts with AWS SDK using OK in enum in HttpResponse.h
#endif

#define FULLSCREEN_GLOBALINFO_NUMLINES		1 // lines for global info (to calc per-worker lines)


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
		"Waiting for start time: %Lus", (unsigned long long)waittimeSec);
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
				liveResults.liveOpsPerSec.numBytesDone / (1024*1024) << " MiB/s; ";
		else
			stream <<
				"wr=[" <<
					liveResults.liveOpsPerSec.numEntriesDone << " " <<
						liveResults.phaseEntryType << "/s; " <<
					liveResults.liveOpsPerSec.numBytesDone / (1024*1024) << " MiB/s" <<
				"] " <<
				"rd=[" <<
					(isRWMixThreadsPhase ?
						std::to_string(liveResults.liveOpsPerSecReadMix.numEntriesDone) + " " +
							liveResults.phaseEntryType + "/s; " : "") <<
					liveResults.liveOpsPerSecReadMix.numBytesDone / (1024*1024) << " MiB/s" <<
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
				liveResults.liveOpsPerSec.numBytesDone / (1024*1024) << " MiB/s; " <<
				liveResults.newLiveOps.numBytesDone / (1024*1024) << " MiB; ";
		else
			stream <<
				"wr=[" <<
					liveResults.liveOpsPerSec.numIOPSDone << " " << "IOPS; " <<
					liveResults.liveOpsPerSec.numBytesDone / (1024*1024) << " MiB/s; " <<
					liveResults.newLiveOps.numBytesDone / (1024*1024) << " MiB"
				"] " <<
				"rd=[" <<
					liveResults.liveOpsPerSecReadMix.numIOPSDone << " " << "IOPS; " <<
					liveResults.liveOpsPerSecReadMix.numBytesDone / (1024*1024) << " MiB/s; " <<
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
 * @throw ProgException on error
 * @throw WorkerException if worker encountered error is detected
 */
void Statistics::loopSingleLineLiveStats()
{
	std::chrono::milliseconds sleepMS(progArgs.getLiveStatsSleepMS() );
	std::chrono::steady_clock::time_point nextWakeupT = workersSharedData.phaseStartT;
	const bool useLiveStatsNewLine = progArgs.getUseBriefLiveStatsNewLine();

	LiveResults liveResults;

	liveResults.phaseName = TranslatorTk::benchPhaseToPhaseName(
		workersSharedData.currentBenchPhase, &progArgs);
	liveResults.phaseEntryType = TranslatorTk::benchPhaseToPhaseEntryType(
		workersSharedData.currentBenchPhase);
	liveResults.entryTypeUpperCase =
		TranslatorTk::benchPhaseToPhaseEntryType(workersSharedData.currentBenchPhase, true);

	workerManager.getPhaseNumEntriesAndBytes(
		liveResults.numEntriesPerWorker, liveResults.numBytesPerWorker);

	liveCpuUtil.update(); // init (further updates in loop below)

	if(!useLiveStatsNewLine)
		disableConsoleBuffering();

	while(true)
	{
		nextWakeupT += sleepMS; // prepare for next wait round

		std::unique_lock<std::mutex> lock(workersSharedData.mutex); // L O C K (scoped)

		bool workersDone = workersSharedData.condition.wait_until(
			lock, nextWakeupT,
			[&, this]
			{ return workerManager.checkWorkersDoneUnlocked(&liveResults.numWorkersDone); } );
		if(workersDone)
			goto workers_done;

		lock.unlock(); // U N L O C K

		workerManager.checkPhaseTimeLimit(); // (interrupts workers if time limit exceeded)

		liveCpuUtil.update(); // update local cpu util
		updateLiveStatsRemoteInfo(liveResults); // update info for master mode
		updateLiveStatsLiveOps(liveResults); // update live ops and percent done

		printLiveStatsCSV(liveResults); // live stats csv file

		printSingleLineLiveStatsLine(liveResults);
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
	std::chrono::milliseconds sleepMS(progArgs.getLiveStatsSleepMS() );
	std::chrono::steady_clock::time_point nextWakeupT = workersSharedData.phaseStartT;

	LiveResults liveResults;

	liveResults.phaseName = TranslatorTk::benchPhaseToPhaseName(
		workersSharedData.currentBenchPhase, &progArgs);
	liveResults.phaseEntryType = TranslatorTk::benchPhaseToPhaseEntryType(
		workersSharedData.currentBenchPhase);
	liveResults.entryTypeUpperCase =
		TranslatorTk::benchPhaseToPhaseEntryType(workersSharedData.currentBenchPhase, true);

	workerManager.getPhaseNumEntriesAndBytes(
		liveResults.numEntriesPerWorker, liveResults.numBytesPerWorker);

	liveCpuUtil.update(); // init (further updates in loop below)

	while(true)
	{
		nextWakeupT += sleepMS; // prepare for next wait round

		std::unique_lock<std::mutex> lock(workersSharedData.mutex); // L O C K (scoped)

		bool workersDone = workersSharedData.condition.wait_until(
			lock, nextWakeupT,
			[&, this]
			{ return workerManager.checkWorkersDoneUnlocked(&liveResults.numWorkersDone); } );
		if(workersDone)
			goto workers_done;

		lock.unlock(); // U N L O C K

		workerManager.checkPhaseTimeLimit(); // (interrupts workers if time limit exceeded)

		liveCpuUtil.update(); // update local cpu util
		updateLiveStatsRemoteInfo(liveResults); // update info for master mode
		updateLiveStatsLiveOps(liveResults); // update live ops and percent done

		printLiveStatsCSV(liveResults); // live stats csv file
	}

workers_done:

	return;
}

#ifdef NCURSES_SUPPORT

/**
 * Print whole screen version of live stats until workers are done with phase.
 *
 * @throw ProgException on error
 * @throw WorkerException if worker encountered error is detected
 */
void Statistics::loopFullScreenLiveStats()
{
	std::chrono::milliseconds sleepMS(progArgs.getLiveStatsSleepMS() );
	std::chrono::steady_clock::time_point nextWakeupT = workersSharedData.phaseStartT;
	bool ncursesInitialized = false;

	LiveResults liveResults;

	liveResults.phaseName = TranslatorTk::benchPhaseToPhaseName(
		workersSharedData.currentBenchPhase, &progArgs);
	liveResults.phaseEntryType = TranslatorTk::benchPhaseToPhaseEntryType(
		workersSharedData.currentBenchPhase);
	liveResults.entryTypeUpperCase =
		TranslatorTk::benchPhaseToPhaseEntryType(workersSharedData.currentBenchPhase, true);

	workerManager.getPhaseNumEntriesAndBytes(
		liveResults.numEntriesPerWorker, liveResults.numBytesPerWorker);

	liveCpuUtil.update(); // init (further updates in loop below)

	while(true)
	{
		nextWakeupT += sleepMS; // prepare for next wait round

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
			if(ncursesInitialized)
				endwin();

			throw;
		}

		lock.unlock(); // U N L O C K

		// delayed ncurses init (here instead of before loop to avoid empty screen for first wait)
		IF_UNLIKELY(!ncursesInitialized)
		{
			// (note: initscr() terminates the process on error, hence newterm() here instead)
			SCREEN* initRes = newterm(getenv("TERM"), stdout, stdin); // init curses mode
			if(!initRes)
			{
				std::cerr << "NOTE: ncurses terminal init for fullscreen live statistics failed. "
					"Falling back to \"--" ARG_BRIEFLIVESTATS_LONG "\"." <<
					std::endl;

				loopSingleLineLiveStats();

				return;
			}

			curs_set(0); // hide cursor

			ncursesInitialized = true;
		}

		workerManager.checkPhaseTimeLimit(); // (interrupts workers if time limit exceeded)

		liveCpuUtil.update(); // update local cpu util
		updateLiveStatsRemoteInfo(liveResults); // update info for master mode
		updateLiveStatsLiveOps(liveResults); // update live ops and percent done

		printLiveStatsCSV(liveResults); // live stats csv file

		getmaxyx(stdscr, liveResults.winHeight, liveResults.winWidth); // update screen dimensions

		clear(); // clear screen buffer. (actual screen clear/repaint when refresh() called.)

		// print global info...
		printFullScreenLiveStatsGlobalInfo(liveResults);

		// print table of per-worker results
		printFullScreenLiveStatsWorkerTable(liveResults);

		refresh(); // clear and repaint screen
	}


workers_done:

	if(ncursesInitialized)
		endwin(); // end curses mode
}

/**
 * Print a single line of fullscreen live stats to the ncurses buffer, taking the line length
 * into account by reducing the length if it exceeds the console line length.
 *
 * @stream buffer containing the line to print to ncurses buffer; will be cleared after contents
 * 		have been added to ncurses buffer.
 * @lineLength the maximum line length that max not be exceeded.
 * @fillIfShorter if stream buffer length is shorter than line length then fill it up with spaces
 * 		to match given line length.
 */
void Statistics::printFullScreenLiveStatsLine(std::ostringstream& stream, unsigned lineLength,
	bool fillIfShorter)
{
	std::string lineStr(stream.str() );

	// cut off or fill as requested
	if( (lineStr.length() > lineLength) ||
		(fillIfShorter && (lineStr.length() < lineLength) ) )
		lineStr.resize(lineLength, ' ');

	// add to ncurses buffer
	addstr(lineStr.c_str() );

	// add newline only if string doesn't fill up the complete line
	if(lineStr.length() < lineLength)
		addch('\n');

	// clear stream for next round
	stream.str(std::string() );
}

/**
 * Print global info lines of whole screen live stats.
 */
void Statistics::printFullScreenLiveStatsGlobalInfo(const LiveResults& liveResults)
{
	std::chrono::seconds elapsedSec =
				std::chrono::duration_cast<std::chrono::seconds>
				(std::chrono::steady_clock::now() - workersSharedData.phaseStartT);

	std::ostringstream stream;

	stream << boost::format("Phase: %||  CPU: %|3|%%  Active: %||  Elapsed: %||")
		% liveResults.phaseName
		% (unsigned) liveCpuUtil.getCPUUtilPercent()
		% (workerVec.size() - liveResults.numWorkersDone)
		% UnitTk::elapsedSecToHumanStr(elapsedSec.count() );

	printFullScreenLiveStatsLine(stream, liveResults.winWidth, false);

	if(!progArgs.getShowLatency() )
		return;

	// latency

	const bool isRWMixPhase = (liveResults.newLiveOpsReadMix.numBytesDone ||
		liveResults.newLiveOpsReadMix.numEntriesDone);
	const bool isRWMixThreadsPhase = (isRWMixPhase && progArgs.hasUserSetRWMixReadThreads() );

	stream << "Latency: ";

	if(!isRWMixPhase)
	{
		if(progArgs.getBenchPathType() != BenchPathType_DIR)
			stream << UnitTk::latencyUsToHumanStr(liveResults.liveLatency.avgIOLatMicroSecsSum);
		else
		{ // BenchPathType_DIR
			stream <<
				"IO=" <<
				UnitTk::latencyUsToHumanStr(liveResults.liveLatency.avgIOLatMicroSecsSum) <<
				" " <<
				liveResults.entryTypeUpperCase << "=" <<
				UnitTk::latencyUsToHumanStr(liveResults.liveLatency.avgEntriesLatMicroSecsSum);
		}
	}
	else
	{ // rwmix
		stream <<
			"IO ["
			"wr=" <<
			UnitTk::latencyUsToHumanStr(liveResults.liveLatency.avgIOLatMicroSecsSum) <<
			" "
			"rd=" <<
			UnitTk::latencyUsToHumanStr(liveResults.liveLatency.avgIOLatReadMixMicroSecsSum) <<
			"]";

		if(progArgs.getBenchPathType() == BenchPathType_DIR)
		{
			stream << "  "; // double space as separator

			if(!isRWMixThreadsPhase)
			{
				stream <<
					liveResults.entryTypeUpperCase << "=" <<
					UnitTk::latencyUsToHumanStr(
						liveResults.liveLatency.avgEntriesLatMicroSecsSum);
			}
			else
			{
				stream <<
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

	printFullScreenLiveStatsLine(stream, liveResults.winWidth, false);
}

/**
 * Print table of per-worker results for whole screen live stats (plus headline and total line).
 */
void Statistics::printFullScreenLiveStatsWorkerTable(const LiveResults& liveResults)
{
	const char tableHeadlineFormat[] = "%|5| %|3| %|10| %|10| %|10|";
	const char dirModeTableHeadlineFormat[] = " %|10| %|10|"; // appended to standard table fmt
	const char remoteTableHeadlineFormat[] = " %|4| %|3| %||"; // appended to standard table format

	const bool isRWMixPhase = (liveResults.newLiveOpsReadMix.numBytesDone ||
		liveResults.newLiveOpsReadMix.numEntriesDone);
	const bool isRWMixThreadsPhase = (isRWMixPhase && progArgs.hasUserSetRWMixReadThreads() );

	const bool showDirStats = progArgs.getShowDirStats() &&
		(progArgs.getBenchPathType() == BenchPathType_DIR) &&
		progArgs.getTreeFilePath().empty() &&
		( (workersSharedData.currentBenchPhase == BenchPhase_CREATEFILES) ||
			(workersSharedData.currentBenchPhase == BenchPhase_READFILES) );

	const bool useNetBench = progArgs.getUseNetBench();

	// how many lines we have to show per-worker stats ("+2" for table header and total line)
	size_t maxNumWorkerLines = liveResults.winHeight - (FULLSCREEN_GLOBALINFO_NUMLINES + 2);

	std::ostringstream stream;

	// print table headline...

	attron(A_STANDOUT); // highlight table headline
	stream << boost::format(tableHeadlineFormat)
		% "Rank"
		% "%"
		% "DoneMiB"
		% "MiB/s"
		% "IOPS";

	if(progArgs.getBenchPathType() == BenchPathType_DIR)
	{ // add columns for dir mode
		stream << boost::format(dirModeTableHeadlineFormat)
			% liveResults.entryTypeUpperCase
			% (liveResults.entryTypeUpperCase + "/s");
	}

	if(showDirStats)
	{ // add columns for dir stats in file write/read phase of dir mode
		stream << boost::format(dirModeTableHeadlineFormat)
			% "Dirs"
			% "Dirs/s";
	}

	if(!progArgs.getHostsVec().empty() )
	{ // add columns for remote mode
		stream << boost::format(remoteTableHeadlineFormat)
			% "Act"
			% "CPU"
			% "Service";
	}

	printFullScreenLiveStatsLine(stream, liveResults.winWidth, true);
	attroff(A_STANDOUT); // disable highlighting

	// print total for all workers...

	std::string totalPercentDone = std::to_string(std::min(liveResults.percentDone, (size_t)100) );

	if(useNetBench)
		totalPercentDone = "-";

	stream << boost::format(tableHeadlineFormat)
		% (isRWMixPhase ? "Write" : "Total")
		% totalPercentDone
		% (liveResults.newLiveOps.numBytesDone / (1024*1024) )
		% (liveResults.liveOpsPerSec.numBytesDone / (1024*1024) )
		% liveResults.liveOpsPerSec.numIOPSDone;

	if(progArgs.getBenchPathType() == BenchPathType_DIR)
	{ // add columns for dir mode
		stream << boost::format(dirModeTableHeadlineFormat)
			% liveResults.newLiveOps.numEntriesDone
			% liveResults.liveOpsPerSec.numEntriesDone;
	}

	if(showDirStats)
	{ // add columns for dir stats in file write/read phase of dir mode
		stream << boost::format(dirModeTableHeadlineFormat)
			% (liveResults.newLiveOps.numEntriesDone / progArgs.getNumFiles() )
			% (liveResults.liveOpsPerSec.numEntriesDone / progArgs.getNumFiles() );
	}

	if(!progArgs.getHostsVec().empty() )
	{ // add columns for remote mode
		stream << boost::format(remoteTableHeadlineFormat)
			% liveResults.numRemoteThreadsLeft
			% liveResults.percentRemoteCPU
			% "";
	}

	printFullScreenLiveStatsLine(stream, liveResults.winWidth, true);

	// print rwmix read line for all workers...
	if(isRWMixPhase)
	{
		std::string readPercentDone =
			std::to_string(std::min(liveResults.percentDoneReadMix, (size_t)100) );

		if(useNetBench)
			readPercentDone = "-";

		stream << boost::format(tableHeadlineFormat)
			% "Read"
			% readPercentDone
			% (liveResults.newLiveOpsReadMix.numBytesDone / (1024*1024) )
			% (liveResults.liveOpsPerSecReadMix.numBytesDone / (1024*1024) )
			% liveResults.liveOpsPerSecReadMix.numIOPSDone;

		if(progArgs.getBenchPathType() == BenchPathType_DIR)
		{ // add columns for dir mode
			stream << boost::format(dirModeTableHeadlineFormat)
				% (isRWMixThreadsPhase ?
					std::to_string(liveResults.newLiveOpsReadMix.numEntriesDone) : "-")
				% (isRWMixThreadsPhase ?
					std::to_string(liveResults.liveOpsPerSecReadMix.numEntriesDone) : "-");
		}

		if(showDirStats)
		{ // add columns for dir stats in file write/read phase of dir mode
			size_t numFiles = progArgs.getNumFiles();

			stream << boost::format(dirModeTableHeadlineFormat)
				% (isRWMixThreadsPhase ?
					std::to_string(liveResults.newLiveOpsReadMix.numEntriesDone / numFiles) :
					"-")
				% (isRWMixThreadsPhase ?
					std::to_string(liveResults.liveOpsPerSecReadMix.numEntriesDone / numFiles) :
					"-");
		}

		if(!progArgs.getHostsVec().empty() )
		{ // add columns for remote mode
			stream << boost::format(remoteTableHeadlineFormat)
				% "-"
				% "-"
				% "";
		}

		printFullScreenLiveStatsLine(stream, liveResults.winWidth, true);
	}

	// print individual worker result lines...

	for(size_t i=0; i < std::min(workerVec.size(), maxNumWorkerLines); i++)
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

		if(useNetBench)
		{ // special settings for netbench mode
			if(i < progArgs.getNumNetBenchServers() )
			{ // this is a netbench server
				workerPercentDoneStr = "-"; // this is a server, we only have pct done for clients
				netbenchServiceSuffixStr = " [server]";
			}
			else // this is a netbench client
				netbenchServiceSuffixStr = " [client]";
		}

		stream << boost::format(tableHeadlineFormat)
			% i
			% workerPercentDoneStr
			% (workerDone.numBytesDone / (1024*1024) )
			% (workerDonePerSec.numBytesDone / (1024*1024) )
			% workerDonePerSec.numIOPSDone;

		if(progArgs.getBenchPathType() == BenchPathType_DIR)
		{ // add columns for dir mode
			stream << boost::format(dirModeTableHeadlineFormat)
				% workerDone.numEntriesDone
				% workerDonePerSec.numEntriesDone;
		}

		if(showDirStats)
		{ // add columns for dir stats in file write/read phase of dir mode
			stream << boost::format(dirModeTableHeadlineFormat)
				% (workerDone.numEntriesDone / progArgs.getNumFiles() )
				% (workerDonePerSec.numEntriesDone / progArgs.getNumFiles() );
		}

		if(!progArgs.getHostsVec().empty() )
		{ // add columns for remote mode
			RemoteWorker* remoteWorker = static_cast<RemoteWorker*>(workerVec[i]);
			stream << boost::format(remoteTableHeadlineFormat)
				% (progArgs.getNumThreads() - remoteWorker->getNumWorkersDone() )
				% remoteWorker->getCPUUtilLive()
				% (progArgs.getHostsVec()[i] + netbenchServiceSuffixStr);
		}

		printFullScreenLiveStatsLine(stream, liveResults.winWidth, true);
	}
}

#endif // NCURSES_SUPPORT

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
 */
void Statistics::updateLiveStatsLiveOps(LiveResults& liveResults)
{
	getLiveOps(liveResults.newLiveOps, liveResults.newLiveOpsReadMix, liveResults.liveLatency);

	liveResults.liveOpsPerSec = liveResults.newLiveOps - liveResults.lastLiveOps;
	(liveResults.liveOpsPerSec *= 1000) /= progArgs.getLiveStatsSleepMS();

	liveResults.lastLiveOps = liveResults.newLiveOps;

	liveResults.liveOpsPerSecReadMix =
		liveResults.newLiveOpsReadMix - liveResults.lastLiveOpsReadMix;
	(liveResults.liveOpsPerSecReadMix *= 1000) /= progArgs.getLiveStatsSleepMS();

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

	#ifdef NCURSES_SUPPORT
		if( (!progArgs.getUseBriefLiveStats() ) &&
			( (progArgs.getHostsVec().size() > 1) ||
			  (progArgs.getHostsVec().empty() && (progArgs.getNumThreads() > 1) ) ) )
		{
			loopFullScreenLiveStats();
			return;
		}
	#endif // NCURSES_SUPPORT

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
 * @usePerThreadValues true to div numEntriesDone/numBytesDone by number of workers.
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

	outTree.put(XFER_STATS_BENCHID, workersSharedData.currentBenchID);
	outTree.put(XFER_STATS_BENCHPHASENAME,
		TranslatorTk::benchPhaseToPhaseName(workersSharedData.currentBenchPhase, &progArgs) );
	outTree.put(XFER_STATS_BENCHPHASECODE, workersSharedData.currentBenchPhase);
	outTree.put(XFER_STATS_NUMWORKERSDONE, workersSharedData.numWorkersDone);
	outTree.put(XFER_STATS_NUMWORKERSDONEWITHERR, workersSharedData.numWorkersDoneWithError);
	outTree.put(XFER_STATS_TRIGGERSTONEWALL,
		!workerVec.empty() && workerVec[0]->getStoneWallTriggered());
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
			progArgs.getUseNetBench() ) )
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
	if(!progArgs.getResFilePath().empty() )
	{
		std::ofstream fileStream;

		fileStream.open(progArgs.getResFilePath(), std::ofstream::app);

		if(!fileStream)
		{
			std::cerr << "ERROR: Opening results file failed: " << progArgs.getResFilePath() <<
				std::endl;

			return;
		}

		// print current date

		std::time_t currentTime = std::time(NULL);

		fileStream << "ISO DATE: " << std::put_time(std::localtime(&currentTime), "%FT%T%z") <<
			std::endl;

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
			std::cerr << "ERROR: Writing to results file failed: " << progArgs.getResFilePath() <<
				std::endl;

			return;
		}
	}

	// print to CSV results file (if specified by user)
	if(!progArgs.getCSVFilePath().empty() && progArgs.getPrintCSVLabels() &&
		FileTk::checkFileEmpty(progArgs.getCSVFilePath() ) )
	{
		std::ofstream fileStream;

		fileStream.open(progArgs.getCSVFilePath(), std::ofstream::app);

		if(!fileStream)
		{
			std::cerr << "ERROR: Opening CSV results file failed: " << progArgs.getCSVFilePath() <<
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
				progArgs.getResFilePath() << std::endl;

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

    std::stringstream dateStream;
    dateStream << std::put_time(std::localtime(&time), "%FT%T") << "."
        << std::setfill('0') << std::setw(3) << milliseconds
        << std::put_time(std::localtime(&time), "%z");

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
		std::cout << "Skipping stats print due to unavailable worker results." << std::endl;
	else
		printPhaseResultsToStream(phaseResults, std::cout); // print to console


	// print to results file (if specified by user)
	if(!progArgs.getResFilePath().empty() )
	{
		std::ofstream fileStream;

		fileStream.open(progArgs.getResFilePath(), std::ofstream::app);

		if(!fileStream)
		{
			std::cerr << "ERROR: Opening results file failed: " << progArgs.getResFilePath() <<
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
	if(genRes && !progArgs.getCSVFilePath().empty() )
	{
		std::ofstream fileStream;

		fileStream.open(progArgs.getCSVFilePath(), std::ofstream::app);

		if(!fileStream)
		{
			std::cerr << "ERROR: Opening results CSV file failed: " << progArgs.getCSVFilePath() <<
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
    if(genRes && !progArgs.getJSONFilePath().empty() )
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
	std::string entryTypeUpperCase =
		TranslatorTk::benchPhaseToPhaseEntryType(workersSharedData.currentBenchPhase, true);
	std::string entryTypeLowerCase =
		TranslatorTk::benchPhaseToPhaseEntryType(workersSharedData.currentBenchPhase, false);

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
            % (isRWMixPhase ? "MiB/s write" : "Throughput MiB/s")
            % ":"
            % (phaseResults.opsStoneWallPerSec.numBytesDone / (1024*1024) )
            % (phaseResults.opsPerSec.numBytesDone / (1024*1024) )
            << std::endl;
    }

    // rwmix read bytes per second total for all workers
    if(phaseResults.opsTotalReadMix.numBytesDone)
    {
        outStream << boost::format(Statistics::phaseResultsFormatStr)
            % ""
            % "MiB/s read"
            % ":"
            % (phaseResults.opsStoneWallPerSecReadMix.numBytesDone / (1024*1024) )
            % (phaseResults.opsPerSecReadMix.numBytesDone / (1024*1024) )
            << std::endl;

        uint64_t totalBytesPerSecFirstDone = phaseResults.opsStoneWallPerSec.numBytesDone +
            phaseResults.opsStoneWallPerSecReadMix.numBytesDone;
        uint64_t totalBytesPerSecLastDone = phaseResults.opsPerSec.numBytesDone +
            phaseResults.opsPerSecReadMix.numBytesDone;

        outStream << boost::format(Statistics::phaseResultsFormatStr)
            % ""
            % "MiB/s total"
            % ":"
            % (totalBytesPerSecFirstDone / (1024*1024) )
            % (totalBytesPerSecLastDone / (1024*1024) )
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
	outStream << "---" << std::endl;
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

    std::stringstream dateStream;
    dateStream << std::put_time(std::localtime(&time), "%FT%T") << "."
        << std::setfill('0') << std::setw(3) << milliseconds
        << std::put_time(std::localtime(&time), "%z");

    ptree.put("iso_start_date", dateStream.str() );

    // elapsed time

    firstDoneSubtree.put("elapsed_time_ms", phaseResults.firstFinishUSec / 1000);
    lastDoneSubtree.put("elapsed_time_ms", phaseResults.lastFinishUSec / 1000);


    // fist done subtree

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

    // copy first done & last done subtrees into main tree

    ptree.put_child("first_done", firstDoneSubtree);
    ptree.put_child("last_done", lastDoneSubtree);

    // elbencho version

    ptree.put("version", EXE_VERSION);

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

    ptree.put("command", cmdString);

    // print json

    try
    {
        std::ofstream fileStream;

        fileStream.open(progArgs.getJSONFilePath(), std::ofstream::app);

        if(!fileStream)
        {
            std::cerr << "ERROR: Opening results JSON file failed: " << progArgs.getJSONFilePath() <<
                std::endl;

            return;
        }

        bpt::json_parser::write_json(fileStream, ptree, false); // throws on error
    }
    catch(const std::exception& e)
    {
        throw ProgException("Error writing JSON result file. "
            "File: " + progArgs.getJSONFilePath() + "; " +
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

	outTree.put(XFER_STATS_BENCHID, workersSharedData.currentBenchID);
	outTree.put(XFER_STATS_BENCHPHASENAME,
		TranslatorTk::benchPhaseToPhaseName(workersSharedData.currentBenchPhase, &progArgs) );
	outTree.put(XFER_STATS_BENCHPHASECODE, workersSharedData.currentBenchPhase);
	outTree.put(XFER_STATS_NUMWORKERSDONE, workersSharedData.numWorkersDone);
	outTree.put(XFER_STATS_NUMWORKERSDONEWITHERR, workersSharedData.numWorkersDoneWithError);
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
				progArgs.getUseNetBench() ) )
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
			progArgs.getUseNetBench() ) )
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
	if(progArgs.getUseNetBench() )
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

    std::stringstream dateStream;
    dateStream << std::put_time(std::localtime(&time), "%FT%T") << "."
        << std::setfill('0') << std::setw(3) << milliseconds
        << std::put_time(std::localtime(&time), "%z");

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
