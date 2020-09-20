#ifndef COORDINATOR_H_
#define COORDINATOR_H_

#include "Statistics.h"
#include "Worker.h"
#include "WorkerManager.h"


/**
 * The workers coordinator for the benchmarks.
 */
class Coordinator
{
	public:
		explicit Coordinator(ProgArgs& progArgs) : progArgs(progArgs), workerManager(progArgs) {};

		int main();

	private:
		ProgArgs& progArgs;
		WorkerManager workerManager;
		Statistics statistics{progArgs, workerManager};

		static void handleInterruptSignal(int signal);

		void waitForUserDefinedStartTime();
		void checkInterruptionBetweenPhases();
		void runBenchmarkPhase(BenchPhase newBenchPhase);
		void runSyncAndDropCaches();
		void runBenchmarks();
		void registerSignalHandlers();
};

#endif /* COORDINATOR_H_ */
