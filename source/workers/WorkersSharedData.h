#ifndef WORKERS_WORKERSSHAREDDATA_H_
#define WORKERS_WORKERSSHAREDDATA_H_

#include <boost/uuid/string_generator.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
#include "CPUUtil.h"
#include "Common.h"
#include "S3UploadStore.h"


class Worker; // forward declaration for WorkerVec;
typedef std::vector<Worker*> WorkerVec;
typedef std::vector<std::thread*> ThreadGroup;

class ProgArgs; // forward declaration to avoid including ProgArgs.h here

namespace buuids = boost::uuids;


/**
 * Common data for all workers.
 */
class WorkersSharedData
{
	public:
		static bool gotUserInterruptSignal; // main thread SIGINT/SIGTERM handler sets this to true
		static bool isPhaseTimeExpired; // expired progArgs::timeLimitSecs sets this to true

		ThreadGroup* threadGroup;
		ProgArgs* progArgs;
		WorkerVec* workerVec;

		std::chrono::steady_clock::time_point phaseStartT; // time when main thread starts phase

		std::mutex mutex; // also protects console err logs to not mix lines
		std::condition_variable condition;
		BenchPhase currentBenchPhase{BenchPhase_IDLE}; // changed together with new bench ID
		buuids::uuid currentBenchID = buuids::nil_uuid(); /* changed ID informs workers about next
			phase start */
		size_t numWorkersDone{0}; /* number of threads/services that are through with current phase
			(protected by mutex, change signaled by condition) */
		size_t numWorkersDoneWithError{0}; /* number of threads that failed the current phase
			(protected by mutex, change signaled by condition) */
		CPUUtil cpuUtilFirstDone; // 1st update() by WorkerManager, 2nd update() by first finisher
		CPUUtil cpuUtilLastDone; // 1st update() by WorkerManager, 2nd update() by last finisher

		void incNumWorkersDoneUnlocked(bool triggerStoneWall);

	// inliners
	public:
		void resetNumWorkersDoneUnlocked()
		{
			numWorkersDone = 0;
			numWorkersDoneWithError = 0;
			condition.notify_all();
		}

		/**
		 * To be called before a new phase starts. Also resets the finish time.
		 */
		void resetNumWorkersDone()
		{
			std::unique_lock<std::mutex> lock(mutex); // L O C K (scoped)
			resetNumWorkersDoneUnlocked();
		}

		/**
		 * To be called by a worker that has finished the current phase successfully.
		 *
		 * Note: Probably inappropriate to call this instead of incNumWorkersDoneUnlocked() (while
		 * mutex locked by caller), because other bits like stonewall stats need to be updated as
		 * well while the mutex is locked.
		 *
		 * @triggerStoneWall true if this thread triggers the "first done" stonewall stats. This
		 * 		cannot be checked via "numWorkersDone==1" because the first finisher might just not
		 * 		have gotten any work assigned and thus doesn't trigger the stonewall stats.
		 */
		void incNumWorkersDone(bool triggerStoneWall)
		{
			std::unique_lock<std::mutex> lock(mutex); // L O C K (scoped)
			incNumWorkersDoneUnlocked(triggerStoneWall);
		}

		/**
		 * To be called by a worker that has finished or cancelled the current phase with an error.
		 */
		void incNumWorkersDoneWithError()
		{
			std::unique_lock<std::mutex> lock(mutex); // L O C K (scoped)
			numWorkersDoneWithError++;
			condition.notify_all();
		}

};




#endif /* WORKERS_WORKERSSHAREDDATA_H_ */
