#include <boost/format.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <chrono>
#include <ncurses.h>
#undef OK // defined by ncurses.h, but conflicts with AWS SDK CPP using OK in enum in HttpResponse.h
#include "ProgException.h"
#include "Statistics.h"
#include "Terminal.h"
#include "toolkits/TranslatorTk.h"
#include "workers/RemoteWorker.h"
#include "workers/Worker.h"
#include "workers/WorkerException.h"

#define CONTROLCHARS_CLEARLINE_AND_CARRIAGERETURN	"\x1B[2K\r" /* "\x1B[2K" is the VT100 code to
										clear the line. "\r" moves cursor to beginning of line. */
#define WHOLESCREEN_GLOBALINFO_NUMLINES		1 // lines for global info (to calc per-worker lines)


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

	bool disableRes = Terminal::disableConsoleBuffering();

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

	bool resetRes = Terminal::resetConsoleBuffering();

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

	if(!Terminal::isStdoutTTY() )
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

	deleteSingleLineLiveStatsLine();
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

	printf("%c[2K\r"
		"Waiting for start time: %Lus", 27, (unsigned long long)waittimeSec);
}

/**
 * Print a live stats line, ensuring that the printed line does not exceed the terminal row length.
 *
 * @liveOpsPerSec operations per second.
 * @liveOps absolute values since beginning of current benchmark phase.
 * @elapsedSec elapsed seconds since beginning of current benchmark phase.
 */
void Statistics::printSingleLineLiveStatsLine(const char* phaseName, const char* phaseEntryType,
	LiveOps& liveOpsPerSec, LiveOps& rwMixReadLiveOpsPerSec, LiveOps& liveOps,
	unsigned long long numWorkersLeft, size_t cpuUtil, unsigned long long elapsedSec)
{
	const size_t hiddenControlCharsLen = strlen(CONTROLCHARS_CLEARLINE_AND_CARRIAGERETURN);

	// prepare line including control chars to clear line and do carriage return
	std::ostringstream stream;

	if(progArgs.getBenchPathType() == BenchPathType_DIR)
		stream << CONTROLCHARS_CLEARLINE_AND_CARRIAGERETURN <<
			phaseName << ": " <<
			liveOpsPerSec.numEntriesDone << " " << phaseEntryType << "/s; " <<
			liveOpsPerSec.numBytesDone / (1024*1024) << " MiB/s; " <<
			(!rwMixReadLiveOpsPerSec.numBytesDone ? "" :
				std::to_string(rwMixReadLiveOpsPerSec.numBytesDone / (1024*1024) ) +
				" MiB/s read; ") <<
			liveOps.numEntriesDone << " " << phaseEntryType << "; " <<
			liveOps.numBytesDone / (1024*1024) << " MiB; " <<
			numWorkersLeft << " threads; " <<
			cpuUtil << "% CPU; " <<
			elapsedSec << "s";
	else // show IOPS instead of files/dirs per sec
		stream << CONTROLCHARS_CLEARLINE_AND_CARRIAGERETURN <<
			phaseName << ": " <<
			liveOpsPerSec.numIOPSDone << " " << "IOPS; " <<
			liveOpsPerSec.numBytesDone / (1024*1024) << " MiB/s; " <<
			(!rwMixReadLiveOpsPerSec.numBytesDone ? "" :
				std::to_string(rwMixReadLiveOpsPerSec.numBytesDone / (1024*1024) ) +
				" MiB/s read; ") <<
			liveOps.numBytesDone / (1024*1024) << " MiB; " <<
			numWorkersLeft << " threads; " <<
			cpuUtil << "% CPU; " <<
			elapsedSec << "s";

	std::string lineStr(stream.str() );

	// check console size to handle console resize at runtime

	int terminalLineLen = Terminal::getTerminalLineLength();
	if(!terminalLineLen)
		return; // don't cancel program (by throwing) just because we can't show live stats

	unsigned usableLineLen = terminalLineLen + hiddenControlCharsLen;

	if(lineStr.length() > usableLineLen)
		lineStr.resize(usableLineLen);

	std::cout << lineStr;
}

/**
 * Erase the current line and move cursor back to beginning of erased line.
 */
void Statistics::deleteSingleLineLiveStatsLine()
{
	std::cout << CONTROLCHARS_CLEARLINE_AND_CARRIAGERETURN;
}

/**
 * Print single line version of live stats until workers are done with phase.
 *
 * @throw ProgException on error
 * @throw WorkerException if worker encountered error is detected
 */
