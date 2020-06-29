#ifndef WORKERMANAGER_H_
#define WORKERMANAGER_H_

#include "Worker.h"
#include "WorkersSharedData.h"


class WorkerManager
{
	public:
		explicit WorkerManager(const ProgArgs& progArgs) : progArgs(progArgs) {}
		~WorkerManager();

		void prepareThreads();
		void interruptAndNotifyWorkers();
		void joinAllThreads();
		void waitForWorkersDone();
		bool checkWorkersDoneUnlocked(size_t* outNumWorkersDone);
		bool checkWorkersDone(size_t* outNumWorkersDone);
		void checkPhaseTimeLimitUnlocked();
		void checkPhaseTimeLimit();

		void startNextPhase(BenchPhase newBenchPhase, std::string* benchID = NULL);

		void getPhaseNumEntriesAndBytes(size_t& outNumEntriesPerThread,
			size_t& outNumBytesPerThread);
		BenchPathType getBenchPathType();


	private:
		const ProgArgs& progArgs;
		ThreadGroup threadGroup;
		WorkerVec workerVec;
		WorkersSharedData workersSharedData;

		void interruptAndNotifyWorkersUnlocked();

		// inliners
	public:
		WorkerVec& getWorkerVec() { return workerVec; }
		WorkersSharedData& getWorkersSharedData() { return workersSharedData; }
		size_t& getNumWorkersDoneWithError() { return workersSharedData.numWorkersDoneWithError; }
};

#endif /* WORKERMANAGER_H_ */
