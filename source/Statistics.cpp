#include <boost/format.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <chrono>
#include <ncurses.h>
#include "LocalWorker.h"
#include "ProgException.h"
#include "RemoteWorker.h"
#include "Statistics.h"
#include "Terminal.h"
#include "TranslatorTk.h"
#include "Worker.h"
#include "WorkerException.h"

#define CONTROLCHARS_CLEARLINE_AND_CARRIAGERETURN	"\x1B[2K\r" /* "\x1B[2K" is the VT100 code to
										clear the line. "\r" moves cursor to beginning of line. */


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
	LiveOps& liveOpsPerSec, LiveOps& liveOps, unsigned long long numWorkersLeft,
	unsigned long long elapsedSec)
{
	const size_t hiddenControlCharsLen = strlen(CONTROLCHARS_CLEARLINE_AND_CARRIAGERETURN);

	// prepare line including control chars to clear line and do carriage return
	std::ostringstream stream;

	if(progArgs.getBenchPathType() == BenchPathType_DIR)
		stream << CONTROLCHARS_CLEARLINE_AND_CARRIAGERETURN <<
			phaseName << ": " <<
			liveOpsPerSec.numEntriesDone << " " << phaseEntryType << "/s; " <<
			liveOpsPerSec.numBytesDone / (1024*1024) << " MiB/s; " <<
			liveOps.numEntriesDone << " " << phaseEntryType << "; " <<
			liveOps.numBytesDone / (1024*1024) << " MiB; " <<
			numWorkersLeft << " threads; " <<
			elapsedSec << "s";
	else // show IOPS instead of files/dirs per sec
		stream << CONTROLCHARS_CLEARLINE_AND_CARRIAGERETURN <<
			phaseName << ": " <<
			liveOpsPerSec.numIOPSDone << " " << "IOPS; " <<
			liveOpsPerSec.numBytesDone / (1024*1024) << " MiB/s; " <<
			liveOps.numBytesDone / (1024*1024) << " MiB; " <<
			numWorkersLeft << " threads; " <<
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
		workersSharedData.currentBenchPhase);
	std::string phaseEntryType = TranslatorTk::benchPhaseToPhaseEntryType(
		workersSharedData.currentBenchPhase);
	time_t startTime = time(NULL);
	LiveOps lastLiveOps = {};
	size_t numWorkersDone;

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

		// calc numWorkersLeft
		if(progArgs.getHostsVec().empty() )
		{ // local mode
			numWorkersLeft = workerVec.size() - numWorkersDone;
		}
		else
		{ // remote mode
			numWorkersLeft = progArgs.getHostsVec().size() * progArgs.getNumThreads();

			for(Worker* worker : workerVec)
			{
				RemoteWorker* remoteWorker =  static_cast<RemoteWorker*>(worker);
				numWorkersLeft -= remoteWorker->getNumWorkersDone();
				numWorkersLeft -= remoteWorker->getNumWorkersDoneWithError();
			}
		}

		LiveOps newLiveOps;

		getLiveOps(newLiveOps);

		LiveOps opsPerSec = newLiveOps - lastLiveOps;
		opsPerSec /= progArgs.getLiveStatsSleepSec();

		lastLiveOps = newLiveOps;

		time_t elapsedSec = time(NULL) - startTime;

		printSingleLineLiveStatsLine(phaseName.c_str(), phaseEntryType.c_str(),
			opsPerSec, newLiveOps, numWorkersLeft, elapsedSec);
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
	std::string phaseName = TranslatorTk::benchPhaseToPhaseName(
		workersSharedData.currentBenchPhase);
	std::string phaseEntryType = TranslatorTk::benchPhaseToPhaseEntryType(
		workersSharedData.currentBenchPhase);
	time_t startTime = time(NULL);
	LiveOps lastLiveOps = {};
	size_t numWorkersDone;
	int winHeight, winWidth;
	int numFixedHeaderLines = 3; // to calc how many lines are left to show per-worker stats
	size_t maxNumWorkerLines; // how many lines we have to show per-worker stats
	size_t numEntriesPerWorker; // total number for this phase
	size_t numBytesPerWorker; // total number for this phase

	workerManager.getPhaseNumEntriesAndBytes(numEntriesPerWorker, numBytesPerWorker);

	while(true)
	{
		std::unique_lock<std::mutex> lock(workersSharedData.mutex); // L O C K (scoped)

		try
		{
			bool workersDone = workersSharedData.condition.wait_for(
				lock, std::chrono::seconds(progArgs.getLiveStatsSleepSec() ),
				[&, this]{ return workerManager.checkWorkersDoneUnlocked(&numWorkersDone); } );
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

		workerManager.checkPhaseTimeLimit();

		size_t numRemoteThreadsLeft; // only set in remote mode

		if(!progArgs.getHostsVec().empty() )
		{ // remote mode
			size_t numRemoteThreadsTotal = workerVec.size() * progArgs.getNumThreads();
			numRemoteThreadsLeft = numRemoteThreadsTotal;

			for(Worker* worker : workerVec)
				numRemoteThreadsLeft -= static_cast<RemoteWorker*>(worker)->getNumWorkersDone();
		}

		LiveOps newLiveOps;

		getLiveOps(newLiveOps);

		LiveOps liveOpsPerSec = newLiveOps - lastLiveOps;
		liveOpsPerSec /= progArgs.getLiveStatsSleepSec();

		lastLiveOps = newLiveOps;

		// if we have bytes in this phase, use them for percent done; otherwise use num entries
		size_t percentDone = numBytesPerWorker ?
			(100 * newLiveOps.numBytesDone) / (numBytesPerWorker * workerVec.size() ) :
			(100 * newLiveOps.numEntriesDone) / (numEntriesPerWorker * workerVec.size() );

		time_t elapsedSec = time(NULL) - startTime;

		getmaxyx(stdscr, winHeight, winWidth); // get current screen dimensions

		maxNumWorkerLines = winHeight - numFixedHeaderLines;

		// clear screen buffer and screen. (screen will be cleared when refresh() is called.)
		clear();

		// fill curses buffer...
		std::ostringstream stream;

		stream << boost::format("Phase: %||  Active: %||  Elapsed: %||s")
			% phaseName
			% (workerVec.size() - numWorkersDone)
			% elapsedSec;

		printWholeScreenLine(stream, winWidth, false);

		const char tableHeadlineFormat[] = "%|5| %|3| %|10| %|10| %|10| %|10| %|10|";
		const char remoteTableHeadlineFormat[] = " %|4| %||"; // appended to standard table format

		attron(A_STANDOUT); // highlight table headline
		stream << boost::format(tableHeadlineFormat)
			% "Rank"
			% "%"
			% "Done"
			% "Done/s"
			% "DoneMiB"
			% "MiB/s"
			% "IOPS";

		if(!progArgs.getHostsVec().empty() )
		{ // add columns for remote mode
			stream << boost::format(remoteTableHeadlineFormat)
				% "Act"
				% "Service";
		}

		printWholeScreenLine(stream, winWidth, true);
		attroff(A_STANDOUT); // disable highlighting

		stream << boost::format(tableHeadlineFormat)
			% "Total"
			% percentDone
			% newLiveOps.numEntriesDone
			% liveOpsPerSec.numEntriesDone
			% (newLiveOps.numBytesDone / (1024*1024) )
			% (liveOpsPerSec.numBytesDone / (1024*1024) )
			% liveOpsPerSec.numIOPSDone;

		if(!progArgs.getHostsVec().empty() )
		{ // add columns for remote mode
			stream << boost::format(remoteTableHeadlineFormat)
				% numRemoteThreadsLeft
				% "";
		}

		printWholeScreenLine(stream, winWidth, true);

		// print line for each worker
		for(size_t i=0; i < std::min(workerVec.size(), maxNumWorkerLines); i++)
		{
			LiveOps workerDone; // total numbers
			LiveOps workerDonePerSec; // difference to last round

			workerVec[i]->getLiveOps(workerDone);
			workerVec[i]->getAndResetDiffStats(workerDonePerSec);

			workerDonePerSec /= progArgs.getLiveStatsSleepSec();

			// if we have bytes in this phase, use them for percent done; otherwise use num entries
			size_t workerPercentDone = numBytesPerWorker ?
				(100 * workerDone.numBytesDone) / numBytesPerWorker :
				(100 * workerDone.numEntriesDone) / numEntriesPerWorker;

			stream << boost::format(tableHeadlineFormat)
				% i
				% workerPercentDone
				% workerDone.numEntriesDone
				% workerDonePerSec.numEntriesDone
				% (workerDone.numBytesDone / (1024*1024) )
				% (workerDonePerSec.numBytesDone / (1024*1024) )
				% workerDonePerSec.numIOPSDone;

			if(!progArgs.getHostsVec().empty() )
			{ // add columns for remote mode
				stream << boost::format(remoteTableHeadlineFormat)
					% (progArgs.getNumThreads() -
						static_cast<RemoteWorker*>(workerVec[i])->getNumWorkersDone() )
					% progArgs.getHostsVec()[i];
			}

			printWholeScreenLine(stream, winWidth, true);
		}

		// print prepared buffer to screen
		refresh();
	}


workers_done:

	if(ncursesInitialized)
		endwin(); // end curses mode
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
 * @usePerThreadValues true to div numEntriesDone/numBytesDone by number of workers.
 */
void Statistics::getLiveStatsAsPropertyTree(bpt::ptree& outTree)
{
	LiveOps liveOps;

	getLiveOps(liveOps);

	std::chrono::seconds elapsedDurationSecs =
				std::chrono::duration_cast<std::chrono::seconds>
				(std::chrono::steady_clock::now() - workersSharedData.phaseStartT);
	size_t elapsedSecs = elapsedDurationSecs.count();

	outTree.put(XFER_STATS_BENCHID, workersSharedData.currentBenchID);
	outTree.put(XFER_STATS_BENCHPHASENAME,
		TranslatorTk::benchPhaseToPhaseName(workersSharedData.currentBenchPhase) );
	outTree.put(XFER_STATS_BENCHPHASECODE, workersSharedData.currentBenchPhase);
	outTree.put(XFER_STATS_NUMWORKERSDONE, workersSharedData.numWorkersDone);
	outTree.put(XFER_STATS_NUMWORKERSDONEWITHERR, workersSharedData.numWorkersDoneWithError);
	outTree.put(XFER_STATS_NUMENTRIESDONE, liveOps.numEntriesDone);
	outTree.put(XFER_STATS_NUMBYTESDONE, liveOps.numBytesDone);
	outTree.put(XFER_STATS_NUMIOPSDONE, liveOps.numIOPSDone);
	outTree.put(XFER_STATS_ELAPSEDSECS, elapsedSecs);
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

	// print to results file (if specified by user)
	if(!progArgs.getResFilePath().empty() )
	{
		std::ofstream outfile;

		outfile.open(progArgs.getResFilePath(), std::ofstream::app);

		if(!outfile)
		{
			std::cerr << "ERROR: Opening results file failed: " << progArgs.getResFilePath() <<
				std::endl;

			return;
		}

		// print current date

		std::time_t currentTime = std::time(0);

		outfile << "DATE: " << std::ctime(&currentTime); // includes newline

		// print command line

		outfile << "COMMAND LINE: ";

		for(int i=0; i < progArgs.getProgArgCount(); i++)
			outfile << "\"" << progArgs.getProgArgVec()[i] << "\" ";

		outfile << std::endl;


		printPhaseResultsTableHeaderToStream(outfile);
		outfile.close();

		if(!outfile)
		{
			std::cerr << "ERROR: Writing to results file failed: " << progArgs.getResFilePath() <<
				std::endl;

			return;
		}
}
}

/**
 * Print table header for phase results.
 *
 * @outstream where to print results to.
 */
void Statistics::printPhaseResultsTableHeaderToStream(std::ostream& outstream)
{
	outstream << boost::format(phaseResultsFormatStr)
		% "OPERATION"
		% "RESULT TYPE"
		% ""
		% "FIRST DONE"
		% "LAST DONE"
		<< std::endl;
	outstream << boost::format(phaseResultsFormatStr)
		% "========="
		% "==========="
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
	// print to console
	printPhaseResultsToStream(std::cout);

	// print to results file (if specified by user)
	if(!progArgs.getResFilePath().empty() )
	{
		std::ofstream outfile;

		outfile.open(progArgs.getResFilePath(), std::ofstream::app);

		if(!outfile)
		{
			std::cerr << "ERROR: Opening results file failed: " << progArgs.getResFilePath() <<
				std::endl;

			return;
		}

		printPhaseResultsToStream(outfile);
	}
}

/**
 * Gather and print statistics after all workers completed a phase.
 *
 * @outstream where to print results to.
 */
void Statistics::printPhaseResultsToStream(std::ostream& outstream)
{
	std::string phaseName =
		TranslatorTk::benchPhaseToPhaseName(workersSharedData.currentBenchPhase);
	size_t elapsedMSTotal = 0; // sum of elapsed time of all workers (for average calculation)
	LiveOps opsTotal = {}; // processed by all workers
	LiveOps opsStoneWallTotal = {}; // processed by all workers when stonewall was hit
	size_t numIOWorkersTotal = 0; /* sum of all I/O threads (for local: all workers; for remote:
		all workers on the remote hosts) */
	LatencyHistogram iopsLatHisto; // sum of all histograms
	LatencyHistogram entriesLatHisto; // sum of all histograms

	// check if results initialized. uninitialized can happen if called through HTTP before 1st run
	IF_UNLIKELY(workerVec.empty() || workerVec[0]->getElapsedMSVec().empty() )
	{
		outstream << "Skipping stats print due to unavailable worker results." << std::endl;
		return;
	}

	// init first/last elapsed milliseconds (note: at() as sanity check here).
	size_t firstFinishMS =
		workerVec[0]->getElapsedMSVec()[0]; // stonewall: time to completion of fastest worker
	size_t lastFinishMS =
		workerVec[0]->getElapsedMSVec()[0]; // time to completion of slowest worker

	// sum up total values
	for(Worker* worker : workerVec)
	{
		IF_UNLIKELY(worker->getElapsedMSVec().empty() )
		{
			outstream << "Skipping stats print due to unavailable worker results." << std::endl;
			return;
		}

		for(size_t elapsedMS : worker->getElapsedMSVec() )
		{
			elapsedMSTotal += elapsedMS;
			numIOWorkersTotal++;

			if(elapsedMS < firstFinishMS)
				firstFinishMS = elapsedMS;

			if(elapsedMS > lastFinishMS)
				lastFinishMS = elapsedMS;
		}

		worker->getAndAddLiveOps(opsTotal);
		worker->getAndAddStoneWallOps(opsStoneWallTotal);
		iopsLatHisto += worker->getIOPSLatencyHistogram();
		entriesLatHisto += worker->getEntriesLatencyHistogram();

	} // end of for loop

	LiveOps opsStoneWallPerSec = {}; // total per sec for all workers by 1st finisher
	if(firstFinishMS)
	{
		opsStoneWallPerSec = opsStoneWallTotal;
		opsStoneWallPerSec *= 1000;
		opsStoneWallPerSec /= firstFinishMS;
	}

	LiveOps opsPerSec = {}; // total per sec for all workers by last finisher
	if(lastFinishMS)
	{
		opsPerSec = opsTotal;
		opsPerSec *= 1000;
		opsPerSec /= lastFinishMS;
	}

	// convert to per-worker values depending on user setting
	if(progArgs.getShowPerThreadStats() )
	{
		/* note: service hosts are aware of per-thread value and provide their local per-thread
			result. that's why we div by workerVec.size() instead of by numIOWorkersTotal. */
		opsStoneWallPerSec /= workerVec.size();
		opsPerSec /= workerVec.size();
		opsStoneWallTotal /= workerVec.size();
		opsTotal /= workerVec.size();
	}


	// elapsed time
	outstream << boost::format(Statistics::phaseResultsFormatStr)
		% phaseName
		% "time ms"
		% ":"
		% firstFinishMS
		% lastFinishMS
		<< std::endl;

	// entries per second
	if(opsTotal.numEntriesDone)
	{
		outstream << boost::format(Statistics::phaseResultsFormatStr)
			% phaseName
			% "entries/s"
			% ":"
			% opsStoneWallPerSec.numEntriesDone
			% opsPerSec.numEntriesDone
			<< std::endl;

		outstream << boost::format(Statistics::phaseResultsFormatStr)
			% phaseName
			% "entries"
			% ":"
			% opsStoneWallTotal.numEntriesDone
			% opsTotal.numEntriesDone
			<< std::endl;
	}

	// IOPS
	if(opsTotal.numIOPSDone)
	{
		outstream << boost::format(Statistics::phaseResultsFormatStr)
			% phaseName
			% "IOPS"
			% ":"
			% opsStoneWallPerSec.numIOPSDone
			% opsPerSec.numIOPSDone
			<< std::endl;

		outstream << boost::format(Statistics::phaseResultsFormatStr)
			% phaseName
			% "I/Os"
			% ":"
			% opsStoneWallTotal.numIOPSDone
			% opsTotal.numIOPSDone
			<< std::endl;
	}

	// bytes per second total for all workers
	// (only show in phases where data has been written/read)
	if(opsTotal.numBytesDone)
	{
		outstream << boost::format(Statistics::phaseResultsFormatStr)
			% phaseName
			% "MiB/s"
			% ":"
			% (opsStoneWallPerSec.numBytesDone / (1024*1024) )
			% (opsPerSec.numBytesDone / (1024*1024) )
			<< std::endl;

		outstream << boost::format(Statistics::phaseResultsFormatStr)
			% phaseName
			% "MiB"
			% ":"
			% (opsStoneWallTotal.numBytesDone / (1024*1024) )
			% (opsTotal.numBytesDone / (1024*1024) )
			<< std::endl;
	}

	// print individual elapsed time results for each worker
	if(progArgs.getShowAllElapsed() )
	{
		// individual results header (note: keep format in sync with general table format string)
		outstream << boost::format(Statistics::phaseResultsLeftFormatStr)
			% phaseName
			% "time ms each"
			% ":";

		outstream << "[ ";

		// print elapsed milliseconds of each I/O thread
		for(Worker* worker : workerVec)
		{
			for(size_t elapsedMS : worker->getElapsedMSVec() )
				outstream << elapsedMS << " ";
		}

		outstream << "]" << std::endl;
	}

	// entries & iops latency results
	printPhaseResultsLatency(entriesLatHisto, "Ent", outstream);
	printPhaseResultsLatency(iopsLatHisto, "IO", outstream);

	// warn in case of invalid results
	if( (firstFinishMS == 0) && !progArgs.getIgnore0MSErrors() )
	{
		/* very fast completion, so notify user about possibly useless results (due to accuracy,
			but also because we show 0 op/s to avoid division by 0 */

		LOGGER(Log_NORMAL, "WARNING: Fastest worker thread completed in less than 1 millisecond, "
			"so results might not be useful (some op/s are shown as 0). You might want to try a "
			"larger data set. Otherwise, option '--" ARG_IGNORE0MSERR_LONG "' disables this "
			"check.)" << std::endl);
	}
}

/**
 * Print latency results as sub-task of printPhaseResults().
 *
 * @latHisto the latency histogram for which to print the results.
 * @latTypeStr short latency type description, e.g. for entries or iops.
 * @outstream where to print results to.
 */
void Statistics::printPhaseResultsLatency(LatencyHistogram& latHisto, std::string latTypeStr,
	std::ostream& outstream)
{
	std::string phaseName =
		TranslatorTk::benchPhaseToPhaseName(workersSharedData.currentBenchPhase);

	// print IO latency min/avg/max
	if(progArgs.getShowLatency() && latHisto.getNumStoredValues() )
	{
		// individual results header (note: keep format in sync with general table format string)
		outstream << boost::format(Statistics::phaseResultsLeftFormatStr)
			% phaseName
			% (latTypeStr + " lat us")
			% ":";

		outstream <<
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
		outstream << boost::format(Statistics::phaseResultsLeftFormatStr)
			% phaseName
			% (latTypeStr + " lat %ile")
			% ":";

		outstream << "[ ";

		if(latHisto.getHistogramExceeded() )
			outstream << "Histogram exceeded";
		else
			outstream <<
				"1%<=" << latHisto.getPercentileStr(1) << " "
				"50%<=" << latHisto.getPercentileStr(50) << " "
				"75%<=" << latHisto.getPercentileStr(75) << " "
				"99%<=" << latHisto.getPercentileStr(99);

		outstream << " ]" << std::endl;
	}

	// print IO latency histogram
	if(progArgs.getShowLatencyHistogram() && latHisto.getNumStoredValues() )
	{
		// individual results header (note: keep format in sync with general table format string)
		outstream << boost::format(Statistics::phaseResultsLeftFormatStr)
			% phaseName
			% (latTypeStr + " lat histo")
			% ":";

		outstream << "[ ";

		outstream << latHisto.getHistogramStr();

		outstream << " ]" << std::endl;
	}
}

/**
 * @usePerThreadValues true to div numEntriesDone/numBytesDone by number of workers.
 */
void Statistics::getBenchResultAsPropertyTree(bpt::ptree& outTree)
{
	LiveOps liveOps;
	LatencyHistogram iopsLatHisto; // sum of all histograms
	LatencyHistogram entriesLatHisto; // sum of all histograms

	getLiveOps(liveOps);

	if(progArgs.getShowPerThreadStats() )
		liveOps /= workerVec.size(); // divide by number of threads for per-thread values

	outTree.put(XFER_STATS_BENCHID, workersSharedData.currentBenchID);
	outTree.put(XFER_STATS_BENCHPHASENAME,
		TranslatorTk::benchPhaseToPhaseName(workersSharedData.currentBenchPhase) );
	outTree.put(XFER_STATS_BENCHPHASECODE, workersSharedData.currentBenchPhase);
	outTree.put(XFER_STATS_NUMWORKERSDONE, workersSharedData.numWorkersDone);
	outTree.put(XFER_STATS_NUMWORKERSDONEWITHERR, workersSharedData.numWorkersDoneWithError);
	outTree.put(XFER_STATS_NUMENTRIESDONE, liveOps.numEntriesDone);
	outTree.put(XFER_STATS_NUMBYTESDONE, liveOps.numBytesDone);
	outTree.put(XFER_STATS_NUMIOPSDONE, liveOps.numIOPSDone);

	for(Worker* worker : workerVec)
	{
		// add finishElapsedMS of each worker
		for(size_t elapsedMS : worker->getElapsedMSVec() )
			outTree.add(XFER_STATS_ELAPSEDMSLIST_ITEM, elapsedMS);

		iopsLatHisto += worker->getIOPSLatencyHistogram();
		entriesLatHisto += worker->getEntriesLatencyHistogram();
	}

	iopsLatHisto.getAsPropertyTree(outTree, XFER_STATS_LAT_PREFIX_IOPS);
	entriesLatHisto.getAsPropertyTree(outTree, XFER_STATS_LAT_PREFIX_ENTRIES);

	outTree.put(XFER_STATS_ERRORHISTORY, LoggerBase::getErrHistory() );
}
