// SPDX-FileCopyrightText: 2020-2025 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "Common.h"
#include "toolkits/SignalTk.h"
#include "toolkits/TranslatorTk.h"
#include "LocalWorker.h"
#include "RemoteWorker.h"
#include "WorkerException.h"
#include "WorkerManager.h"


WorkerManager::~WorkerManager()
{
	deleteThreads(); // (also stops the ramp timer)
}

/**
 * Interrupt and notify all workers (typically after a fatal error).
 */
void WorkerManager::interruptAndNotifyWorkersUnlocked()
{
	progArgs.setS3InterruptionRequested();

	for(Worker* worker : workerVec)
		worker->interruptExecution();

	workersSharedData.condition.notify_all();
}

/**
 * Interrupt and notify all workers (typically after a fatal error).
 */
void WorkerManager::interruptAndNotifyWorkers()
{
	std::unique_lock<std::mutex> lock(workersSharedData.mutex); // L O C K (scoped)

	interruptAndNotifyWorkersUnlocked();
}

/**
 * Quick check if workers are done or still working on current phase.
 * Also checks for gotUserInterrupSignal to be true.
 *
 * @outNumWorkersDone number of done workers (including those that completed with error).
 * @return true if all workers done, false otherwise.
 * @thow WorkerException if any of the workers encountered an error in this phase; or
 * 		ProgInterruptedException if gotUserInterrupSignal is set.
 */
bool WorkerManager::checkWorkersDoneUnlocked(size_t* outNumWorkersDone)
{
	*outNumWorkersDone =
		workersSharedData.numWorkersDone + workersSharedData.numWorkersDoneWithError;

	IF_UNLIKELY(workersSharedData.numWorkersDoneWithError)
	{ // worker encountered error, so notify others and cancel
		interruptAndNotifyWorkersUnlocked();
		throw WorkerException("Worker encountered error");
	}

	IF_UNLIKELY(WorkersSharedData::gotUserInterruptSignalT)
		interruptAndNotifyWorkersUnlocked();

	if(*outNumWorkersDone == workerVec.size() )
		return true; // all workers done

	return false;
}


/**
 * Quick check if workers are done or still working on current phase.
 *
 * @outNumWorkersDone number of done workers (including those that completed with error).
 * @return true if all workers done, false otherwise.
 * @thow WorkerException if any of the workers encountered an error in this phase.
 */
bool WorkerManager::checkWorkersDone(size_t* outNumWorkersDone)
{
	std::unique_lock<std::mutex> lock(workersSharedData.mutex); // L O C K (scoped)

	return checkWorkersDoneUnlocked(outNumWorkersDone);
}

/**
 * Check if the user-defined phase time limit has been exceeded and interrupt workers if it is
 * exceeded.
 */
void WorkerManager::checkPhaseTimeLimitUnlocked()
{
	if(!progArgs.getTimeLimitSecs() )
		return;

	std::chrono::seconds elapsedDurationSecs =
					std::chrono::duration_cast<std::chrono::seconds>
					(std::chrono::steady_clock::now() - workersSharedData.phaseStartT);
	size_t elapsedSecs = elapsedDurationSecs.count();

	IF_UNLIKELY(elapsedSecs >= progArgs.getTimeLimitSecs() )
	{
		workersSharedData.isPhaseTimeExpired = true;
		interruptAndNotifyWorkersUnlocked();
	}
}

/**
 * Check if the user-defined phase time limit has been exceeded and interrupt workers if it is
 * exceeded.
 */
void WorkerManager::checkPhaseTimeLimit()
{
	/* note: this does not call checkPhaseTimeLimitUnlocked because there is no reason to have the
			workersSharedData.mutex locked every time this is called, except in the unlikely event
			of actually exceeded time limit. */

	if(!progArgs.getTimeLimitSecs() )
		return;

	std::chrono::seconds elapsedDurationSecs =
					std::chrono::duration_cast<std::chrono::seconds>
					(std::chrono::steady_clock::now() - workersSharedData.phaseStartT);
	size_t elapsedSecs = elapsedDurationSecs.count();

	IF_UNLIKELY(elapsedSecs >= progArgs.getTimeLimitSecs() )
	{
		WorkersSharedData::isPhaseTimeExpired = true;
		interruptAndNotifyWorkers();
	}
}

