#include <string>
#include <sstream>
#include "RemoteWorker.h"
#include "toolkits/TranslatorTk.h"
#include "WorkerException.h"
#include "WorkersSharedData.h"

#define THROW_WORKEREXCEPTION_OR_LOG_ERR(isThrowingAllowed, errorMsgStr) \
	do \
	{ \
		if(isThrowingAllowed) \
			throw WorkerException(errorMsgStr); \
		else \
			std::cerr << "Error: " << errorMsgStr << std::endl; \
	} while(0)



/**
 * Entry point for the thread.
 * Kick off the work that this worker has to do. Each phase is sychronized to wait for notification
 * by coordinator.
 */
void RemoteWorker::run()
{
	try
	{
		buuids::uuid currentBenchID = buuids::nil_uuid();

		// preparation phase
		applyNumaBinding();
		preparePhase();

		// signal coordinator that our preparations phase is done (and ignore elapsed ms)
		phaseFinished = true; // before incNumWorkersDone(), as Coordinator can reset after done inc
		incNumWorkersDone();

		for( ; ; )
		{
			// wait for coordinator to set new bench ID to signal us that we are good to start
			waitForNextPhase(currentBenchID);

			currentBenchID = workersSharedData->currentBenchID;

			switch(workersSharedData->currentBenchPhase)
			{
				case BenchPhase_TERMINATE:
				{
					/* interrupt remote threads and close open FDs on service host or make remote
						service quit if requested by user */
					interruptBenchPhase(false);
					return;
				} break;
				case BenchPhase_CREATEDIRS:
				case BenchPhase_DELETEDIRS:
				case BenchPhase_CREATEFILES:
				case BenchPhase_READFILES:
				case BenchPhase_DELETEFILES:
				case BenchPhase_SYNC:
				case BenchPhase_DROPCACHES:
				case BenchPhase_STATFILES:
				{
					startBenchPhase();

					try
					{
						waitForBenchPhaseCompletion(true);
					}
					catch(WorkerInterruptedException& e)
					{
						// whoever interrupted us will have a reason for it, so only debug log level
						ErrLogger(Log_DEBUG, false, false) << "Interrupted exception. " <<
							"Rank: " << workerRank << "; " <<
							"Host: " << host << std::endl;

						interruptBenchPhase(false);

						waitForBenchPhaseCompletion(false);

						// let coordinator know that we are done
						finishPhase(false);

						return;
					}
					catch(WorkerRemoteException& e)
					{ // service host worker encountered error, so try to clean up
						// actual error is in e.what(), so only debug log level
						ErrLogger(Log_DEBUG, false, false) << "Remote worker exception. " <<
							"Rank: " << workerRank << "; " <<
							"Host: " << host << std::endl;

						interruptBenchPhase(false);

						ErrLogger(Log_NORMAL, false, false) << e.what() << std::endl;

						incNumWorkersDoneWithError();

						return;
					}
				} break;
				default:
				{ // should never happen
					throw WorkerException("Unknown/invalid next phase type: " +
						std::to_string(workersSharedData->currentBenchPhase) );
				} break;

			} // end of switch

			// let coordinator know that we are done
			finishPhase(true);

		} // end of for loop

	}
	catch(WorkerInterruptedException& e)
	{
		// whoever interrupted us will have a reason for it, so only print debug level here
		ErrLogger(Log_DEBUG, false, false) << "Interrupted exception. " <<
			"Rank: " << workerRank << "; " <<
			"Host: " << host << std::endl;

		// interrupt to free service resources even if phaseFinished==true
		interruptBenchPhase(false);

		// return here to not increase error counter if service successfully finished its phase
		if(phaseFinished)
			return;
	}
	catch(std::exception& e)
	{
		ErrLogger(Log_NORMAL, false, false) << e.what() << std::endl;
	}

	incNumWorkersDoneWithError();
}

/**
 * This is the end of a successful benchmark phase. Update finish time values, then signal
 * coordinator that we completed successfully.
 *
 * @allowExceptionThrow false to log errors instead of throwing an exception.
 * @throw if allowExceptionThrow is true, WorkerException on error, e.g. http client problem
 */
