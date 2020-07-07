#include "LocalWorker.h"
#include "RemoteWorker.h"
#include "TranslatorTk.h"
#include "WorkerException.h"
#include "WorkerManager.h"


WorkerManager::~WorkerManager()
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
 * Kill all old threads, wait for their termination and prepare a new set of threads according to
 * the number given in progArgs.
 * (Also initializes basics of workersSharedData and set workersSharedData.phaseStartT to now.)
 *
 * In local or master mode, this will be called only once at the beginning. In service mode, this
 * will be called for each new bench preparation message.
 */
void WorkerManager::prepareThreads()
{
	interruptAndNotifyWorkers();
	joinAllThreads();

	// cleanup allocated thread objects
	for(std::thread* thread : threadGroup)
		delete(thread);

	threadGroup.resize(0);
	threadGroup.reserve(progArgs.getHostsVec().empty() ?
			progArgs.getNumThreads() : progArgs.getHostsVec().size() );

	// cleanup allocated worker objects
	for(Worker* worker : workerVec)
		delete(worker);

	workerVec.resize(0);
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

			std::thread* thread = new std::thread(Worker::threadStart, workerVec[i] );
			threadGroup.push_back(thread);
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

			std::thread* thread = new std::thread(Worker::threadStart, workerVec[i] );
			threadGroup.push_back(thread);
		}
    }

	// make sure all worker threads are ready & able before we return
	waitForWorkersDone();
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
 * Reset num done and notify workers to start next phase.
 *
 * @newBenchPhase type of new bench phase to run.
 * @benchID new bench UUID, may be NULL if it should be randomly generated.
 */
void WorkerManager::startNextPhase(BenchPhase newBenchPhase, std::string* benchID)
{
	Logger(Log_VERBOSE) << "Starting benchmark phase: " <<
		TranslatorTk::benchPhaseToPhaseName(newBenchPhase) << std::endl;

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

	workersSharedData.phaseStartT = std::chrono::steady_clock::now();

	workersSharedData.condition.notify_all();
}

/**
 * Returns the total number of entries to be read/written and number of bytes to be read/written in
 * the current phase.
 *
 * The returned numbers are per worker, so for local benchmarks they are per LocalWorker; if this
 * is the master of a distributed benchmark then they are per RemoteWorker.
 */
void WorkerManager::getPhaseNumEntriesAndBytes(size_t& outNumEntriesPerWorker,
	size_t& outNumBytesPerWorker)
{
	if(progArgs.getBenchPathType() == BenchPathType_DIR)
	{
		switch(workersSharedData.currentBenchPhase)
		{
			case BenchPhase_CREATEDIRS:
			case BenchPhase_DELETEDIRS:
			{
				outNumEntriesPerWorker = progArgs.getNumDirs();
				outNumBytesPerWorker = 0;
			} break;

			case BenchPhase_CREATEFILES:
			case BenchPhase_READFILES:
			{
				outNumEntriesPerWorker = progArgs.getNumDirs() * progArgs.getNumFiles();
				outNumBytesPerWorker = outNumEntriesPerWorker * progArgs.getFileSize();
			} break;

			case BenchPhase_DELETEFILES:
			{
				outNumEntriesPerWorker = progArgs.getNumDirs() * progArgs.getNumFiles();
				outNumBytesPerWorker = 0;
			} break;

			default:
			{ // should never happen
				outNumEntriesPerWorker = 0;
				outNumBytesPerWorker = 0;
			}

		} // end of switch
	}
	else
	{ // file/blockdev mode
		outNumEntriesPerWorker = progArgs.getBenchPaths().size();
		size_t numBytesPerFile = progArgs.getUseRandomOffsets() ?
			progArgs.getRandomAmount() : progArgs.getFileSize();

		switch(workersSharedData.currentBenchPhase)
		{
			case BenchPhase_CREATEFILES:
			case BenchPhase_READFILES:
			{
				outNumBytesPerWorker = outNumEntriesPerWorker * numBytesPerFile /
					progArgs.getNumThreads();
			} break;

			case BenchPhase_DELETEFILES:
			{
				outNumBytesPerWorker = 0;
			} break;

			default:
			{ // should never happen
				outNumBytesPerWorker = 0;
			}

		} // end of switch
	}

	// for RemoteWorkers multiply values by number of threads on RemoteWorker
	if(!progArgs.getHostsVec().empty() )
	{
		outNumEntriesPerWorker *= progArgs.getNumThreads();
		outNumBytesPerWorker *= progArgs.getNumThreads();

		if(progArgs.getIsBenchPathShared() &&
			(progArgs.getBenchPathType() != BenchPathType_DIR) )
		{ // shared bench path, so each host reads/writes its own fraction
			outNumEntriesPerWorker /= progArgs.getHostsVec().size();
			outNumBytesPerWorker /= progArgs.getHostsVec().size();
		}
	}
}

/**
 * Get BenchPathType from RemoteWorkers. Only valid after prepareThreads() has been called.
 *
 * @return BenchPathType as reported by workers as result of preparation phase.
 * @throw WorkerException if conflicting BenchPathTypes were found for different RemoteWorkers.
 */
BenchPathType WorkerManager::getBenchPathType()
{
	if(progArgs.getHostsVec().empty() )
		return progArgs.getBenchPathType();

	BenchPathType globalBenchPathType =
		static_cast<RemoteWorker*>(workerVec.at(0) )->getBenchPathType();

	for(Worker* worker : workerVec)
	{
		RemoteWorker* remoteWorker =  static_cast<RemoteWorker*>(worker);
		BenchPathType benchPathType = remoteWorker->getBenchPathType();

		if(benchPathType != globalBenchPathType)
			throw ProgException(
				"Conflicting benchmark path types detected on different service hosts. "
				"Servers with different path types: "
				"ServerA: " + static_cast<RemoteWorker*>(workerVec.at(0) )->getHost() + "; "
				"ServerB: " + remoteWorker->getHost() );
	}

	return globalBenchPathType;
}