/**
 * Prepare a new set of threads according to the number given in progArgs.
 * (Also initializes basics of workersSharedData and set workersSharedData.phaseStartT to now.)
 *
 * In local or master mode, this will be called only once at the beginning.
 * In service mode, this will be called for each new bench preparation message. Use cleanupThreads()
 * to delete the threads of the previous round before changing progArgs and calling this.
 *
 * This will return after the workers have finished their preparation phase.
 *
 * @throw WorkerException in case of error during worker preparation phase.
 */
void WorkerManager::prepareThreads()
{
	/* a leftover timer of a previous phase refers to workerVec, which we are about to grow, so it
		may not still be running here (same reason as in deleteThreads() ) */
	stopRampTimer();

	threadGroup.reserve(progArgs.getHostsVec().empty() ?
			progArgs.getNumThreads() : progArgs.getHostsVec().size() );
	workerVec.reserve(progArgs.getHostsVec().empty() ?
		progArgs.getNumThreads() : progArgs.getHostsVec().size() );

	workersSharedData.threadGroup = &threadGroup;
	workersSharedData.progArgs = &progArgs;
	workersSharedData.workerVec = &workerVec;
    workersSharedData.phaseStartT = std::chrono::steady_clock::now();
	workersSharedData.phaseStartLocalT = std::chrono::system_clock::now();
	workersSharedData.currentBenchID = buuids::nil_uuid(); // workers expect this as prep phase ID

	// the preparation phase has no results and thus no "--ramp" warm-up period
	workersSharedData.isRampActive = false;
	workersSharedData.isRampDone = false;

	workersSharedData.resetNumWorkersDoneUnlocked();

	if(progArgs.getHostsVec().empty() )
	{ // we're running in local or service mode, so create LocalWorkers

		for(size_t i=0; i < progArgs.getNumThreads(); i++)
		{
			Worker* newWorker = new LocalWorker(&workersSharedData, progArgs.getRankOffset() + i);
			workerVec.push_back(newWorker);
		}
	}
	else
	{ // we're running in maser mode, so create one RemoteWorker for each host
		const StringVec& hostsVec = progArgs.getHostsVec();

		for(size_t i=0; i < hostsVec.size(); i++)
		{
			Worker* newWorker = new RemoteWorker(
				&workersSharedData, progArgs.getRankOffset() + i, hostsVec[i] );
			workerVec.push_back(newWorker);
		}
	}

	// run worker threads
	for(size_t i=0; i < workerVec.size(); i++)
	{
		/* Linux can send process signals to any thread, so ensure that workers have SIGINT/SIGTERM
			blocked and only the main thread receives it. */
		SignalTk::blockInterruptSignals();
		std::thread* thread = new std::thread(Worker::threadStart, workerVec[i] );
		SignalTk::unblockInterruptSignals(); // unblock for current thread
		threadGroup.push_back(thread);
	}

	// temporarily disable phase time limit, as it makes no sense for worker preparation phase
	size_t oldTimeLimitTecs = progArgs.getTimeLimitSecs();
	progArgs.setTimeLimitSecs(0);

	// wait for workers to finish their preparation phase before returning
	waitForWorkersDone();

	// restore user-defined phase time limit
	progArgs.setTimeLimitSecs(oldTimeLimitTecs);
}

/**
 * Delete all worker threads objects.
 *
 * Caller must ensure that all threads are stopped (e.g. via joinAllThreads() ).
 *
 * This is separate from prepareWorkers, because inside prepareWorkers would create problems when
 * progArgs get changed in service mode.
 */