void RemoteWorker::finishPhase(bool allowExceptionThrow)
{
	// note: finish time is calculated on remote workers, so nothing to do for that here

	// note: we want to print phase results even in case of ctrl+c, so no check for interrupt here

	try
	{
		auto response = httpClient.request("GET", HTTPCLIENTPATH_BENCHRESULT);

		IF_UNLIKELY(response->status_code != Web::status_code(Web::StatusCode::success_ok) )
		{
			Logger(Log_DEBUG) << "HTTP status code: " + response->status_code << std::endl;

			THROW_WORKEREXCEPTION_OR_LOG_ERR(allowExceptionThrow,
				"Service host encountered an error. "
				"Server: " + host + "; "
				"Phase: Finalization; "
				"Message: " + response->content.string() );
		}

		bpt::ptree resultTree;
		bpt::read_json(response->content, resultTree);

		buuids::string_generator uuidGen;
		buuids::uuid currentBenchID = uuidGen(resultTree.get<std::string>(XFER_STATS_BENCHID) );

		IF_UNLIKELY(workersSharedData->currentBenchID != currentBenchID)
			THROW_WORKEREXCEPTION_OR_LOG_ERR(allowExceptionThrow,
				"Service host got hijacked for a different benchmark. "
				"Server: " + host);

		numWorkersDone = resultTree.get<size_t>(XFER_STATS_NUMWORKERSDONE);
		numWorkersDoneWithError = resultTree.get<size_t>(XFER_STATS_NUMWORKERSDONEWITHERR);

		IF_UNLIKELY(numWorkersDoneWithError)
		{
			std::string errorMsg = resultTree.get<std::string>(XFER_STATS_ERRORHISTORY);
			THROW_WORKEREXCEPTION_OR_LOG_ERR(allowExceptionThrow, frameHostErrorMsg(errorMsg) );
		}

		IF_UNLIKELY(numWorkersDone < progArgs->getNumThreads() )
			THROW_WORKEREXCEPTION_OR_LOG_ERR(allowExceptionThrow,
				"Phase finish handler was called before all workers on service "
				"host completed. "
				"Server: " + host + "; "
				"numWorkersDone: " + host + "; "
				"numWorkersDoneWithError: " + host + "; "
				"numThreads: " + host + "; ");

		atomicLiveOps.numEntriesDone = resultTree.get<size_t>(XFER_STATS_NUMENTRIESDONE);
		atomicLiveOps.numBytesDone = resultTree.get<size_t>(XFER_STATS_NUMBYTESDONE);
		atomicLiveOps.numIOPSDone = resultTree.get<size_t>(XFER_STATS_NUMIOPSDONE);

		cpuUtil.stoneWall = resultTree.get<unsigned>(XFER_STATS_CPUUTIL_STONEWALL);
		cpuUtil.lastDone = resultTree.get<unsigned>(XFER_STATS_CPUUTIL);

		elapsedUSecVec.resize(0);
		elapsedUSecVec.reserve(progArgs->getNumThreads() );

		for(bpt::ptree::value_type& elapsedUSecItem :
			resultTree.get_child(XFER_STATS_ELAPSEDUSECLIST) )
			elapsedUSecVec.push_back(elapsedUSecItem.second.get_value<uint64_t>() );

		iopsLatHisto.setFromPropertyTree(resultTree, XFER_STATS_LAT_PREFIX_IOPS);
		entriesLatHisto.setFromPropertyTree(resultTree, XFER_STATS_LAT_PREFIX_ENTRIES);

		if( (workersSharedData->currentBenchPhase == BenchPhase_CREATEFILES) &&
			(progArgs->getRWMixPercent() ) )
		{
			atomicLiveRWMixReadOps.numBytesDone =
					resultTree.get<size_t>(XFER_STATS_NUMBYTESDONE_RWMIXREAD);
			atomicLiveRWMixReadOps.numIOPSDone =
					resultTree.get<size_t>(XFER_STATS_NUMIOPSDONE_RWMIXREAD);
		}

		phaseFinished = true; // before incNumWorkersDone() because Coordinator can reset after inc

		incNumWorkersDone();
	}
	catch(Web::system_error& e)
	{
		THROW_WORKEREXCEPTION_OR_LOG_ERR(allowExceptionThrow,
			std::string("HTTP client error in finish benchmark phase: ") + e.what() + ". "
			"Server: " + host);

		phaseFinished = true; // before incNumWorkersDone because Coordinator can reset after inc

		incNumWorkersDoneWithError();
	}

}


/**
 * @throw WorkerException on error, e.g. http client problem
 */