void Statistics::printSingleLineLiveStats()
{
	std::string phaseName = TranslatorTk::benchPhaseToPhaseName(
		workersSharedData.currentBenchPhase, &progArgs);
	std::string phaseEntryType = TranslatorTk::benchPhaseToPhaseEntryType(
		workersSharedData.currentBenchPhase);
	time_t startTime = time(NULL);
	LiveOps lastLiveOps = {}; // init all to 0
	LiveOps lastLiveRWMixReadOps = {}; // init all to 0
	size_t numWorkersDone;

	liveCpuUtil.update();

	disableConsoleBuffering();

	while(true)
	{
		std::unique_lock<std::mutex> lock(workersSharedData.mutex); // L O C K (scoped)

		bool workersDone = workersSharedData.condition.wait_for(
			lock, std::chrono::seconds(progArgs.getLiveStatsSleepSec() ),
			[&, this]{ return workerManager.checkWorkersDoneUnlocked(&numWorkersDone); } );
		if(workersDone)
			goto workers_done;

		lock.unlock(); // U N L O C K

		workerManager.checkPhaseTimeLimit();

		unsigned long long numWorkersLeft;
		size_t cpuUtil = 0;

		// calc numWorkersLeft & cpu util
		if(progArgs.getHostsVec().empty() )
		{ // local mode
			liveCpuUtil.update();

			numWorkersLeft = workerVec.size() - numWorkersDone;
			cpuUtil = liveCpuUtil.getCPUUtilPercent();
		}
		else
		{ // remote mode
			numWorkersLeft = progArgs.getHostsVec().size() * progArgs.getNumThreads();

			for(Worker* worker : workerVec)
			{
				RemoteWorker* remoteWorker =  static_cast<RemoteWorker*>(worker);
				numWorkersLeft -= remoteWorker->getNumWorkersDone();
				numWorkersLeft -= remoteWorker->getNumWorkersDoneWithError();
				cpuUtil = remoteWorker->getCPUUtilLive();
			}

			cpuUtil /= workerVec.size();
		}

		LiveOps newLiveOps;
		LiveOps newLiveRWMixReadOps;

		getLiveOps(newLiveOps, newLiveRWMixReadOps);

		LiveOps opsPerSec = newLiveOps - lastLiveOps;
		opsPerSec /= progArgs.getLiveStatsSleepSec();

		lastLiveOps = newLiveOps;

		LiveOps rwMixReadOpsPerSec = newLiveRWMixReadOps - lastLiveRWMixReadOps;
		rwMixReadOpsPerSec /= progArgs.getLiveStatsSleepSec();

		lastLiveRWMixReadOps = newLiveRWMixReadOps;

		time_t elapsedSec = time(NULL) - startTime;

		printSingleLineLiveStatsLine(phaseName.c_str(), phaseEntryType.c_str(),
			opsPerSec, rwMixReadOpsPerSec, newLiveOps, numWorkersLeft, cpuUtil, elapsedSec);
	}

workers_done:

	deleteSingleLineLiveStatsLine();
	resetConsoleBuffering();
}

/**
 * Print a single line of whole screen live stats to the ncurses buffer, taking the line length
 * into account by reducing the length if it exceeds the console line length.
 *
 * @stream buffer containing the line to print to ncurses buffer; will be cleared after contents
 * 		have been added to ncurses buffer.
 * @lineLength the maximum line length that max not be exceeded.
 * @fillIfShorter if stream buffer length is shorter than line length then fill it up with spaces
 * 		to match given line length.
 */
