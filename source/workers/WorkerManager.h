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
		void cleanupThreads();
		void interruptAndNotifyWorkers();
		void joinAllThreads();
		void waitForWorkersDone();
		bool checkWorkersDoneUnlocked(size_t* outNumWorkersDone);
		bool checkWorkersDone(size_t* outNumWorkersDone);
		void checkPhaseTimeLimitUnlocked();
		void checkPhaseTimeLimit();

		void startNextPhase(BenchPhase newBenchPhase, std::string* benchID = NULL);

		static void getPhaseNumEntriesAndBytes(const ProgArgs& progArgs, BenchPhase benchPhase,
			BenchPathType benchPathType, size_t& outNumEntriesPerThread,
			uint64_t& outNumBytesPerThread);

		void checkServiceBenchPathInfos();


	private:
		ProgArgs& progArgs;
		ThreadGroup threadGroup;
		WorkerVec workerVec;
		WorkersSharedData workersSharedData;

		void interruptAndNotifyWorkersUnlocked();

		// inliners
	public:
		WorkerVec& getWorkerVec() { return workerVec; }
		WorkersSharedData& getWorkersSharedData() { return workersSharedData; }
		size_t& getNumWorkersDoneWithError() { return workersSharedData.numWorkersDoneWithError; }

		void getPhaseNumEntriesAndBytes(size_t& outNumEntriesPerThread,
			uint64_t& outNumBytesPerThread)
		{
			getPhaseNumEntriesAndBytes(progArgs, workersSharedData.currentBenchPhase,
				progArgs.getBenchPathType(), outNumEntriesPerThread, outNumBytesPerThread);
		}
};

#endif /* WORKERS_WORKERMANAGER_H_ */