void RemoteWorker::preparePhase()
{
	bpt::ptree tree;
	progArgs->getAsPropertyTree(tree, workerRank);
	std::stringstream treeStream;
	bpt::write_json(treeStream, tree, true);

	if(progArgs->getInterruptServices() || progArgs->getQuitServices() )
		return; // nothing to prepare here

	try
	{
		std::string requestPath = HTTPCLIENTPATH_PREPAREPHASE "?"
			XFER_PREP_PROTCOLVERSION "=" HTTP_PROTOCOLVERSION;

		auto response = httpClient.request("POST", requestPath, treeStream);

		IF_UNLIKELY(response->status_code != Web::status_code(Web::StatusCode::success_ok) )
		{
			Logger(Log_DEBUG) << "HTTP status code: " + response->status_code << std::endl;

			throw WorkerException(frameHostErrorMsg(response->content.string() ) );
		}

		IF_UNLIKELY(!response->content.size() )
			throw WorkerException("Service host sent unexpected empty reply as preparation result. "
				"Server: " + host);

		bpt::ptree resultTree;
		bpt::read_json(response->content, resultTree);

		benchPathType = (BenchPathType)resultTree.get<size_t>(XFER_PREP_BENCHPATHTYPE);
		std::string errorHistory = resultTree.get<std::string>(XFER_PREP_ERRORHISTORY);

		IF_UNLIKELY(!errorHistory.empty() )
			throw WorkerException(frameHostErrorMsg(response->content.string() ) );
	}
	catch(Web::system_error& e)
	{
		throw WorkerException(
			std::string("Communication error in preparation phase: ") + e.what() + ". "
			"Server: " + host);
	}
}

/**
 * @throw WorkerException on error, e.g. http client problem
 */
void RemoteWorker::startBenchPhase()
{
	try
	{
		std::string requestPath = HTTPCLIENTPATH_STARTPHASE "?"
			XFER_START_BENCHPHASECODE "=" + std::to_string(workersSharedData->currentBenchPhase) +
			"&" XFER_START_BENCHID "=" + buuids::to_string(workersSharedData->currentBenchID);

		auto response = httpClient.request("GET", requestPath);

		IF_UNLIKELY(response->status_code != Web::status_code(Web::StatusCode::success_ok) )
		{
			Logger(Log_DEBUG) << "HTTP status code: " + response->status_code << std::endl;

			throw WorkerException(frameHostErrorMsg(response->content.string() ) );
		}

		IF_UNLIKELY(response->content.size() )
			throw WorkerException(frameHostErrorMsg(response->content.string() ) );
	}
	catch(Web::system_error& e)
	{
		throw WorkerException(
			std::string("HTTP client error in benchmark phase: ") + e.what() + ". "
			"Server: " + host);
	}
}

/**
 * Loop and update stats until benchmark completes on host.
 *
 * @checkInterruption true to throw an exception when interruption request comes in, false
 * 		to skip this check and thus not throw an exception.
 * @throw WorkerException on error, e.g. http client problem
 */
void RemoteWorker::waitForBenchPhaseCompletion(bool checkInterruption)
{
	bool firstRound = true;
	size_t sleepUS = progArgs->getSvcUpdateIntervalMS() * 1000;
	size_t firstRoundSleepUS = std::min( (size_t)500000, sleepUS/2); /* sleep half sec in first
		round to have first result when live stats get printed for the first time after 1 sec */

	while(numWorkersDone < progArgs->getNumThreads() )
	{
		usleep(firstRound ? firstRoundSleepUS : sleepUS);

		if(checkInterruption)
			checkInterruptionRequest();

		try
		{
			auto response = httpClient.request("GET", HTTPCLIENTPATH_STATUS);

			IF_UNLIKELY(response->status_code != Web::status_code(Web::StatusCode::success_ok) )
			{
				throw WorkerException(
					"Service host encountered an error. "
					"Server: " + host + "; "
					"Phase: Wait for benchmark completion; "
					"HTTP status code: " + response->status_code);
			}

			bpt::ptree statusTree;
			bpt::read_json(response->content, statusTree);

			buuids::string_generator uuidGen;
			buuids::uuid currentBenchID = uuidGen(statusTree.get<std::string>(XFER_STATS_BENCHID) );

			IF_UNLIKELY(workersSharedData->currentBenchID != currentBenchID)
				throw WorkerException("Service host got hijacked for a different benchmark. "
					"Server: " + host);

			numWorkersDone = statusTree.get<size_t>(XFER_STATS_NUMWORKERSDONE);
			numWorkersDoneWithError = statusTree.get<size_t>(XFER_STATS_NUMWORKERSDONEWITHERR);
			atomicLiveOps.numEntriesDone = statusTree.get<size_t>(XFER_STATS_NUMENTRIESDONE);
			atomicLiveOps.numBytesDone = statusTree.get<size_t>(XFER_STATS_NUMBYTESDONE);
			atomicLiveOps.numIOPSDone = statusTree.get<size_t>(XFER_STATS_NUMIOPSDONE);
			cpuUtil.live = statusTree.get<unsigned>(XFER_STATS_CPUUTIL);

			if( (workersSharedData->currentBenchPhase == BenchPhase_CREATEFILES) &&
				(progArgs->getRWMixPercent() ) )
			{
				atomicLiveRWMixReadOps.numBytesDone =
					statusTree.get<size_t>(XFER_STATS_NUMBYTESDONE_RWMIXREAD);
				atomicLiveRWMixReadOps.numIOPSDone =
					statusTree.get<size_t>(XFER_STATS_NUMIOPSDONE_RWMIXREAD);
			}

			IF_UNLIKELY(numWorkersDoneWithError)
			{
				std::string errorMsg = statusTree.get<std::string>(XFER_STATS_ERRORHISTORY);
				throw WorkerRemoteException(frameHostErrorMsg(errorMsg) );
			}

			if(numWorkersDone && !stoneWallTriggered)
			{
				for(Worker* worker : *workersSharedData->workerVec)
							worker->createStoneWallStats();
			}

			firstRound = false;
		}
		catch(Web::system_error& e)
		{
			throw WorkerException(
				std::string("HTTP client error in benchmark phase: ") + e.what() + ". "
				"Server: " + host);
		}
	} // end of while loop

}

