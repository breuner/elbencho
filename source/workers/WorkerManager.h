// SPDX-FileCopyrightText: 2020-2025 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef WORKERS_WORKERMANAGER_H_
#define WORKERS_WORKERMANAGER_H_

#include "Worker.h"
#include "WorkersSharedData.h"


class WorkerManager
{
	public:
		explicit WorkerManager(ProgArgs& progArgs) : progArgs(progArgs) {}
		~WorkerManager();

		void prepareThreads();
		void deleteThreads();
		void interruptAndNotifyWorkers();
		void joinAllThreads();
		void waitForWorkersDone();
		void cleanupWorkersAfterPhaseDone();
		bool checkWorkersDoneUnlocked(size_t* outNumWorkersDone);
		bool checkWorkersDone(size_t* outNumWorkersDone);
		void checkPhaseTimeLimitUnlocked();
		void checkPhaseTimeLimit();

		void startNextPhase(BenchPhase newBenchPhase, std::string* benchID = NULL);
		void stopRampTimer();

		static void getPhaseNumEntriesAndBytes(const ProgArgs& progArgs, BenchPhase benchPhase,
			BenchPathType benchPathType, size_t& outNumEntriesPerThread,
			uint64_t& outNumBytesPerThread);

		void checkServiceBenchPathInfos();


	private:
		ProgArgs& progArgs;
		ThreadGroup threadGroup;
		WorkerVec workerVec;
		WorkersSharedData workersSharedData;

		std::thread rampTimerThread;
		bool rampTimerStopRequested{false}; // protected by workersSharedData.mutex

		void interruptAndNotifyWorkersUnlocked();
		static bool hasRampPhase(BenchPhase benchPhase);
		void startRampTimer();
		void rampTimerLoop(buuids::uuid benchID,
			std::chrono::steady_clock::time_point rampDeadlineT);

		// inliners
	public:
		WorkerVec& getWorkerVec() { return workerVec; }
		WorkersSharedData& getWorkersSharedData() { return workersSharedData; }
		size_t& getNumWorkersDoneWithError() { return workersSharedData.numWorkersDoneWithError; }

		void getPhaseNumEntriesAndBytes(size_t& outNumEntriesPerThread,
			uint64_t& outNumBytesPerThread) const
		{
			getPhaseNumEntriesAndBytes(progArgs, workersSharedData.currentBenchPhase,
				progArgs.getBenchPathType(), outNumEntriesPerThread, outNumBytesPerThread);
		}
};

#endif /* WORKERS_WORKERMANAGER_H_ */