void WorkerManager::deleteThreads()
{
	stopRampTimer(); // (the timer refers to workerVec, so it may not outlive the workers)

	// cleanup allocated thread objects
	for(std::thread* thread : threadGroup)
		delete(thread);

	threadGroup.resize(0);

	// cleanup allocated worker objects
	for(Worker* worker : workerVec)
		delete(worker);

	workerVec.resize(0);
}


/**
 * Join all threads in threadGroup and wait for them to terminate.
 */
void WorkerManager::joinAllThreads()
{
	/* note: joinable() is only true once (and always false after join() ) and we join the same
		workers multiple times, e.g. in HTTP servers's "/preparephase", so joinable() false is not
		an indication of a problem. */

	for(std::thread* thread : threadGroup)
		if(thread->joinable() )
			thread->join();
}

/**
 * Wait for all workers to finish this phase.
 *
 * @throw WorkerException if any of the workers encountered an error in this phase;
 * 		ProgInterruptedException if user interrupt signal was detected; ProgException if the
 * 		workload of this phase completed before the end of the "--ramp" warm-up period.
 */
void WorkerManager::waitForWorkersDone()
{
	const unsigned sleepSecs = 2; // how often to wake up to check for interrupt signal

	std::unique_lock<std::mutex> lock(workersSharedData.mutex); // L O C K (scoped)

	bool workersDone;

	do
	{
		workersDone = workersSharedData.condition.wait_for(
			lock, std::chrono::seconds(sleepSecs),
			[&, this]
			{
				size_t numWorkersDone;
				return checkWorkersDoneUnlocked(&numWorkersDone);
			} );

		checkPhaseTimeLimitUnlocked();

	} while(!workersDone);

	/* the ramp timer won't fire anymore now that all workers are done, so there is no measured
		window left to report. (Interrupt and time limit end a phase early for a reason of their
		own, which the caller reports, so they are not blamed on the warm-up period.) */
	IF_UNLIKELY(workersSharedData.isWarmingUp() &&
		!WorkersSharedData::gotUserInterruptSignalT && !WorkersSharedData::isPhaseTimeExpired)
		throw ProgException("Workload completed before the end of the \"--" ARG_RAMPSECS_LONG
			"\" warm-up period, so there is nothing left to measure.");
}

/**
 * Run the late cleanup phase of worker data structures.
 *
 * This is for (typically shared) data structures that can only be cleaned when all workers have
 * stopped, but cannot be called in worker destructor because in service mode, the worker obj is
 * only deleted when the next phase starts, not when a phase ends (because we need the worker obj
 * to retrieve the statistics).
 */
void WorkerManager::cleanupWorkersAfterPhaseDone()
{
	// cleanup allocated worker objects
	for(Worker* worker : workerVec)
		worker->cleanupAfterPhaseDone();
}


/**
 * Reset num done and notify workers to start next phase. Also restarts the "--ramp" warm-up timer.
 *
 * @newBenchPhase type of new bench phase to run.
 * @benchID new bench UUID, may be NULL if it should be randomly generated.
 */
void WorkerManager::startNextPhase(BenchPhase newBenchPhase, std::string* benchID)
{
	Logger(Log_VERBOSE) << "Starting benchmark phase: " <<
		TranslatorTk::benchPhaseToPhaseName(newBenchPhase, &progArgs) << std::endl;

	// (before the stats reset below, as a leftover timer would request another ramp reset)
	stopRampTimer();

	std::unique_lock<std::mutex> lock(workersSharedData.mutex); // L O C K (scoped)

	workersSharedData.resetNumWorkersDoneUnlocked();

	// reset worker stats
	for(size_t i=0; i < workerVec.size(); i++)
		workerVec[i]->resetStats();


	if(benchID)
	{
		buuids::string_generator uuidGen;
		workersSharedData.currentBenchID = uuidGen(*benchID);
	}
	else
	{
		buuids::random_generator uuidGen;
		workersSharedData.currentBenchID = uuidGen();
	}

	workersSharedData.currentBenchPhase = newBenchPhase;

	workersSharedData.isRampActive = progArgs.getRampSecs() && hasRampPhase(newBenchPhase);
	workersSharedData.isRampDone = false;
	rampTimerStopRequested = false;
	workersSharedData.cpuUtilFirstDone.update();
	workersSharedData.cpuUtilLastDone.update();
	workersSharedData.phaseStartT = std::chrono::steady_clock::now();
    workersSharedData.phaseStartLocalT = std::chrono::system_clock::now();

	workersSharedData.condition.notify_all();

	lock.unlock(); // U N L O C K

	startRampTimer();
}

