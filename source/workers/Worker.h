#ifndef WORKERS_WORKER_H_
#define WORKERS_WORKER_H_

#include <iostream>
#include "LatencyHistogram.h"
#include "LiveOps.h"
#include "ProgArgs.h"
#include "WorkersSharedData.h"


/**
 * Generic interface for workers. Behind this, there can be a simple LocalWorker, representing a
 * single local thread. Or it can be a RemoteWorker, representing multiple threads on the remote
 * host, resulting in an array of values with one element per remote worker thread.
 */
class Worker
{
	public:
		explicit Worker(WorkersSharedData* workersSharedData, size_t workerRank) :
			workersSharedData(workersSharedData), progArgs(workersSharedData->progArgs),
			workerRank(workerRank)
		{
			resetStats();
		}

		virtual ~Worker() {}

		static void threadStart(Worker* worker);


	protected:
		WorkersSharedData* workersSharedData; // common data for all workers
		ProgArgs* progArgs; // shortcut for member of workersSharedData
		size_t workerRank; // rank of this worker in range 0 to numWorkers-1

		std::atomic_bool phaseFinished; /* true after finishPhase() until resetStats() to prevent
			finishPhase() inc'ing done counter twice on interrupt in waitForNextPhase() */
		UInt64Vec elapsedUSecVec; /* Microsecs. Only valid when phase completed successfully. For
			LocalWorker: finish of only thread; for RemoteWorker: finish of each worker on host */
		std::atomic_bool isInterruptionRequested{false}; // set true to request self-termination
		AtomicLiveOps atomicLiveOps; // done in current phase
		AtomicLiveOps atomicLiveRWMixReadOps; // read ops done in current write phase with rwmix>0
		AtomicLiveOps oldAtomicLiveOps; // copy of old atomicLiveOps for diff stats
		std::atomic_bool stoneWallTriggered{false}; // true after 1st worker triggered stonewall
		LiveOps stoneWallOps; // done values when stonewall was hit
		LiveOps stoneWallRWMixReadOps; // read ops done values when stonewall was hit with rwmix>0
		LatencyHistogram iopsLatHisto; // read/write ops latency histogram (valid only at phase end)
		LatencyHistogram entriesLatHisto; // entry latency histogram (valid only at phase end)

		virtual void run() = 0;
		virtual void cleanup() {}; // cleanup that needs to be done after run()

		void incNumWorkersDone();
		void incNumWorkersDoneWithError();
		void waitForNextPhase(const buuids::uuid& oldBenchID);
		void checkInterruptionRequest();
		void checkInterruptionRequest(std::function<void()> func);
		void applyNumaBinding();

	// inliners
	public:
		const SizeTVec& getElapsedUSecVec() const { return elapsedUSecVec; }
		const LatencyHistogram& getIOPSLatencyHistogram() const { return iopsLatHisto; }
		const LatencyHistogram& getEntriesLatencyHistogram() const { return entriesLatHisto; }

		virtual void resetStats()
		{
			phaseFinished = false;

			elapsedUSecVec.resize(0);
			atomicLiveOps.setToZero();
			atomicLiveRWMixReadOps.setToZero();
			oldAtomicLiveOps.setToZero();
			stoneWallTriggered = false;
			stoneWallOps.setToZero();
			stoneWallRWMixReadOps.setToZero();
			iopsLatHisto.reset();
			entriesLatHisto.reset();
		}

		void getLiveOps(LiveOps& outLiveOps) const
		{
			atomicLiveOps.getAsLiveOps(outLiveOps);
		}

		/**
		 * Add current live ops values of this worker to given outSumLiveOps.
		 */
		void getAndAddLiveOps(LiveOps& outSumLiveOps) const
		{
			atomicLiveOps.getAndAddLiveOps(outSumLiveOps);
		}

		/**
		 * Add current live ops values of this worker to given outSumLiveOps.
		 */
		void getAndAddLiveOps(LiveOps& outSumLiveOps, LiveOps& outSumLiveRWMixReadOps) const
		{
			atomicLiveOps.getAndAddLiveOps(outSumLiveOps);
			atomicLiveRWMixReadOps.getAndAddLiveOps(outSumLiveRWMixReadOps);
		}

		/**
		 * Add stonewall ops values of this worker to given outSumStoneWallOps.
		 */
		void getAndAddStoneWallOps(LiveOps& outSumStoneWallOps) const
		{
			stoneWallOps.getAndAddOps(outSumStoneWallOps);
		}

		/**
		 * Add stonewall ops values of this worker to given outSumStoneWallOps.
		 */
		void getAndAddStoneWallOps(LiveOps& outSumStoneWallOps,
			LiveOps& outSumStoneWallRWMixReadOps) const
		{
			stoneWallOps.getAndAddOps(outSumStoneWallOps);
			stoneWallRWMixReadOps.getAndAddOps(outSumStoneWallRWMixReadOps);
		}

		/**
		 * Store differnce of current and old live ops in outLiveOpsDiff and copy current
		 * live ops to old live ops.
		 */
		void getAndResetDiffStats(LiveOps& outLiveOpsDiff)
		{
			outLiveOpsDiff.numEntriesDone = atomicLiveOps.numEntriesDone -
					oldAtomicLiveOps.numEntriesDone.exchange(atomicLiveOps.numEntriesDone);
			outLiveOpsDiff.numBytesDone = atomicLiveOps.numBytesDone -
					oldAtomicLiveOps.numBytesDone.exchange(atomicLiveOps.numBytesDone);
			outLiveOpsDiff.numIOPSDone = atomicLiveOps.numIOPSDone -
					oldAtomicLiveOps.numIOPSDone.exchange(atomicLiveOps.numIOPSDone);
		}

		/**
		 * Creates a copy of the current num{Entries,Bytes}Done in the stonewall stats.
		 */
		void createStoneWallStats()
		{
			stoneWallTriggered = true;

			atomicLiveOps.getAsLiveOps(stoneWallOps);
			atomicLiveRWMixReadOps.getAsLiveOps(stoneWallRWMixReadOps);
		}

		/**
		 * Friendly ask for the worker to terminate ifself. Workers check this in regular intervals.
		 */
		void interruptExecution()
		{
			isInterruptionRequested = true;
		}

};

#endif /* WORKERS_WORKER_H_ */
