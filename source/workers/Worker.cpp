// SPDX-FileCopyrightText: 2020-2025 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "toolkits/NumaTk.h"
#include "toolkits/SignalTk.h"
#include "toolkits/TranslatorTk.h"
#include "Worker.h"
#include "WorkerException.h"


/**
 * Instantiate a worker object and call its run method.
 */
void Worker::threadStart(Worker* worker)
{
	SignalTk::registerFaultSignalHandlers(*(worker->progArgs) );

#ifdef THREADNAME_SUPPORT
    // set thread name (max 15 chars plus '\0')
    std::string threadName = "elb-wrk-" + std::to_string(worker->workerRank);
    pthread_setname_np(pthread_self(), threadName.c_str() );
#endif

	worker->run();
	worker->cleanup();
}


/**
 * Increase number of done workers and create stonewall stats if this is the first worker to
 * finish.
 */
void Worker::incNumWorkersDone()
{
	std::unique_lock<std::mutex> lock(workersSharedData->mutex); // L O C K (scoped)

	/* ensure that last worker triggers stonewall if all have !workerCanTriggerStoneWall, but only
		if this is not a server, because then master will have this trigger if needed and we don't
		want the first finishing service to trigger stonewall if the service didn't do anything */
	size_t numWorkersTotal = workersSharedData->workerVec->size();
	bool lastFinisherTrigger = progArgs->getRunAsService() ?
		false : ( (workersSharedData->numWorkersDone + 1) == numWorkersTotal);
	bool triggerStoneWall = (!stoneWallTriggered &&
		(workerGotPhaseWork || lastFinisherTrigger) );

	workersSharedData->incNumWorkersDoneUnlocked(triggerStoneWall);

	/* create stonewall stats when 1st real worker finishes. (mutex guarantees that no other worker
	   increases the done counter in the meantime) */
	if(triggerStoneWall)
	{
		for(Worker* worker : *workersSharedData->workerVec)
			worker->createStoneWallStats();
	}
}

/**
 * Increase number of done and workers with error after the current phase has finished or been
 * cancelled with an error.
 */
void Worker::incNumWorkersDoneWithError()
{
	ErrLogger(Log_DEBUG) << "Increasing done with error counter. " <<
		"WorkerRank: " << this->workerRank << std::endl;

	workersSharedData->incNumWorkersDoneWithError();
}

/**
 * Check if this worker has been friendly asked to interrupt itself. Also applies a pending ramp
 * stats reset, because this is the common safe point of all worker I/O loops.
 *
 * @throw WorkerInterruptedException if friendly ask to interrupt has been received.
 */
void Worker::checkInterruptionRequest()
{
	checkRampStatsReset();

	IF_UNLIKELY(isInterruptionRequested)
		throw WorkerInterruptedException("Received friendly request to interrupt execution.");
}

/**
 * Check if this worker has been friendly asked to interrupt itself. If it was, then call func
 * before throwing exception. Also applies a pending ramp stats reset, because this is the common
 * safe point of all worker I/O loops.
 *
 * @func function to call before throwing exception if request to interrupt was detected.
 * @throw WorkerInterruptedException if friendly ask to interrupt has been received.
 */
void Worker::checkInterruptionRequest(std::function<void()> func)
{
	checkRampStatsReset();

	IF_UNLIKELY(isInterruptionRequested)
	{
		func();
		throw WorkerInterruptedException("Received friendly request to interrupt execution.");
	}
}

/**
 * Drop everything that this worker measured during the "--ramp" warm-up period, so that only the
 * steady-state remainder of the phase gets reported. Called by the worker thread itself at one of
 * its safe points, so no lock is needed: all touched values belong to this worker and are either
 * atomic or only ever read by others after the phase has ended.
 *
 * The instant of the reset is kept as the start of this worker's measured window, so that its
 * elapsed time in the phase results matches exactly the counters that survive here.
 *
 * Note: the live latency accumulators are deliberately not reset, because they are concurrently
 * drained by the live stats thread and only affect the display of the current live interval.
 */
void Worker::applyRampStatsReset()
{
	rampResetPending = false;

	IF_UNLIKELY(phaseFinished)
		return; /* this worker is already through with the phase, so its numbers are final and
			the coordinator might be collecting them right now */

	atomicLiveOps.setToZero();
	atomicLiveOpsReadMix.setToZero();
	oldAtomicLiveOps.setToZero();
	oldAtomicLiveOpsReadMix.setToZero();

	iopsLatHisto.reset();
	iopsLatHistoReadMix.reset();
	entriesLatHisto.reset();
	entriesLatHistoReadMix.reset();

	rampEndT = std::chrono::steady_clock::now();
	isRampDone = true; // (after rampEndT, so that readers of rampEndT can rely on this flag)
}

/**
 * Apply configured NUMA and CPU core binding to calling worker thread. Do nothing if NUMA or core
 * binding is not configured.
 *
 * @throw WorkerException on error, e.g. if binding fails.
 */
void Worker::applyNumaAndCoreBinding()
{
	const IntVec& numaZonesVec = progArgs->getNumaZonesVec();
	const IntVec& cpuCoresVec = progArgs->getCPUCoresVec();

	if(!numaZonesVec.empty() || !cpuCoresVec.empty() )
		LOGGER(Log_DEBUG, __func__ << " entry: " << "workerRank: " << workerRank << "; "
			"numCores: " << std::thread::hardware_concurrency() << "; "
			"cpu core affinity list: " << NumaTk::getCurrentCPUAffinityStrHuman() << std::endl);

	if(!numaZonesVec.empty() && NumaTk::isNumaInfoAvailable() )
	{
		int zoneNum = numaZonesVec[workerRank % numaZonesVec.size() ];

		try
		{
			NumaTk::bindToNumaZone(std::to_string(zoneNum) );
		}
		catch(ProgException& e)
		{
			// turn NumaTk's ProgException into WorkerException
			throw WorkerException(e.what() );
		}
	}

	if(!cpuCoresVec.empty() )
	{
		int coreNum = cpuCoresVec[workerRank % cpuCoresVec.size() ];

		try
		{
			NumaTk::bindToCPUCore(coreNum);
		}
		catch(ProgException& e)
		{
			// turn NumaTk's ProgException into WorkerException
			throw WorkerException(e.what() );
		}
	}

	if(!numaZonesVec.empty() || !cpuCoresVec.empty() )
		LOGGER(Log_DEBUG, __func__ << " end: " << "workerRank: " << workerRank << "; "
			"numCores: " << std::thread::hardware_concurrency() << "; "
			"cpu core affinity list: " << NumaTk::getCurrentCPUAffinityStrHuman() << std::endl);
}

/**
 * Wait for notification to start with next phase.
 *
 * @oldBenchID wait for workersSharedData->currentBenchID to be different from this value.
 */
void Worker::waitForNextPhase(const buuids::uuid& oldBenchID)
{
	std::unique_lock<std::mutex> lock(workersSharedData->mutex); // L O C K (scoped)

	checkInterruptionRequest();

	while(oldBenchID == workersSharedData->currentBenchID)
	{
		workersSharedData->condition.wait(lock);
		checkInterruptionRequest();
	}
}