void Statistics::printWholeScreenLine(std::ostringstream& stream, unsigned lineLength,
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
 * Print whole screen version of live stats until workers are done with phase.
 *
 * @throw ProgException on error
 * @throw WorkerException if worker encountered error is detected
 */
void Statistics::printWholeScreenLiveStats()
{
	bool ncursesInitialized = false;

	LiveResults liveResults;

	liveResults.phaseName = TranslatorTk::benchPhaseToPhaseName(
		workersSharedData.currentBenchPhase, &progArgs);
	liveResults.phaseEntryType = TranslatorTk::benchPhaseToPhaseEntryType(
		workersSharedData.currentBenchPhase);
	liveResults.entryTypeUpperCase =
		TranslatorTk::benchPhaseToPhaseEntryType(workersSharedData.currentBenchPhase, true);
	liveResults.startTime = time(NULL);

	workerManager.getPhaseNumEntriesAndBytes(
		liveResults.numEntriesPerWorker, liveResults.numBytesPerWorker);

	liveCpuUtil.update(); // further updates per round in printWholeScreenLiveStatsGlobalInfo()

	while(true)
	{
		std::unique_lock<std::mutex> lock(workersSharedData.mutex); // L O C K (scoped)

		try
		{
			bool workersDone = workersSharedData.condition.wait_for(
				lock, std::chrono::seconds(progArgs.getLiveStatsSleepSec() ),
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
			WINDOW* initRes = initscr(); // init curses mode
			if(!initRes)
			{
				ErrLogger(Log_DEBUG) << "ncurses initscr() failed." << std::endl;
				return;
			}

			curs_set(0); // hide cursor

			ncursesInitialized = true;
		}

		workerManager.checkPhaseTimeLimit(); // (interrupts workers if time limit exceeded)

		wholeScreenLiveStatsUpdateRemoteInfo(liveResults); // update info for master mode
		wholeScreenLiveStatsUpdateLiveOps(liveResults); // update live ops and percent done

		getmaxyx(stdscr, liveResults.winHeight, liveResults.winWidth); // update screen dimensions

		clear(); // clear screen buffer. (actual screen clear/repaint when refresh() called.)

		// print global info...
		printWholeScreenLiveStatsGlobalInfo(liveResults);

		// print table of per-worker results
		printWholeScreenLiveStatsWorkerTable(liveResults);

		refresh(); // clear and repaint screen
	}


workers_done:

	if(ncursesInitialized)
		endwin(); // end curses mode
}

/**
 * In master mode, update number of remote threads left in liveResults and avg cpu; otherwise do
 * nothing.
 */
void Statistics::wholeScreenLiveStatsUpdateRemoteInfo(LiveResults& liveResults)
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
void Statistics::wholeScreenLiveStatsUpdateLiveOps(LiveResults& liveResults)
{
	getLiveOps(liveResults.newLiveOps, liveResults.newRWMixReadLiveOps);

	liveResults.liveOpsPerSec = liveResults.newLiveOps - liveResults.lastLiveOps;
	liveResults.liveOpsPerSec /= progArgs.getLiveStatsSleepSec();

	liveResults.lastLiveOps = liveResults.newLiveOps;

	liveResults.rwMixReadliveOpsPerSec =
		liveResults.newRWMixReadLiveOps - liveResults.lastRWMixReadLiveOps;
	liveResults.rwMixReadliveOpsPerSec /= progArgs.getLiveStatsSleepSec();

	liveResults.lastRWMixReadLiveOps = liveResults.newRWMixReadLiveOps;

	// if we have bytes in this phase, use them for percent done; otherwise use num entries
	if(liveResults.numBytesPerWorker)
		liveResults.percentDone =
			(100 * liveResults.newLiveOps.numBytesDone) /
			(liveResults.numBytesPerWorker * workerVec.size() );
	else
	if(liveResults.numEntriesPerWorker)
		liveResults.percentDone =
			(100 * liveResults.newLiveOps.numEntriesDone) /
			(liveResults.numEntriesPerWorker * workerVec.size() );
	else
		liveResults.percentDone = 0; // no % available in phases like "sync" or "dropcaches"
}

/**
 * Print global info lines of whole screen live stats.
 */
void Statistics::printWholeScreenLiveStatsGlobalInfo(LiveResults& liveResults)
{
	time_t elapsedSec = time(NULL) - liveResults.startTime;

	// update cpu util

	liveCpuUtil.update();

	std::ostringstream stream;

	stream << boost::format("Phase: %||  CPU: %|3|%%  Active: %||  Elapsed: %||s")
		% liveResults.phaseName
		% (unsigned) liveCpuUtil.getCPUUtilPercent()
		% (workerVec.size() - liveResults.numWorkersDone)
		% elapsedSec;

	printWholeScreenLine(stream, liveResults.winWidth, false);
}

/**
 * Print table of per-worker results for whole screen live stats (plus headline and total line).
 */
void Statistics::printWholeScreenLiveStatsWorkerTable(LiveResults& liveResults)
{
	const char tableHeadlineFormat[] = "%|5| %|3| %|10| %|10| %|10|";
	const char dirModeTableHeadlineFormat[] = " %|10| %|10|"; // appended to standard table fmt
	const char remoteTableHeadlineFormat[] = " %|4| %|3| %||"; // appended to standard table format

	// how many lines we have to show per-worker stats ("+2" for table header and total line)
	size_t maxNumWorkerLines = liveResults.winHeight - (WHOLESCREEN_GLOBALINFO_NUMLINES + 2);

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

	if(!progArgs.getHostsVec().empty() )
	{ // add columns for remote mode
		stream << boost::format(remoteTableHeadlineFormat)
			% "Act"
			% "CPU"
			% "Service";
	}

	printWholeScreenLine(stream, liveResults.winWidth, true);
	attroff(A_STANDOUT); // disable highlighting

	// print total for all workers...

	stream << boost::format(tableHeadlineFormat)
		% "Total"
		% std::min(liveResults.percentDone, (size_t)100)
		% (liveResults.newLiveOps.numBytesDone / (1024*1024) )
		% (liveResults.liveOpsPerSec.numBytesDone / (1024*1024) )
		% liveResults.liveOpsPerSec.numIOPSDone;

	if(progArgs.getBenchPathType() == BenchPathType_DIR)
	{ // add columns for dir mode
		stream << boost::format(dirModeTableHeadlineFormat)
			% liveResults.newLiveOps.numEntriesDone
			% liveResults.liveOpsPerSec.numEntriesDone;
	}

	if(!progArgs.getHostsVec().empty() )
	{ // add columns for remote mode
		stream << boost::format(remoteTableHeadlineFormat)
			% liveResults.numRemoteThreadsLeft
			% liveResults.percentRemoteCPU
			% "";
	}

	printWholeScreenLine(stream, liveResults.winWidth, true);

	// print rwmix read line for all workers...
	if(liveResults.newRWMixReadLiveOps.numBytesDone)
	{
		stream << boost::format(tableHeadlineFormat)
			% "Read"
			% "-"
			% (liveResults.newRWMixReadLiveOps.numBytesDone / (1024*1024) )
			% (liveResults.rwMixReadliveOpsPerSec.numBytesDone / (1024*1024) )
			% liveResults.rwMixReadliveOpsPerSec.numIOPSDone;

		if(progArgs.getBenchPathType() == BenchPathType_DIR)
		{ // add columns for dir mode
			stream << boost::format(dirModeTableHeadlineFormat)
				% liveResults.newRWMixReadLiveOps.numEntriesDone
				% liveResults.rwMixReadliveOpsPerSec.numEntriesDone;
		}

		if(!progArgs.getHostsVec().empty() )
		{ // add columns for remote mode
			stream << boost::format(remoteTableHeadlineFormat)
				% "-"
				% "-"
				% "";
		}

		printWholeScreenLine(stream, liveResults.winWidth, true);
	}

	// print individual worker result lines...

	for(size_t i=0; i < std::min(workerVec.size(), maxNumWorkerLines); i++)
	{
		LiveOps workerDone; // total numbers
		LiveOps workerDonePerSec; // difference to last round

		workerVec[i]->getLiveOps(workerDone);
		workerVec[i]->getAndResetDiffStats(workerDonePerSec);

		workerDonePerSec /= progArgs.getLiveStatsSleepSec();

		size_t workerPercentDone = 0;

		// if we have bytes in this phase, use them for percent done; otherwise use num entries
		// (note: phases like sync and drop_caches have neither bytes nor entries)
		if(liveResults.numBytesPerWorker)
			workerPercentDone = (100 * workerDone.numBytesDone) / liveResults.numBytesPerWorker;
		else
		if(liveResults.numEntriesPerWorker)
			workerPercentDone = (100 * workerDone.numEntriesDone) / liveResults.numEntriesPerWorker;

		stream << boost::format(tableHeadlineFormat)
			% i
			% std::min(workerPercentDone, (size_t)100)
			% (workerDone.numBytesDone / (1024*1024) )
			% (workerDonePerSec.numBytesDone / (1024*1024) )
			% workerDonePerSec.numIOPSDone;

		if(progArgs.getBenchPathType() == BenchPathType_DIR)
		{ // add columns for dir mode
			stream << boost::format(dirModeTableHeadlineFormat)
				% workerDone.numEntriesDone
				% workerDonePerSec.numEntriesDone;
		}

		if(!progArgs.getHostsVec().empty() )
		{ // add columns for remote mode
			RemoteWorker* remoteWorker = static_cast<RemoteWorker*>(workerVec[i]);
			stream << boost::format(remoteTableHeadlineFormat)
				% (progArgs.getNumThreads() - remoteWorker->getNumWorkersDone() )
				% remoteWorker->getCPUUtilLive()
				% progArgs.getHostsVec()[i];
		}

		printWholeScreenLine(stream, liveResults.winWidth, true);
	}
}

/**
 * Print live statistics on console until workers are done with phase.
 *
 * @throw ProgException on error
 * @throw WorkerException if worker encountered error is detected
 */
void Statistics::printLiveStats()
{
	if(progArgs.getDisableLiveStats() )
		return; // live stats disabled

	if(!Terminal::isStdoutTTY() )
		return; // live stats won't work

	if( (progArgs.getHostsVec().size() == 1) ||
		(progArgs.getHostsVec().empty() && (progArgs.getNumThreads() == 1) ) )
		printSingleLineLiveStats();

	printWholeScreenLiveStats();
}

/**
 * Get current processed entries sum and bytes written/read of all workers.
 *
 * @param outLiveOps entries/ops/bytes done for all workers (does not need to be initialized to 0).
 */
void Statistics::getLiveOps(LiveOps& outLiveOps)
{
	outLiveOps = {}; // set all members to zero

	for(size_t i=0; i < workerVec.size(); i++)
		workerVec[i]->getAndAddLiveOps(outLiveOps);
}

/**
 * Get current processed entries sum and bytes written/read of all workers.
 *
 * @param outLiveOps entries/ops/bytes done for all workers (does not need to be initialized to 0).
 * @param outLiveRWMixReadOps rwmix mode read entries/ops/bytes done for all workers (does not need
 * 		to be initialized to 0).
 */
void Statistics::getLiveOps(LiveOps& outLiveOps, LiveOps& outLiveRWMixReadOps)
{
	outLiveOps = {}; // set all members to zero
	outLiveRWMixReadOps = {}; // set all members to zero

	for(size_t i=0; i < workerVec.size(); i++)
		workerVec[i]->getAndAddLiveOps(outLiveOps, outLiveRWMixReadOps);
}

/**
 * @usePerThreadValues true to div numEntriesDone/numBytesDone by number of workers.
 */
void Statistics::getLiveStatsAsPropertyTree(bpt::ptree& outTree)
{
	LiveOps liveOps;
	LiveOps liveRWMixReadOps;

	getLiveOps(liveOps, liveRWMixReadOps);

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
	outTree.put(XFER_STATS_NUMENTRIESDONE, liveOps.numEntriesDone);
	outTree.put(XFER_STATS_NUMBYTESDONE, liveOps.numBytesDone);
	outTree.put(XFER_STATS_NUMIOPSDONE, liveOps.numIOPSDone);
	outTree.put(XFER_STATS_CPUUTIL, (unsigned) liveCpuUtil.getCPUUtilPercent() );
	outTree.put(XFER_STATS_ELAPSEDSECS, elapsedSecs);

	if( (workersSharedData.currentBenchPhase == BenchPhase_CREATEFILES) &&
		(progArgs.getRWMixPercent() || progArgs.getNumS3RWMixReadThreads() ) )
	{
		outTree.put(XFER_STATS_NUMBYTESDONE_RWMIXREAD, liveRWMixReadOps.numBytesDone);
		outTree.put(XFER_STATS_NUMIOPSDONE_RWMIXREAD, liveRWMixReadOps.numIOPSDone);
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

		// print command line

		fileStream << "COMMAND LINE: ";

		for(int i=0; i < progArgs.getProgArgCount(); i++)
			fileStream << "\"" << progArgs.getProgArgVec()[i] << "\" ";

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
	if(!progArgs.getCSVFilePath().empty() && progArgs.getPrintCSVLabels() && checkCSVFileEmpty() )
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

		printISODateToStringVec(labelsVec, resultsVec);
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
 * Get current ISO date & time to string vector, e.g. to later use it for CSV output.
 */
void Statistics::printISODateToStringVec(StringVec& outLabelsVec, StringVec& outResultsVec)
{
	time_t time = std::time(NULL);
	std::stringstream dateStream;
	dateStream << std::put_time(std::localtime(&time), "%FT%T%z");

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
		% "========="
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
	}

	// print to results CSV file (if specified by user)
	if(!progArgs.getCSVFilePath().empty() )
	{
		std::ofstream fileStream;

		fileStream.open(progArgs.getCSVFilePath(), std::ofstream::app);

		if(!fileStream)
		{
			std::cerr << "ERROR: Opening results CSV file failed: " << progArgs.getResFilePath() <<
				std::endl;

			return;
		}

		if(!genRes)
			fileStream << "Skipping stats print due to unavailable worker results." << std::endl;
		else
		{
			StringVec labelsVec;
			StringVec resultsVec;

			printISODateToStringVec(labelsVec, resultsVec);
			progArgs.getAsStringVec(labelsVec, resultsVec);
			printPhaseResultsToStringVec(phaseResults, labelsVec, resultsVec);

			std::string resultsCSVStr = TranslatorTk::stringVecToString(resultsVec, ",");

			fileStream << resultsCSVStr << std::endl;
		}
	}

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
	IF_UNLIKELY(workerVec.empty() || workerVec[0]->getElapsedUSecVec().empty() )
		return false;

	// init first/last elapsed milliseconds
	phaseResults.firstFinishUSec =
		workerVec[0]->getElapsedUSecVec()[0]; // stonewall: time to completion of fastest worker
	phaseResults.lastFinishUSec =
		workerVec[0]->getElapsedUSecVec()[0]; // time to completion of slowest worker

	// sum up total values
	for(Worker* worker : workerVec)
	{
		IF_UNLIKELY(worker->getElapsedUSecVec().empty() )
			return false;

		for(uint64_t elapsedUSec : worker->getElapsedUSecVec() )
		{
			if(elapsedUSec < phaseResults.firstFinishUSec)
				phaseResults.firstFinishUSec = elapsedUSec;

			if(elapsedUSec > phaseResults.lastFinishUSec)
				phaseResults.lastFinishUSec = elapsedUSec;
		}

		worker->getAndAddLiveOps(phaseResults.opsTotal, phaseResults.opsRWMixReadTotal);
		worker->getAndAddStoneWallOps(phaseResults.opsStoneWallTotal,
			phaseResults.opsStoneWallRWMixReadTotal);
		phaseResults.iopsLatHisto += worker->getIOPSLatencyHistogram();
		phaseResults.entriesLatHisto += worker->getEntriesLatencyHistogram();

	} // end of for loop

	// total per sec for all workers by 1st finisher
	if(phaseResults.firstFinishUSec)
	{
		phaseResults.opsStoneWallTotal.getPerSecFromUSec(
			phaseResults.firstFinishUSec, phaseResults.opsStoneWallPerSec);
	}

	// rwmix read total per sec for all workers by 1st finisher
	if(phaseResults.firstFinishUSec && phaseResults.opsRWMixReadTotal.numIOPSDone)
	{
		phaseResults.opsStoneWallRWMixReadTotal.getPerSecFromUSec(
			phaseResults.firstFinishUSec, phaseResults.opsStoneWallRWMixReadPerSec);
	}

	// total per sec for all workers by last finisher
	if(phaseResults.lastFinishUSec)
	{
		phaseResults.opsTotal.getPerSecFromUSec(
			phaseResults.lastFinishUSec, phaseResults.opsPerSec);
	}

	// rwmix read total per sec for all workers by last finisher
	if(phaseResults.lastFinishUSec && phaseResults.opsRWMixReadTotal.numIOPSDone)
	{
		phaseResults.opsRWMixReadTotal.getPerSecFromUSec(
			phaseResults.lastFinishUSec, phaseResults.opsRWMixReadPerSec);
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

	// elapsed time
	outStream << boost::format(Statistics::phaseResultsFormatStr)
		% phaseName
		% "Elapsed ms"
		% ":"
		% (phaseResults.firstFinishUSec / 1000)
		% (phaseResults.lastFinishUSec / 1000)
		<< std::endl;

	// entries (dirs/files) per second
	if(phaseResults.opsTotal.numEntriesDone)
	{
		outStream << boost::format(Statistics::phaseResultsFormatStr)
			% ""
			% (entryTypeUpperCase + "/s")
			% ":"
			% phaseResults.opsStoneWallPerSec.numEntriesDone
			% phaseResults.opsPerSec.numEntriesDone
			<< std::endl;
	}

	// IOPS
	if(phaseResults.opsTotal.numIOPSDone)
	{
		/* print iops only if path is bdev/file; or in dir mode when each file consists of more than
		   one block read/write (because otherwise iops is equal to files/s) */
		if( (progArgs.getBenchPathType() != BenchPathType_DIR) ||
			(progArgs.getBlockSize() != progArgs.getFileSize() ) )
		outStream << boost::format(Statistics::phaseResultsFormatStr)
			% ""
			% "IOPS"
			% ":"
			% phaseResults.opsStoneWallPerSec.numIOPSDone
			% phaseResults.opsPerSec.numIOPSDone
			<< std::endl;
	}

	// IOPS rwmix read
	if(phaseResults.opsRWMixReadTotal.numIOPSDone)
	{
		/* print iops only if path is bdev/file; or in dir mode when each file consists of more than
		   one block read/write (because otherwise iops is equal to files/s) */
		if( (progArgs.getBenchPathType() != BenchPathType_DIR) ||
			(progArgs.getBlockSize() != progArgs.getFileSize() ) )
		outStream << boost::format(Statistics::phaseResultsFormatStr)
			% ""
			% "IOPS read"
			% ":"
			% phaseResults.opsStoneWallRWMixReadPerSec.numIOPSDone
			% phaseResults.opsRWMixReadPerSec.numIOPSDone
			<< std::endl;
	}

	// bytes per second total for all workers
	if(phaseResults.opsTotal.numBytesDone)
	{
		outStream << boost::format(Statistics::phaseResultsFormatStr)
			% ""
			% "Throughput MiB/s"
			% ":"
			% (phaseResults.opsStoneWallPerSec.numBytesDone / (1024*1024) )
			% (phaseResults.opsPerSec.numBytesDone / (1024*1024) )
			<< std::endl;
	}

	// rwmix read bytes per second total for all workers
	if(phaseResults.opsRWMixReadTotal.numBytesDone)
	{
		outStream << boost::format(Statistics::phaseResultsFormatStr)
			% ""
			% "Read MiB/s"
			% ":"
			% (phaseResults.opsStoneWallRWMixReadPerSec.numBytesDone / (1024*1024) )
			% (phaseResults.opsRWMixReadPerSec.numBytesDone / (1024*1024) )
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

	// entries (dirs/files) processed in total
	if(phaseResults.opsTotal.numEntriesDone)
	{
		outStream << boost::format(Statistics::phaseResultsFormatStr)
			% ""
			% ("Total " + entryTypeLowerCase)
			% ":"
			% phaseResults.opsStoneWallTotal.numEntriesDone
			% phaseResults.opsTotal.numEntriesDone
			<< std::endl;
	}

	// sum of bytes read/written
	if(phaseResults.opsTotal.numBytesDone)
	{
		outStream << boost::format(Statistics::phaseResultsFormatStr)
			% ""
			% "Total MiB"
			% ":"
			% (phaseResults.opsStoneWallTotal.numBytesDone / (1024*1024) )
			% (phaseResults.opsTotal.numBytesDone / (1024*1024) )
			<< std::endl;
	}

	// rwmix read sum of bytes
	if(phaseResults.opsRWMixReadTotal.numBytesDone)
	{
		outStream << boost::format(Statistics::phaseResultsFormatStr)
			% ""
			% "Read MiB"
			% ":"
			% (phaseResults.opsStoneWallRWMixReadTotal.numBytesDone / (1024*1024) )
			% (phaseResults.opsRWMixReadTotal.numBytesDone / (1024*1024) )
			<< std::endl;
	}

	// IOs (number of blocks read/written)
	if(phaseResults.opsTotal.numIOPSDone)
	{
		if(progArgs.getLogLevel() > Log_NORMAL)
			outStream << boost::format(Statistics::phaseResultsFormatStr)
				% ""
				% "IOs total"
				% ":"
				% phaseResults.opsStoneWallTotal.numIOPSDone
				% phaseResults.opsTotal.numIOPSDone
				<< std::endl;
	}

	// rwmix read IOs (number of blocks read)
	if(phaseResults.opsRWMixReadTotal.numIOPSDone)
	{
		if(progArgs.getLogLevel() > Log_NORMAL)
			outStream << boost::format(Statistics::phaseResultsFormatStr)
				% ""
				% "IOs read"
				% ":"
				% phaseResults.opsStoneWallRWMixReadTotal.numIOPSDone
				% phaseResults.opsRWMixReadTotal.numIOPSDone
				<< std::endl;
	}

	// print individual elapsed time results for each worker
	if(progArgs.getShowAllElapsed() )
	{
		// individual results header (note: keep format in sync with general table format string)
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

	// entries & iops latency results
	printPhaseResultsLatencyToStream(phaseResults.entriesLatHisto, entryTypeUpperCase, outStream);
	printPhaseResultsLatencyToStream(phaseResults.iopsLatHisto, "IO", outStream);

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

	// rwmix read IOPS

	outLabelsVec.push_back("rwmix read IOPS [first]");
	outResultsVec.push_back(!phaseResults.opsRWMixReadTotal.numIOPSDone ?
		"" : std::to_string(phaseResults.opsStoneWallRWMixReadPerSec.numIOPSDone) );

	outLabelsVec.push_back("rwmix read IOPS [last]");
	outResultsVec.push_back(!phaseResults.opsRWMixReadTotal.numIOPSDone ?
		"" : std::to_string(phaseResults.opsRWMixReadPerSec.numIOPSDone) );


	// rwmix read MiB/s

	outLabelsVec.push_back("rwmix read MiB/s [first]");
	outResultsVec.push_back(!phaseResults.opsRWMixReadTotal.numBytesDone ?
		"" : std::to_string(phaseResults.opsStoneWallRWMixReadPerSec.numBytesDone / (1024*1024) ) );

	outLabelsVec.push_back("rwmix read MiB/s [last]");
	outResultsVec.push_back(!phaseResults.opsRWMixReadTotal.numBytesDone ?
		"" : std::to_string(phaseResults.opsRWMixReadPerSec.numBytesDone / (1024*1024) ) );
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
			% (latTypeStr + " latency us")
			% ":";

		outStream <<
			"[ " <<
			"min=" << latHisto.getMinMicroSecLat() << " "
			"avg=" << latHisto.getAverageMicroSec() << " "
			"max=" << latHisto.getMaxMicroSecLat() <<
			" ]" << std::endl;
	}

	// print IO latency percentiles
	if(progArgs.getShowLatencyPercentiles() && latHisto.getNumStoredValues() )
	{
		// individual results header (note: keep format in sync with general table format string)
		outStream << boost::format(Statistics::phaseResultsLeftFormatStr)
			% ""
			% (latTypeStr + " lat %ile us")
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
			% (latTypeStr + " lat histo")
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

/**
 * Get results of a completed benchmark phase.
 */
void Statistics::getBenchResultAsPropertyTree(bpt::ptree& outTree)
{
	LiveOps liveOps;
	LiveOps liveRWMixReadOps;
	LatencyHistogram iopsLatHisto; // sum of all histograms
	LatencyHistogram entriesLatHisto; // sum of all histograms

	getLiveOps(liveOps, liveRWMixReadOps);

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

	for(Worker* worker : workerVec)
	{
		// add finishElapsedUSec of each worker
		for(uint64_t elapsedUSec : worker->getElapsedUSecVec() )
			outTree.add(XFER_STATS_ELAPSEDUSECLIST_ITEM, elapsedUSec);

		iopsLatHisto += worker->getIOPSLatencyHistogram();
		entriesLatHisto += worker->getEntriesLatencyHistogram();
	}

	iopsLatHisto.getAsPropertyTree(outTree, XFER_STATS_LAT_PREFIX_IOPS);
	entriesLatHisto.getAsPropertyTree(outTree, XFER_STATS_LAT_PREFIX_ENTRIES);

	if( (workersSharedData.currentBenchPhase == BenchPhase_CREATEFILES) &&
		(progArgs.getRWMixPercent() || progArgs.getNumS3RWMixReadThreads() ) )
	{
		outTree.put(XFER_STATS_NUMBYTESDONE_RWMIXREAD, liveRWMixReadOps.numBytesDone);
		outTree.put(XFER_STATS_NUMIOPSDONE_RWMIXREAD, liveRWMixReadOps.numIOPSDone);
	}

	outTree.put(XFER_STATS_ERRORHISTORY, LoggerBase::getErrHistory() );
}

/**
 * Check if CSV file is empty or not existing yet.
 *
 * @return true if not exists or empty (or if size could not be retrieved), false otherwise
 */
bool Statistics::checkCSVFileEmpty()
{
	struct stat statBuf;
	std::string csvFilePath = progArgs.getCSVFilePath();

	int statRes = stat(csvFilePath.c_str(), &statBuf);
	if(statRes == -1)
	{
		if(errno == ENOENT)
			return true;

		std::cerr << "ERROR: Getting CSV file size failed. "
			"Path: " << csvFilePath <<
			"SysErr: " << strerror(errno) << std::endl;

		return true;
	}

	return (statBuf.st_size == 0);
}

/**
 * Print dry run info (number of entries and dataset size) for each of the user-given phases.
 */
void Statistics::printDryRunInfo()
{
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