/**
 * Whether a "--ramp" warm-up period makes sense for the given phase. Only the data phases have
 * one, because the prep and cleanup phases of a typical run (e.g. "-d -w -r -D") are usually done
 * in no time, so a warm-up period would fail the run right in its mkdirs phase.
 *
 * (A "--rwmixpct"/"--rwmixthreads" phase is a CREATEFILES or READFILES phase as well.)
 */
bool WorkerManager::hasRampPhase(BenchPhase benchPhase)
{
	switch(benchPhase)
	{
		case BenchPhase_CREATEFILES:
		case BenchPhase_READFILES:
			return true;

		default:
			return false;
	}
}

/**
 * Start the async timer for the "--ramp" warm-up period of a new phase. This is a no-op if the
 * current phase has no warm-up period. (In distributed mode, each instance times its own phase.)
 *
 * Caller must not hold workersSharedData.mutex.
 */
void WorkerManager::startRampTimer()
{
	if(!workersSharedData.isRampActive)
		return; // no warm-up period for this phase

	const std::chrono::steady_clock::time_point rampDeadlineT =
		workersSharedData.phaseStartT + std::chrono::seconds(progArgs.getRampSecs() );

	rampTimerThread = std::thread(&WorkerManager::rampTimerLoop, this,
		workersSharedData.currentBenchID, rampDeadlineT);
}

/**
 * Wait for the end of the "--ramp" warm-up period of the current phase and then ask all workers
 * to drop the statistics that they gathered so far. Returns early (without triggering) if the
 * phase ends or gets interrupted before that, so this thread lives at most progArgs::rampSecs.
 *
 * @benchID the phase that this timer was started for; the trigger is skipped if a different phase
 * 		is running by then.
 * @rampDeadlineT end of the warm-up period.
 */
void WorkerManager::rampTimerLoop(buuids::uuid benchID,
	std::chrono::steady_clock::time_point rampDeadlineT)
{
	std::unique_lock<std::mutex> lock(workersSharedData.mutex); // L O C K (scoped)

	const bool phaseOver = workersSharedData.condition.wait_until(lock, rampDeadlineT,
		[&, this]
		{
			return rampTimerStopRequested ||
				WorkersSharedData::gotUserInterruptSignalT ||
				( (workersSharedData.numWorkersDone +
					workersSharedData.numWorkersDoneWithError) == workerVec.size() );
		} );

	/* (if all workers are already through, then isRampDone stays false, which is what makes
		waitForWorkersDone() fail the run) */
	if(phaseOver || (workersSharedData.currentBenchID != benchID) )
		return; // nothing left to warm up for

	size_t numWorkersAlreadyDone = workersSharedData.numWorkersDone +
		workersSharedData.numWorkersDoneWithError;

	for(Worker* worker : workerVec)
		worker->requestRampStatsReset();

	/* restart the cpu util intervals, so that the reported cpu util also refers to the measured
		window only. (cpuUtilFirstDone's 2nd sample is taken by the stonewall trigger, so it may
		only be restarted while no worker is through yet.) */
	if(!numWorkersAlreadyDone)
		workersSharedData.cpuUtilFirstDone.update();

	workersSharedData.cpuUtilLastDone.update();

	workersSharedData.rampDoneT = std::chrono::steady_clock::now();
	workersSharedData.isRampDone = true; // (after rampDoneT, as readers rely on this ordering)

	lock.unlock(); // U N L O C K

	LOGGER(Log_VERBOSE, "Warm-up period is over, resetting statistics. "
		"Warm-up time: " << progArgs.getRampSecs() << "s" << std::endl);

	if(numWorkersAlreadyDone)
		LOGGER(Log_NORMAL, "NOTE: " << numWorkersAlreadyDone << " of " << workerVec.size() <<
			" workers completed this phase before the end of the \"--" ARG_RAMPSECS_LONG
			"\" warm-up period, so the stonewall (FIRST DONE) results refer to the warm-up "
			"period instead of the measured window." << std::endl);
}

