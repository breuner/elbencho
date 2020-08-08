#ifndef WORKERSSHAREDDATA_H_
#define WORKERSSHAREDDATA_H_

#include <boost/config.hpp> // for BOOST_LIKELY
#include <boost/uuid/string_generator.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
#include "Common.h"


class Worker; // forward declaration for WorkerVec;
typedef std::vector<Worker*> WorkerVec;
typedef std::vector<std::thread*> ThreadGroup;
typedef std::vector<size_t> SizeTVec;

class ProgArgs; // forward declaration to avoid including ProgArgs.h here

namespace buuids = boost::uuids;

#ifdef BOOST_UNLIKELY
	#define IF_UNLIKELY(condition)	if(BOOST_UNLIKELY(condition) )
#else // fallback for older boost versions
	#define IF_UNLIKELY(condition)	if(__builtin_expect(condition, 0) )
#endif


/**
 * Common data for all workers.
 */
class WorkersSharedData
{
	public:
		static bool gotUserInterruptSignal; // SIGINT/SIGTERM handler sets this to true
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
		size_t numWorkersDone{0}; /* number of threads that are through with current phase
			(protected by mutex, change signaled by condition) */
		size_t numWorkersDoneWithError{0}; /* number of threads that failed the current phase
			(protected by mutex, change signaled by condition) */

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

		void incNumWorkersDoneUnlocked()
		{
			numWorkersDone++;
			condition.notify_all();
		}

		/**
		 * To be called by a worker that has finished the current phase successfully.
		 */
		void incNumWorkersDone()
		{
			std::unique_lock<std::mutex> lock(mutex); // L O C K (scoped)
			incNumWorkersDoneUnlocked();
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




#endif /* WORKERSSHAREDDATA_H_ */
