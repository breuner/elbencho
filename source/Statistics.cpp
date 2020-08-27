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
	std::string entryTypeUpperCase =
		TranslatorTk::benchPhaseToPhaseEntryType(workersSharedData.currentBenchPhase, true);
	time_t startTime = time(NULL);
	LiveOps lastLiveOps = {};
	size_t numWorkersDone;
	int winHeight, winWidth;
	int numFixedHeaderLines = 3; // to calc how many lines are left to show per-worker stats
	size_t maxNumWorkerLines; // how many lines we have to show per-worker stats
	size_t numEntriesPerWorker; // total number for this phase
	uint64_t numBytesPerWorker; // total number for this phase

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

		const char tableHeadlineFormat[] = "%|5| %|3| %|10| %|10| %|10|";
		const char dirModeTableHeadlineFormat[] = " %|10| %|10|"; // appended to standard table fmt
		const char remoteTableHeadlineFormat[] = " %|4| %||"; // appended to standard table format

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
				% entryTypeUpperCase
				% (entryTypeUpperCase + "/s");
		}

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
			% (newLiveOps.numBytesDone / (1024*1024) )
			% (liveOpsPerSec.numBytesDone / (1024*1024) )
			% liveOpsPerSec.numIOPSDone;

		if(progArgs.getBenchPathType() == BenchPathType_DIR)
		{ // add columns for dir mode
			stream << boost::format(dirModeTableHeadlineFormat)
				% newLiveOps.numEntriesDone
				% liveOpsPerSec.numEntriesDone;
		}

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
	if(!progArgs.getCSVFilePath().empty() && progArgs.getPrintCSVLabels() )
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
	IF_UNLIKELY(workerVec.empty() || workerVec[0]->getElapsedMSVec().empty() )
		return false;

	// init first/last elapsed milliseconds
	phaseResults.firstFinishMS =
		workerVec[0]->getElapsedMSVec()[0]; // stonewall: time to completion of fastest worker
	phaseResults.lastFinishMS =
		workerVec[0]->getElapsedMSVec()[0]; // time to completion of slowest worker

	// sum up total values
	for(Worker* worker : workerVec)
	{
		IF_UNLIKELY(worker->getElapsedMSVec().empty() )
			return false;

		for(size_t elapsedMS : worker->getElapsedMSVec() )
		{
			if(elapsedMS < phaseResults.firstFinishMS)
				phaseResults.firstFinishMS = elapsedMS;

			if(elapsedMS > phaseResults.lastFinishMS)
				phaseResults.lastFinishMS = elapsedMS;
		}

		worker->getAndAddLiveOps(phaseResults.opsTotal);
		worker->getAndAddStoneWallOps(phaseResults.opsStoneWallTotal);
		phaseResults.iopsLatHisto += worker->getIOPSLatencyHistogram();
		phaseResults.entriesLatHisto += worker->getEntriesLatencyHistogram();

	} // end of for loop

	// total per sec for all workers by 1st finisher
	if(phaseResults.firstFinishMS)
	{
		phaseResults.opsStoneWallPerSec = phaseResults.opsStoneWallTotal;
		phaseResults.opsStoneWallPerSec *= 1000;
		phaseResults.opsStoneWallPerSec /= phaseResults.firstFinishMS;
	}

	// total per sec for all workers by last finisher
	if(phaseResults.lastFinishMS)
	{
		phaseResults.opsPerSec = phaseResults.opsTotal;
		phaseResults.opsPerSec *= 1000;
		phaseResults.opsPerSec /= phaseResults.lastFinishMS;
	}

	// convert to per-worker values depending on user setting
	if(progArgs.getShowPerThreadStats() )
	{
		/* note: service hosts are aware of per-thread value and provide their local per-thread
			result. that's why we div by workerVec.size() instead of by numIOWorkersTotal. */
		phaseResults.opsStoneWallPerSec /= workerVec.size();
		phaseResults.opsPerSec /= workerVec.size();
		phaseResults.opsStoneWallTotal /= workerVec.size();
		phaseResults.opsTotal /= workerVec.size();
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
		TranslatorTk::benchPhaseToPhaseName(workersSharedData.currentBenchPhase);
	std::string entryTypeUpperCase =
		TranslatorTk::benchPhaseToPhaseEntryType(workersSharedData.currentBenchPhase, true);
	std::string entryTypeLowerCase =
		TranslatorTk::benchPhaseToPhaseEntryType(workersSharedData.currentBenchPhase, false);

	// elapsed time
	outStream << boost::format(Statistics::phaseResultsFormatStr)
		% phaseName
		% "Elapsed ms"
		% ":"
		% phaseResults.firstFinishMS
		% phaseResults.lastFinishMS
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
			for(size_t elapsedMS : worker->getElapsedMSVec() )
				outStream << elapsedMS << " ";
		}

		outStream << "]" << std::endl;
	}

	// entries & iops latency results
	printPhaseResultsLatencyToStream(phaseResults.entriesLatHisto, entryTypeUpperCase, outStream);
	printPhaseResultsLatencyToStream(phaseResults.iopsLatHisto, "IO", outStream);

	// warn in case of invalid results
	if( (phaseResults.firstFinishMS == 0) && !progArgs.getIgnore0MSErrors() )
	{
		/* very fast completion, so notify user about possibly useless results (due to accuracy,
			but also because we show 0 op/s to avoid division by 0 */

		outStream << "WARNING: Fastest worker thread completed in less than 1 millisecond, "
			"so results might not be useful (some op/s are shown as 0). You might want to try a "
			"larger data set. Otherwise, option '--" ARG_IGNORE0MSERR_LONG "' disables this "
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
		TranslatorTk::benchPhaseToPhaseName(workersSharedData.currentBenchPhase);

	outLabelsVec.push_back("operation");
	outResultsVec.push_back(phaseName);

	// elapsed time

	outLabelsVec.push_back("time ms [first]");
	outResultsVec.push_back(std::to_string(phaseResults.firstFinishMS) );

	outLabelsVec.push_back("time ms [last]");
	outResultsVec.push_back(std::to_string(phaseResults.lastFinishMS) );

	// entries per second

	outLabelsVec.push_back("entries/s [first]");
	outResultsVec.push_back(!phaseResults.opsTotal.numEntriesDone ?
		"" : std::to_string(phaseResults.opsStoneWallPerSec.numEntriesDone) );

	outLabelsVec.push_back("entries/s [last]");
	outResultsVec.push_back(!phaseResults.opsTotal.numEntriesDone ?
		"" : std::to_string(phaseResults.opsPerSec.numEntriesDone) );

	// entries

	outLabelsVec.push_back("entries [first]");
	outResultsVec.push_back(!phaseResults.opsTotal.numEntriesDone ?
		"" : std::to_string(phaseResults.opsStoneWallTotal.numEntriesDone) );

	outLabelsVec.push_back("entries [last]");
	outResultsVec.push_back(!phaseResults.opsTotal.numEntriesDone ?
		"" : std::to_string(phaseResults.opsTotal.numEntriesDone) );

	// IOPS

	outLabelsVec.push_back("IOPS [first]");
	outResultsVec.push_back(!phaseResults.opsTotal.numIOPSDone ?
		"" : std::to_string(phaseResults.opsStoneWallPerSec.numIOPSDone) );

	outLabelsVec.push_back("IOPS [last]");
	outResultsVec.push_back(!phaseResults.opsTotal.numIOPSDone ?
		"" : std::to_string(phaseResults.opsPerSec.numIOPSDone) );

	// I/Os

	outLabelsVec.push_back("I/Os [first]");
	outResultsVec.push_back(!phaseResults.opsTotal.numIOPSDone ?
		"" : std::to_string(phaseResults.opsStoneWallTotal.numIOPSDone) );

	outLabelsVec.push_back("I/Os [last]");
	outResultsVec.push_back(!phaseResults.opsTotal.numIOPSDone ?
		"" : std::to_string(phaseResults.opsTotal.numIOPSDone) );

	// MiB/s

	outLabelsVec.push_back("MiB/s [first]");
	outResultsVec.push_back(!phaseResults.opsTotal.numBytesDone ?
		"" : std::to_string(phaseResults.opsStoneWallPerSec.numBytesDone / (1024*1024) ) );

	outLabelsVec.push_back("MiB/s [last]");
	outResultsVec.push_back(!phaseResults.opsTotal.numBytesDone ?
		"" : std::to_string(phaseResults.opsPerSec.numBytesDone / (1024*1024) ) );

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
		TranslatorTk::benchPhaseToPhaseName(workersSharedData.currentBenchPhase);

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
			% (latTypeStr + " lat %ile")
			% ":";

		outStream << "[ ";

		if(latHisto.getHistogramExceeded() )
			outStream << "Histogram exceeded";
		else
			outStream <<
				"1%<=" << latHisto.getPercentileStr(1) << " "
				"50%<=" << latHisto.getPercentileStr(50) << " "
				"75%<=" << latHisto.getPercentileStr(75) << " "
				"99%<=" << latHisto.getPercentileStr(99);

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