/**
 * Stop and join the ramp timer of the current phase. This is a no-op if no timer is running.
 *
 * Caller must not hold workersSharedData.mutex.
 */
void WorkerManager::stopRampTimer()
{
	if(!rampTimerThread.joinable() )
		return;

	{
		std::unique_lock<std::mutex> lock(workersSharedData.mutex); // L O C K (scoped)

		rampTimerStopRequested = true;
		workersSharedData.condition.notify_all();
	}

	rampTimerThread.join();
}

/**
 * Returns the total number of entries to be read/written and number of bytes to be read/written in
 * the given benchmark phase.
 *
 * The returned numbers are per worker, so for local benchmarks they are per LocalWorker; if this
 * is the master of a distributed benchmark then they are per RemoteWorker.
 */
void WorkerManager::getPhaseNumEntriesAndBytes(const ProgArgs& progArgs, BenchPhase benchPhase,
	BenchPathType benchPathType, size_t& outNumEntriesPerWorker, uint64_t& outNumBytesPerWorker)
{
	if(benchPathType == BenchPathType_DIR)
	{
		if(progArgs.getBenchMode() == BenchMode_NETBENCH)
		{
			// note: keep this in sync with NetBenchProgArgs::printDryRunInfo()

            uint64_t numBytesTotal =
                progArgs.getNetBenchArgs().getExpectedBytesPerClient(progArgs);

			outNumEntriesPerWorker = 0;
			outNumBytesPerWorker = numBytesTotal;

			return;
		}
		else
		if(progArgs.getTreeFilePath().empty() )
		{ // standard dir mode
			switch(benchPhase)
			{
				case BenchPhase_CREATEDIRS:
				case BenchPhase_DELETEDIRS:
				case BenchPhase_PUTBUCKETACL:
				case BenchPhase_GETBUCKETACL:
				{
					outNumEntriesPerWorker = progArgs.getNumDirs();
					outNumBytesPerWorker = 0;
				} break;

				case BenchPhase_CREATEFILES:
				case BenchPhase_READFILES:
				{
					if( (benchPhase == BenchPhase_READFILES) &&
						(progArgs.getBenchMode() == BenchMode_S3) &&
						progArgs.getS3Args().getUseS3RandObjSelect() )
					{ // special case: s3 random object selection is based on randomAmount
						outNumEntriesPerWorker = 0;
						outNumBytesPerWorker = progArgs.getRandomAmount() /
								progArgs.getNumDataSetThreads(); // randamount is total, not per obj
					}
					else
					{ // normal case: based on number of dirs and files per dir
						const size_t numDirs = progArgs.getNumDirs() ? progArgs.getNumDirs() : 1;

						outNumEntriesPerWorker = numDirs * progArgs.getNumFiles();
						outNumBytesPerWorker = outNumEntriesPerWorker * progArgs.getFileSize();
					}
				} break;

				case BenchPhase_DELETEFILES:
				case BenchPhase_STATFILES:
				case BenchPhase_PUTOBJACL:
				case BenchPhase_GETOBJACL:
				case BenchPhase_LISTOBJPARALLEL:
				{
					const size_t numDirs = progArgs.getNumDirs() ? progArgs.getNumDirs() : 1;

					outNumEntriesPerWorker = numDirs * progArgs.getNumFiles();
					outNumBytesPerWorker = 0;
				} break;

				default:
				{ // e.g. sync and drop_caches
					outNumEntriesPerWorker = 0;
					outNumBytesPerWorker = 0;
				}

			} // end of switch
		}
		else
		{ // custom tree mode
			const size_t numDirs = progArgs.getCustomTreeDirs().getNumPaths();
			const size_t numFilesNonShared = progArgs.getCustomTreeFilesNonShared().getNumPaths();
			const size_t numFilesShared = progArgs.getCustomTreeFilesShared().getNumPaths();
			const uint64_t numBytesTotal =
				progArgs.getCustomTreeFilesNonShared().getNumBytesTotal() +
				progArgs.getCustomTreeFilesShared().getNumBytesTotal();
			const size_t numDataSetThreads = progArgs.getNumDataSetThreads();

			switch(benchPhase)
			{
				case BenchPhase_CREATEDIRS:
				case BenchPhase_DELETEDIRS:
				{
					outNumEntriesPerWorker = numDirs / numDataSetThreads;
					outNumBytesPerWorker = 0;
				} break;

				case BenchPhase_CREATEFILES:
				case BenchPhase_READFILES:
				{
					outNumEntriesPerWorker =
						(numFilesNonShared + numFilesShared) / numDataSetThreads;
					outNumBytesPerWorker = numBytesTotal / numDataSetThreads;
				} break;

				case BenchPhase_DELETEFILES:
				case BenchPhase_STATFILES:
				case BenchPhase_PUTOBJACL:
				case BenchPhase_GETOBJACL:
                case BenchPhase_S3MPUCOMPLETE:
				{
					outNumEntriesPerWorker =
						(numFilesNonShared + numFilesShared) / numDataSetThreads;
					outNumBytesPerWorker = 0;
				} break;

				default:
				{ // e.g. sync and drop_caches
					outNumEntriesPerWorker = 0;
					outNumBytesPerWorker = 0;
				}

			} // end of switch
		}
	}
	else
	{ // file/blockdev mode
		outNumEntriesPerWorker = progArgs.getBenchPaths().size();

		switch(benchPhase)
		{
			case BenchPhase_CREATEFILES:
			case BenchPhase_READFILES:
			{
				outNumBytesPerWorker = progArgs.getUseRandomOffsets() ?
					( progArgs.getRandomAmount() / // randamount is total, not per file
						progArgs.getNumDataSetThreads() ) :
					( (outNumEntriesPerWorker * progArgs.getFileSize() ) /
						progArgs.getNumDataSetThreads() );
			} break;

			case BenchPhase_DELETEFILES:
			{
				outNumBytesPerWorker = 0;
			} break;

			default:
			{ // e.g. sync and drop_caches
				outNumBytesPerWorker = 0;
			}

		} // end of switch
	}

	// for RemoteWorkers multiply values by number of threads on RemoteWorker
	if(!progArgs.getHostsVec().empty() )
	{
		outNumEntriesPerWorker *= progArgs.getNumThreads();
		outNumBytesPerWorker *= progArgs.getNumThreads();
	}
}

/**
 * Check BenchPathInfo from RemoteWorkers. Only valid after prepareThreads() has been called, so
 * that RemoteWorkers retrieved the corresponding details.
 *
 * @throw ProgException on error, e.g. conflicting BenchPathTypes found for different
 * 		RemoteWorkers.
 */
void WorkerManager::checkServiceBenchPathInfos()
{
	if(progArgs.getHostsVec().empty() )
		return; // nothing to do if not running as master

	BenchPathInfoVec benchPathInfos;
	benchPathInfos.reserve(progArgs.getHostsVec().size() );

	for(Worker* worker : workerVec)
		benchPathInfos.push_back(static_cast<RemoteWorker*>(worker)->getBenchPathInfo() );

	progArgs.checkServiceBenchPathInfos(benchPathInfos);
}
