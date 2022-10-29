#include <chrono>
#include <thread>
#include <string>
#include <sstream>
#include "RemoteWorker.h"
#include "toolkits/TranslatorTk.h"
#include "WorkerException.h"
#include "WorkersSharedData.h"

namespace Web = SimpleWeb;


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
		applyNumaAndCoreBinding();
		prepareRemoteFile();
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
					interruptBenchPhase(false,
						progArgs->getInterruptServices() || progArgs->getQuitServices() );
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
				case BenchPhase_LISTOBJECTS:
				case BenchPhase_LISTOBJPARALLEL:
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
				"Service instance encountered an error. "
				"Service: " + host + "; "
				"Phase: Finalization; "
				"Message: " + response->content.string() );
		}

		bpt::ptree resultTree;
		bpt::read_json(response->content, resultTree);

		buuids::string_generator uuidGen;
		buuids::uuid currentBenchID = uuidGen(resultTree.get<std::string>(XFER_STATS_BENCHID) );

		IF_UNLIKELY(workersSharedData->currentBenchID != currentBenchID)
			THROW_WORKEREXCEPTION_OR_LOG_ERR(allowExceptionThrow,
				"Service instance got hijacked for a different benchmark. "
				"Service: " + host);

		numWorkersDone = resultTree.get<size_t>(XFER_STATS_NUMWORKERSDONE);
		numWorkersDoneWithError = resultTree.get<size_t>(XFER_STATS_NUMWORKERSDONEWITHERR);

		IF_UNLIKELY(numWorkersDoneWithError)
		{
			std::string errorMsg = resultTree.get<std::string>(XFER_STATS_ERRORHISTORY);
			THROW_WORKEREXCEPTION_OR_LOG_ERR(allowExceptionThrow, frameHostErrorMsg(errorMsg) );
		}

		IF_UNLIKELY(numWorkersDone < progArgs->getNumThreads() )
			THROW_WORKEREXCEPTION_OR_LOG_ERR(allowExceptionThrow,
				"Phase finish handler was called before all workers of service instance completed. "
				"Service: " + host + "; "
				"numWorkersDone: " + host + "; "
				"numWorkersDoneWithError: " + host + "; "
				"numThreads: " + host + "; ");

		atomicLiveOps.numEntriesDone = resultTree.get<size_t>(XFER_STATS_NUMENTRIESDONE);
		atomicLiveOps.numBytesDone = resultTree.get<size_t>(XFER_STATS_NUMBYTESDONE);
		atomicLiveOps.numIOPSDone = resultTree.get<size_t>(XFER_STATS_NUMIOPSDONE);

		cpuUtil.stoneWall = resultTree.get<unsigned>(XFER_STATS_CPUUTIL_STONEWALL);
		cpuUtil.lastDone = resultTree.get<unsigned>(XFER_STATS_CPUUTIL);
		cpuUtil.live = 0;

		elapsedUSecVec.resize(0);
		elapsedUSecVec.reserve(progArgs->getNumThreads() );

		for(bpt::ptree::value_type& elapsedUSecItem :
			resultTree.get_child(XFER_STATS_ELAPSEDUSECLIST) )
			elapsedUSecVec.push_back(elapsedUSecItem.second.get_value<uint64_t>() );

		iopsLatHisto.setFromPropertyTree(resultTree, XFER_STATS_LAT_PREFIX_IOPS);
		entriesLatHisto.setFromPropertyTree(resultTree, XFER_STATS_LAT_PREFIX_ENTRIES);

		if( (workersSharedData->currentBenchPhase == BenchPhase_CREATEFILES) &&
			(progArgs->getRWMixPercent() || progArgs->getNumRWMixReadThreads() ) )
		{
			atomicLiveOpsReadMix.numEntriesDone =
					resultTree.get<size_t>(XFER_STATS_NUMENTRIESDONE_RWMIXREAD);
			atomicLiveOpsReadMix.numBytesDone =
					resultTree.get<size_t>(XFER_STATS_NUMBYTESDONE_RWMIXREAD);
			atomicLiveOpsReadMix.numIOPSDone =
					resultTree.get<size_t>(XFER_STATS_NUMIOPSDONE_RWMIXREAD);

			iopsLatHistoReadMix.setFromPropertyTree(
				resultTree, XFER_STATS_LAT_PREFIX_IOPS_RWMIXREAD);
			entriesLatHistoReadMix.setFromPropertyTree(
				resultTree, XFER_STATS_LAT_PREFIX_ENTRIES_RWMIXREAD);
		}

		phaseFinished = true; // before incNumWorkersDone() because Coordinator can reset after inc

		incNumWorkersDone();
	}
	catch(Web::system_error& e)
	{
		THROW_WORKEREXCEPTION_OR_LOG_ERR(allowExceptionThrow,
			std::string("HTTP client error in finish benchmark phase: ") + e.what() + ". "
			"Service: " + host);

		phaseFinished = true; // before incNumWorkersDone because Coordinator can reset after inc

		incNumWorkersDoneWithError();
	}

}

