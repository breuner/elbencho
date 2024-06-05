#include "toolkits/SignalTk.h"
#include "toolkits/TranslatorTk.h"
#include "LocalWorker.h"
#include "RemoteWorker.h"
#include "WorkerException.h"
#include "WorkerManager.h"


WorkerManager::~WorkerManager()
{
	deleteThreads();
}

/**
 * Interrupt and notify all workers (typically after a fatal error).
 */
void WorkerManager::interruptAndNotifyWorkersUnlocked()
{
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

	IF_UNLIKELY(WorkersSharedData::gotUserInterruptSignal)
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
	threadGroup.reserve(progArgs.getHostsVec().empty() ?
			progArgs.getNumThreads() : progArgs.getHostsVec().size() );
	workerVec.reserve(progArgs.getHostsVec().empty() ?
		progArgs.getNumThreads() : progArgs.getHostsVec().size() );

	workersSharedData.threadGroup = &threadGroup;
	workersSharedData.progArgs = &progArgs;
	workersSharedData.workerVec = &workerVec;
	workersSharedData.phaseStartT = std::chrono::steady_clock::now();
	workersSharedData.currentBenchID = buuids::nil_uuid(); // workers expect this as prep phase ID

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
 * @thow WorkerException if any of the workers encountered an error in this phase or
 * 		ProgInterruptedException if user interrupt signal was detected.
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
 * Reset num done and notify workers to start next phase.
 *
 * @newBenchPhase type of new bench phase to run.
 * @benchID new bench UUID, may be NULL if it should be randomly generated.
 */
void WorkerManager::startNextPhase(BenchPhase newBenchPhase, std::string* benchID)
{
	Logger(Log_VERBOSE) << "Starting benchmark phase: " <<
		TranslatorTk::benchPhaseToPhaseName(newBenchPhase, &progArgs) << std::endl;

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

	workersSharedData.cpuUtilFirstDone.update();
	workersSharedData.cpuUtilLastDone.update();
	workersSharedData.phaseStartT = std::chrono::steady_clock::now();

	workersSharedData.condition.notify_all();
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
		if(progArgs.getUseNetBench() )
		{
			// note: keep this in sync with Statistics::printDryRunInfoNetBench()

			size_t reqRespSizeSum = progArgs.getBlockSize() + progArgs.getNetBenchRespSize();
			size_t numBlocks = progArgs.getBlockSize() ?
				(progArgs.getFileSize() / progArgs.getBlockSize() ) : 0;
			uint64_t numBytesTotal = progArgs.getNumThreads() * numBlocks * reqRespSizeSum;

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
						!progArgs.getS3EndpointsVec().empty() &&
						progArgs.getUseS3RandObjSelect() )
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
