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
		AtomicLiveOps atomicLiveOpsReadMix; // done in current phase
		AtomicLiveOps oldAtomicLiveOps; // copy of old atomicLiveOps for diff stats
		AtomicLiveOps oldAtomicLiveOpsReadMix; // copy of old atomicLiveOps for diff stats
		std::atomic_bool stoneWallTriggered{false}; // true after 1st worker triggered stonewall
		LiveOps stoneWallOps; // done values when stonewall was hit
		LiveOps stoneWallOpsReadMix; // done values when stonewall was hit
		LatencyHistogram iopsLatHisto; // ops latency histogram (valid only at phase end)
		LatencyHistogram iopsLatHistoReadMix; // ops latency histogram (valid only at phase end)
		LatencyHistogram entriesLatHisto; // entry latency histogram (valid only at phase end)
		LatencyHistogram entriesLatHistoReadMix; // entry lat histogram (valid only at phase end)

		virtual void run() = 0;
		virtual void cleanup() {}; // cleanup that needs to be done after run()

		void incNumWorkersDone();
		void incNumWorkersDoneWithError();
		void waitForNextPhase(const buuids::uuid& oldBenchID);
		void checkInterruptionRequest();
		void checkInterruptionRequest(std::function<void()> func);
		void applyNumaAndCoreBinding();

	// inliners
	public:
		const SizeTVec& getElapsedUSecVec() const
			{ return elapsedUSecVec; }
		const LatencyHistogram& getIOPSLatencyHistogram() const
			{ return iopsLatHisto; }
		const LatencyHistogram& getIOPSLatencyHistogramReadMix() const
			{ return iopsLatHistoReadMix; }
		const LatencyHistogram& getEntriesLatencyHistogram() const
			{ return entriesLatHisto; }
		const LatencyHistogram& getEntriesLatencyHistogramReadMix() const
			{ return entriesLatHistoReadMix; }

		virtual void resetStats()
		{
			phaseFinished = false;

			elapsedUSecVec.resize(0);
			atomicLiveOps.setToZero();
			atomicLiveOpsReadMix.setToZero();
			oldAtomicLiveOps.setToZero();
			oldAtomicLiveOpsReadMix.setToZero();
			stoneWallTriggered = false;
			stoneWallOps.setToZero();
			stoneWallOpsReadMix.setToZero();
			iopsLatHisto.reset();
			iopsLatHistoReadMix.reset();
			entriesLatHisto.reset();
			entriesLatHistoReadMix.reset();
		}

		/**
		 * Get sum of normal liveOps and liveOpsReadMix.
		 */
		void getLiveOpsCombined(LiveOps& outLiveOpsCombined) const
		{
			atomicLiveOps.getAsLiveOps(outLiveOpsCombined);
			atomicLiveOpsReadMix.getAndAddLiveOps(outLiveOpsCombined);
		}

		/**
		 * Get normal liveOps and liveOpsReadMix.
		 */
		void getLiveOps(LiveOps& outSumLiveOps, LiveOps& outSumLiveOpsReadMix) const
		{
			atomicLiveOps.getAsLiveOps(outSumLiveOps);
			atomicLiveOpsReadMix.getAsLiveOps(outSumLiveOpsReadMix);
		}

		/**
		 * Add current live ops values of this worker to given outSumLiveOps.
		 */
		void getAndAddLiveOps(LiveOps& outSumLiveOps, LiveOps& outSumLiveOpsReadMix) const
		{
			atomicLiveOps.getAndAddLiveOps(outSumLiveOps);
			atomicLiveOpsReadMix.getAndAddLiveOps(outSumLiveOpsReadMix);
		}

		/**
		 * Add stonewall ops values of this worker to given outSumStoneWallOps.
		 */
		void getAndAddStoneWallOps(LiveOps& outSumStoneWallOps,
			LiveOps& outSumStoneWallOpsReadMix) const
		{
			stoneWallOps.getAndAddOps(outSumStoneWallOps);
			stoneWallOpsReadMix.getAndAddOps(outSumStoneWallOpsReadMix);
		}

		/**
		 * Store difference of current and old live ops in outLiveOpsDiff and copy current
		 * live ops to old live ops.
		 */
		void getAndResetDiffStats(LiveOps& outLiveOpsDiff, LiveOps& outLiveOpsReadMixDiff)
		{
			outLiveOpsDiff.numEntriesDone = atomicLiveOps.numEntriesDone -
				oldAtomicLiveOps.numEntriesDone.exchange(atomicLiveOps.numEntriesDone);
			outLiveOpsDiff.numBytesDone = atomicLiveOps.numBytesDone -
				oldAtomicLiveOps.numBytesDone.exchange(atomicLiveOps.numBytesDone);
			outLiveOpsDiff.numIOPSDone = atomicLiveOps.numIOPSDone -
				oldAtomicLiveOps.numIOPSDone.exchange(atomicLiveOps.numIOPSDone);

			outLiveOpsReadMixDiff.numEntriesDone = atomicLiveOpsReadMix.numEntriesDone -
				oldAtomicLiveOpsReadMix.numEntriesDone.exchange(
					atomicLiveOpsReadMix.numEntriesDone);
			outLiveOpsReadMixDiff.numBytesDone = atomicLiveOpsReadMix.numBytesDone -
				oldAtomicLiveOpsReadMix.numBytesDone.exchange(
					atomicLiveOpsReadMix.numBytesDone);
			outLiveOpsReadMixDiff.numIOPSDone = atomicLiveOpsReadMix.numIOPSDone -
				oldAtomicLiveOpsReadMix.numIOPSDone.exchange(
						atomicLiveOpsReadMix.numIOPSDone);
		}

		/**
		 * Store difference of current and old live ops in outLiveOpsDiff and copy current
		 * live ops to old live ops.
		 */
		void getAndResetDiffStatsCombined(LiveOps& outLiveOpsDiff)
		{
			LiveOps liveOpsReadMixDiff;

			getAndResetDiffStats(outLiveOpsDiff, liveOpsReadMixDiff);

			liveOpsReadMixDiff.getAndAddOps(outLiveOpsDiff);
		}

		/**
		 * Creates a copy of the current num{Entries,Bytes}Done in the stonewall stats.
		 */
		void createStoneWallStats()
		{
			stoneWallTriggered = true;

			atomicLiveOps.getAsLiveOps(stoneWallOps);
			atomicLiveOpsReadMix.getAsLiveOps(stoneWallOpsReadMix);
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