/**
 * Upload files to service host (such as a custom tree file, if given) so that they are
 * available for the benchmark preparation phase.
 *
 * @throw WorkerException on error, e.g. http client problem
 */
void RemoteWorker::prepareRemoteFile()
{
	if(progArgs->getInterruptServices() || progArgs->getQuitServices() )
		return; // nothing to prepare here

	std::string treeFilePath = progArgs->getTreeFilePath();
	if(treeFilePath.empty() )
		return; // nothing to do here

	// open tree file as stream
	std::ifstream treeFileStream(treeFilePath);

	if(!treeFileStream)
		throw WorkerException("Unable to read custom tree file. Path: " + treeFilePath);

	try
	{
		std::string requestPath = HTTPCLIENTPATH_PREPAREFILE "?"
			XFER_PREP_PROTCOLVERSION "=" HTTP_PROTOCOLVERSION
			"&" XFER_PREP_FILENAME "=" SERVICE_UPLOAD_TREEFILE;

		auto response = httpClient.request("POST", requestPath, treeFileStream);

		IF_UNLIKELY(response->status_code != Web::status_code(Web::StatusCode::success_ok) )
		{
			Logger(Log_DEBUG) << "HTTP status code: " + response->status_code << std::endl;

			throw WorkerException(frameHostErrorMsg(response->content.string() ) );
		}

		IF_UNLIKELY(response->content.size() )
			throw WorkerException(
				"Service sent unexpected non-empty reply as remote file preparation result. "
				"Service: " + host);
	}
	catch(Web::system_error& e)
	{
		throw WorkerException(
			std::string("Communication error in remote file preparation phase: ") + e.what() + ". "
			"Service: " + host);
	}
}


/**
 * Send ProgArgs to service host, so service can prepare everything to start the actual benchmark
 * phase.
 *
 * @throw WorkerException on error, e.g. http client problem
 */
