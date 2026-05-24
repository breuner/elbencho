// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <chrono>
#include <thread>
#include <string>
#include <sstream>

#include "RemoteWorker.h"
#include "Common.h"
#include "ProgArgs.h"
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
		prepareRemoteFiles();
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
                case BenchPhase_STATDIRS:
				case BenchPhase_CREATEFILES:
				case BenchPhase_READFILES:
				case BenchPhase_DELETEFILES:
				case BenchPhase_SYNC:
				case BenchPhase_DROPCACHES:
				case BenchPhase_STATFILES:
				case BenchPhase_PUTBUCKETACL:
				case BenchPhase_GETBUCKETACL:
				case BenchPhase_PUTOBJACL:
				case BenchPhase_GETOBJACL:
				case BenchPhase_LISTOBJECTS:
				case BenchPhase_LISTOBJPARALLEL:
				case BenchPhase_MULTIDELOBJ:
                case BenchPhase_GET_S3_OBJECT_MD:
                case BenchPhase_PUT_S3_OBJECT_MD:
                case BenchPhase_DEL_S3_OBJECT_MD:
                case BenchPhase_GET_S3_BUCKET_MD:
                case BenchPhase_PUT_S3_BUCKET_MD:
                case BenchPhase_DEL_S3_BUCKET_MD:
				case BenchPhase_S3MPUCOMPLETE:
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

		workerGotPhaseWork = resultTree.get<bool>(XFER_STATS_TRIGGERSTONEWALL);

		atomicLiveOps.numEntriesDone = resultTree.get<size_t>(XFER_STATS_NUMENTRIESDONE);
		atomicLiveOps.numBytesDone = resultTree.get<size_t>(XFER_STATS_NUMBYTESDONE);
		atomicLiveOps.numIOPSDone = resultTree.get<size_t>(XFER_STATS_NUMIOPSDONE);

		cpuUtil.stoneWall = resultTree.get<unsigned>(XFER_STATS_CPUUTIL_STONEWALL);
		cpuUtil.lastDone = resultTree.get<unsigned>(XFER_STATS_CPUUTIL);
		cpuUtil.live = 0; // this service is done, so no more cpu util

		elapsedUSecVec.resize(0);
		elapsedUSecVec.reserve(progArgs->getNumThreads() );

		if(resultTree.count(XFER_STATS_ELAPSEDUSECLIST) )
		{
			for(bpt::ptree::value_type& elapsedUSecItem :
				resultTree.get_child(XFER_STATS_ELAPSEDUSECLIST) )
				elapsedUSecVec.push_back(elapsedUSecItem.second.get_value<uint64_t>() );
		}

		iopsLatHisto.setFromPropertyTreeForService(resultTree, XFER_STATS_LAT_PREFIX_IOPS);
		entriesLatHisto.setFromPropertyTreeForService(resultTree, XFER_STATS_LAT_PREFIX_ENTRIES);

		liveLatency.setToZero(); // this service is done, so no more latency

		if( (workersSharedData->currentBenchPhase == BenchPhase_CREATEFILES) &&
			(progArgs->getRWMixReadPercent() || progArgs->getNumRWMixReadThreads() ||
				(progArgs->getBenchMode() == BenchMode_NETBENCH) ) )
		{
			atomicLiveOpsReadMix.numEntriesDone =
					resultTree.get<size_t>(XFER_STATS_NUMENTRIESDONE_RWMIXREAD);
			atomicLiveOpsReadMix.numBytesDone =
					resultTree.get<size_t>(XFER_STATS_NUMBYTESDONE_RWMIXREAD);
			atomicLiveOpsReadMix.numIOPSDone =
					resultTree.get<size_t>(XFER_STATS_NUMIOPSDONE_RWMIXREAD);

			iopsLatHistoReadMix.setFromPropertyTreeForService(
				resultTree, XFER_STATS_LAT_PREFIX_IOPS_RWMIXREAD);
			entriesLatHistoReadMix.setFromPropertyTreeForService(
				resultTree, XFER_STATS_LAT_PREFIX_ENTRIES_RWMIXREAD);

			// (note: liveLatency is nulled via .setToZero() above)
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
 void RemoteWorker::prepareRemoteFiles()
 {
    if(progArgs->getInterruptServices() || progArgs->getQuitServices() )
        return; // nothing to prepare here

    std::string treeFilePath = progArgs->getTreeFilePath();
    if(!treeFilePath.empty() )
        prepareRemoteFile(treeFilePath, SERVICE_UPLOAD_TREEFILE);

    if(progArgs->getUseS3MPUSharing() )
        prepareRemoteFile(S3_IMPLICIT_MPUSHAING_PATH, SERVICE_UPLOAD_MPUSHARINGFILE);
 }

/**
 * Upload files to service host (such as a custom tree file, if given) so that they are
 * available for the benchmark preparation phase.
 *
 * @throw WorkerException on error, e.g. http client problem
 */
void RemoteWorker::prepareRemoteFile(std::string localFilePath, std::string remoteFilename)
{
	// open tree file as stream
	std::ifstream fileStream(localFilePath);

	if(!fileStream)
		throw WorkerException("Unable to read custom tree file. Path: " + localFilePath);

	try
	{
		std::string requestPath = HTTPCLIENTPATH_PREPAREFILE "?"
			XFER_PREP_PROTCOLVERSION "=" HTTP_PROTOCOLVERSION "&"
			XFER_PREP_FILENAME "=" + remoteFilename + "&"
            XFER_PREP_AUTHORIZATION "=" + progArgs->getSvcPasswordHash();

		auto response = httpClient.request("POST", requestPath, fileStream);

		IF_UNLIKELY(response->status_code != Web::status_code(Web::StatusCode::success_ok) )
		{
			Logger(Log_DEBUG) << "HTTP status code: " + response->status_code << std::endl;

			throw WorkerException(frameHostErrorMsg(response->content.string() ) );
		}

		IF_UNLIKELY(response->content.size() )
			throw WorkerException(
				"Service sent unexpected non-empty reply as remote file preparation result. "
				"Service: " + host + "; " +
                "Remote filename: " + remoteFilename);
	}
	catch(Web::system_error& e)
	{
		throw WorkerException(
			std::string("Communication error in remote file preparation phase: ") + e.what() + ". "
			"Service: " + host + "; " +
            "Local path: " + localFilePath + "; "
            "Remote filename: " + remoteFilename);
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
			XFER_PREP_PROTCOLVERSION "=" HTTP_PROTOCOLVERSION "&"
			XFER_PREP_AUTHORIZATION "=" + progArgs->getSvcPasswordHash();

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
    const bool isNetBenchMode = (progArgs->getBenchMode() == BenchMode_NETBENCH);

    std::chrono::steady_clock::time_point lastRefreshT = workersSharedData->phaseStartT;

	while(numWorkersDone < progArgs->getNumThreads() )
	{
        lastRefreshT = calcNextRefreshTime(lastRefreshT);

        std::this_thread::sleep_until(lastRefreshT); // wait until it's time for next refresh

        if(checkInterruption)
            checkInterruptionRequest();

		try
		{
            std::chrono::steady_clock::time_point reqStartT = std::chrono::steady_clock::now();

			auto response = httpClient.request("GET", HTTPCLIENTPATH_STATUS);

			IF_UNLIKELY(response->status_code != Web::status_code(Web::StatusCode::success_ok) )
			{
				throw WorkerException(
					"Service encountered an error. "
					"Service: " + host + "; "
					"Phase: Wait for benchmark completion; "
					"HTTP status code: " + response->status_code);
			}

            // calc request latency
            std::chrono::steady_clock::time_point reqEndT = std::chrono::steady_clock::now();
            pingMicroSecs = std::chrono::duration_cast<std::chrono::microseconds>
                (reqEndT - reqStartT).count();

            // process response...

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

            uint64_t numAvgIOLatValues = statusTree.get<uint64_t>(XFER_STATS_LAT_NUM_IOPS);
            if(numAvgIOLatValues)
            { /* this is for rate limiter, where some updates might not have new values, but we
                don't want to show 0 lat in live stats, so we keep previous value */
                    liveLatency.avgIOLatMicroSecsSum =
                    statusTree.get<uint64_t>(XFER_STATS_LAT_SUM_IOPS);
                liveLatency.numAvgIOLatValues = numAvgIOLatValues;
            }

            uint64_t numAvgEntriesLatValues = statusTree.get<uint64_t>(XFER_STATS_LAT_NUM_ENTRIES);
            if(numAvgEntriesLatValues)
            { /* this is for rate limiter, where some updates might not have new values, but we
                don't want to show 0 lat in live stats, so we keep previous value */
                    liveLatency.avgEntriesLatMicroSecsSum =
                    statusTree.get<uint64_t>(XFER_STATS_LAT_SUM_ENTRIES);
                liveLatency.numAvgEntriesLatValues = numAvgEntriesLatValues;
            }

			if( (workersSharedData->currentBenchPhase == BenchPhase_CREATEFILES) &&
				(progArgs->getRWMixReadPercent() || progArgs->getNumRWMixReadThreads() ||
					isNetBenchMode) )
			{
				atomicLiveOpsReadMix.numEntriesDone =
					statusTree.get<size_t>(XFER_STATS_NUMENTRIESDONE_RWMIXREAD);
				atomicLiveOpsReadMix.numBytesDone =
					statusTree.get<size_t>(XFER_STATS_NUMBYTESDONE_RWMIXREAD);
				atomicLiveOpsReadMix.numIOPSDone =
					statusTree.get<size_t>(XFER_STATS_NUMIOPSDONE_RWMIXREAD);

                uint64_t numAvgIOLatReadMixValues =
                    statusTree.get<uint64_t>(XFER_STATS_LAT_NUM_IOPS_RWMIXREAD);
                if(numAvgIOLatReadMixValues)
                { /* this is for rate limiter, where some updates might not have new values, but we
                    don't want to show 0 lat in live stats, so we keep previous value */
                    liveLatency.avgIOLatReadMixMicroSecsSum =
                        statusTree.get<uint64_t>(XFER_STATS_LAT_SUM_IOPS_RWMIXREAD);
                    liveLatency.numAvgIOLatReadMixValues = numAvgIOLatReadMixValues;
                }

                uint64_t numAvgEntriesLatReadMixValues =
                    statusTree.get<uint64_t>(XFER_STATS_LAT_NUM_ENTRIES_RWMIXREAD);
                if(numAvgEntriesLatReadMixValues)
                { /* this is for rate limiter, where some updates might not have new values, but we
                    don't want to show 0 lat in live stats, so we keep previous value */
                    liveLatency.avgEntriesLatReadMixMicrosSecsSum =
                        statusTree.get<uint64_t>(XFER_STATS_LAT_SUM_ENTRIES_RWMIXREAD);
                    liveLatency.numAvgEntriesLatReadMixValues = numAvgEntriesLatReadMixValues;
                }
            }

			IF_UNLIKELY(numWorkersDoneWithError)
			{
				std::string errorMsg = statusTree.get<std::string>(XFER_STATS_ERRORHISTORY);
				throw WorkerRemoteException(frameHostErrorMsg(errorMsg) );
			}

			bool svcHasTriggeredStonewall = statusTree.get<bool>(XFER_STATS_TRIGGERSTONEWALL);

			if(numWorkersDone && svcHasTriggeredStonewall && workerGotPhaseWork &&
				!stoneWallTriggered)
			{ // stonewall triggered

                /* wait 5ms to give the other RemoteWorkers time to retrieve their stats for this
                    stonewall round. */
                if(progArgs->getHostsVec().size() )
                    std::this_thread::sleep_for(std::chrono::milliseconds(5) );

				for(Worker* worker : *workersSharedData->workerVec)
					worker->createStoneWallStats();
			}

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

/**
 * Calculate the next service stats refresh time based on the current round. Lower round numbers
 * will use a more aggressive/shorter refresh interval, which then gets more relaxed in later rounds
 * up to max service update interval from ProgArgs.
 *
 * @param lastRefreshT the result of the last call to this function or phase start time for the
 *      first call.
 * @return the time for the next refresh.
 */
std::chrono::steady_clock::time_point RemoteWorker::calcNextRefreshTime(
    std::chrono::steady_clock::time_point& lastRefreshT)
{
    std::chrono::milliseconds lastRefreshPhaseElapsedT =
        std::chrono::duration_cast<std::chrono::milliseconds>
        (lastRefreshT - workersSharedData->phaseStartT);

    // try to achieve less than 10% error for short runs, thus div by 11 to take ping into account
    unsigned svcUpdateIntervalMS = lastRefreshPhaseElapsedT.count() / 11;

    unsigned minRefreshIntervalMS = 25; /* 25ms as min just because values below 25ms seem too
        aggressive, but still possible to manually set a lower value through ProgArgs due to max
        check afterwards. */

    if(svcUpdateIntervalMS < minRefreshIntervalMS)
        svcUpdateIntervalMS = minRefreshIntervalMS;

    unsigned maxRefreshIntervalMS = std::min(progArgs->getSvcUpdateIntervalMS(),
        progArgs->getLiveStatsSleepMS() / 2);

    if(svcUpdateIntervalMS > maxRefreshIntervalMS)
        svcUpdateIntervalMS = maxRefreshIntervalMS;

    return lastRefreshT + std::chrono::milliseconds(svcUpdateIntervalMS);
}