/**
 * Interrupt the currently running benchmark phase on the service host.
 *
 * @allowExceptionThrow false to log errors instead of throwing an exception.
 * @throw if allowExceptionThrow is true, WorkerException on error, e.g. http client problem
 */
void RemoteWorker::interruptBenchPhase(bool allowExceptionThrow)
{
	try
	{
		std::string requestPath = HTTPCLIENTPATH_INTERRUPTPHASE;

		if(progArgs->getQuitServices() )
			requestPath += "?" XFER_INTERRUPT_QUIT;

		auto response = httpClient.request("GET", requestPath);

		IF_UNLIKELY(response->status_code != Web::status_code(Web::StatusCode::success_ok) )
		{
			std::string errorMsg = "Service host encountered an error. "
				"Server: " + host + "; "
				"Phase: Interruption; "
				"HTTP status code: " + response->status_code;

			THROW_WORKEREXCEPTION_OR_LOG_ERR(allowExceptionThrow, errorMsg);
		}
	}
	catch(Web::system_error& e)
	{
		std::string errorMsg =
			std::string("HTTP client error on benchmark interruption: ") + e.what() + ". "
			"Server: " + host;

		// in case of quit request, connection_refused will be returned by client - ignore that.

		bool logConnRefused = (progArgs->getLogLevel() > Log_NORMAL);

		if(!progArgs->getQuitServices() ||
			(e.code() != boost::asio::error::connection_refused) || logConnRefused)
			THROW_WORKEREXCEPTION_OR_LOG_ERR(allowExceptionThrow, errorMsg);
	}

}

/**
 * Add a frame around the given error message to show clear start/end, host and rank.
 *
 * @return msg string with extra frame characters to show clear start/end of msg.
 */
std::string RemoteWorker::frameHostErrorMsg(std::string msg)
{
	std::ostringstream stream;

	// prefix each new line

	std::string indentStr("  ");

	std::ostringstream endlStream;
	endlStream << std::endl;
	std::ostringstream endlIndentStream;
	endlIndentStream << std::endl << indentStr;;

	std::string endlStr(endlStream.str() );

	msg = indentStr + msg;

	bool msgHasTrailingLineBreak = false; // to ensure line break at end of msg
	std::string::size_type pos = 0;

	// find all line breaks to add additional indent for better readability
	while( (pos = msg.find(endlStr, pos) ) != std::string::npos)
	{
		if(pos == (msg.length() - endlStr.length() ) )
		{
			msgHasTrailingLineBreak = true;
			break; // don't replace trailing endl
		}

		msg.replace(pos, endlStr.length(), endlIndentStream.str());
		pos += indentStr.length();
	}

	stream << "=== [ HOST: " << host << " (Rank: " << workerRank << ") ] ===" << std::endl <<
		msg << (msgHasTrailingLineBreak ? "" : endlStr) <<
		"=== [ END ] ===";

	return stream.str();
}