void RemoteWorker::preparePhase()
{
	bpt::ptree tree;
	progArgs->getAsPropertyTreeForService(tree, workerRank);
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
			throw WorkerException("Service sent unexpected empty reply as preparation result. "
				"Service: " + host);

		// read bench path info and error history from service...

		bpt::ptree resultTree;
		bpt::read_json(response->content, resultTree);

		benchPathInfo.benchPathStr = resultTree.get<std::string>(ARG_BENCHPATHS_LONG);
		benchPathInfo.benchPathType =
			(BenchPathType)resultTree.get<size_t>(XFER_PREP_BENCHPATHTYPE);
		benchPathInfo.numBenchPaths = resultTree.get<size_t>(XFER_PREP_NUMBENCHPATHS);
		benchPathInfo.fileSize = resultTree.get<uint64_t>(ARG_FILESIZE_LONG);
		benchPathInfo.blockSize = resultTree.get<uint64_t>(ARG_BLOCK_LONG);
		benchPathInfo.randomAmount = resultTree.get<uint64_t>(ARG_RANDOMAMOUNT_LONG);

		std::string errorHistory = resultTree.get<std::string>(XFER_PREP_ERRORHISTORY);

		IF_UNLIKELY(!errorHistory.empty() )
			throw WorkerException(frameHostErrorMsg(response->content.string() ) );
	}
	catch(Web::system_error& e)
	{
		throw WorkerException(
			std::string("Communication error in preparation phase: ") + e.what() + ". "
			"Service: " + host);
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
			"Service: " + host);
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

	size_t svcUpdateIntervalMS = progArgs->getSvcUpdateIntervalMS();
	if(svcUpdateIntervalMS > (progArgs->getLiveStatsSleepMS() / 2) )
		svcUpdateIntervalMS = progArgs->getLiveStatsSleepMS() / 2;

	std::chrono::milliseconds sleepMS(svcUpdateIntervalMS );
	std::chrono::milliseconds firstRoundSleepMS( std::min(500, (int)(sleepMS.count() / 2) ) ); /*
		shorter first round sleep to have results when live stats get printed for the first time */
	std::chrono::steady_clock::time_point lastSleepT = workersSharedData->phaseStartT;

	while(numWorkersDone < progArgs->getNumThreads() )
	{
		lastSleepT += firstRound ? firstRoundSleepMS : sleepMS;
		std::this_thread::sleep_until(lastSleepT);

		if(checkInterruption)
			checkInterruptionRequest();

		try
		{
			auto response = httpClient.request("GET", HTTPCLIENTPATH_STATUS);

			IF_UNLIKELY(response->status_code != Web::status_code(Web::StatusCode::success_ok) )
			{
				throw WorkerException(
					"Service encountered an error. "
					"Service: " + host + "; "
					"Phase: Wait for benchmark completion; "
					"HTTP status code: " + response->status_code);
			}

			bpt::ptree statusTree;
			bpt::read_json(response->content, statusTree);

			buuids::string_generator uuidGen;
			buuids::uuid currentBenchID = uuidGen(statusTree.get<std::string>(XFER_STATS_BENCHID) );

			IF_UNLIKELY(workersSharedData->currentBenchID != currentBenchID)
				throw WorkerException("Service got hijacked for a different benchmark. "
					"Service: " + host);

			numWorkersDone = statusTree.get<size_t>(XFER_STATS_NUMWORKERSDONE);
			numWorkersDoneWithError = statusTree.get<size_t>(XFER_STATS_NUMWORKERSDONEWITHERR);
			atomicLiveOps.numEntriesDone = statusTree.get<size_t>(XFER_STATS_NUMENTRIESDONE);
			atomicLiveOps.numBytesDone = statusTree.get<size_t>(XFER_STATS_NUMBYTESDONE);
			atomicLiveOps.numIOPSDone = statusTree.get<size_t>(XFER_STATS_NUMIOPSDONE);
			cpuUtil.live = statusTree.get<unsigned>(XFER_STATS_CPUUTIL);

			if( (workersSharedData->currentBenchPhase == BenchPhase_CREATEFILES) &&
				(progArgs->getRWMixPercent() || progArgs->getNumRWMixReadThreads() ) )
			{
				atomicLiveOpsReadMix.numEntriesDone =
					statusTree.get<size_t>(XFER_STATS_NUMENTRIESDONE_RWMIXREAD);
				atomicLiveOpsReadMix.numBytesDone =
					statusTree.get<size_t>(XFER_STATS_NUMBYTESDONE_RWMIXREAD);
				atomicLiveOpsReadMix.numIOPSDone =
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
				"Service: " + host);
		}
	} // end of while loop

}

/**
 * Interrupt the currently running benchmark phase on the service host and quit the service if
 * progArgs->getQuitServices() is set.
 *
 * @allowExceptionThrow false to log errors instead of throwing an exception.
 * @logSuccessMsg log output for whether service confirmed or not (the latter typically meaning
 * that the service was not running)
 * @throw if allowExceptionThrow is true, WorkerException on error, e.g. http client problem
 */
void RemoteWorker::interruptBenchPhase(bool allowExceptionThrow, bool logSuccessMsg)
{
	try
	{
		std::string requestPath = HTTPCLIENTPATH_INTERRUPTPHASE;

		if(progArgs->getQuitServices() )
			requestPath += "?" XFER_INTERRUPT_QUIT "=1"; // "=1" because some parsers expect "=" val

		auto response = httpClient.request("GET", requestPath);

		IF_UNLIKELY(response->status_code != Web::status_code(Web::StatusCode::success_ok) )
		{
			std::string errorMsg = "Service instance encountered an error. "
				"Service: " + host + "; "
				"Phase: Interruption; "
				"HTTP status code: " + response->status_code;

			THROW_WORKEREXCEPTION_OR_LOG_ERR(allowExceptionThrow, errorMsg);
		}

		// log human-friendly success confirmation message
		if(logSuccessMsg)
			LOGGER(Log_NORMAL, host << ": " << "OK" << std::endl);

	}
	catch(Web::system_error& e)
	{
		// log human-friendly success confirmation message
		if(logSuccessMsg && (e.code() == boost::asio::error::connection_refused) )
			LOGGER(Log_NORMAL, host << ": " <<
				"Service unreachable" << std::endl);

		// log error details...

		std::string errorMsg =
			std::string("HTTP client error on benchmark interruption: ") + e.what() + ". "
			"Service: " + host;

		// in case of quit request, connection_refused will be returned by client - ignore that.

		bool logConnRefused = (progArgs->getLogLevel() > Log_NORMAL);

		if(!logSuccessMsg ||
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
