#ifndef COORDINATOR_H_
#define COORDINATOR_H_

#include "Statistics.h"
#include "workers/Worker.h"
#include "workers/WorkerManager.h"


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
		void registerInterruptSignalHandlers();
};

#endif /* COORDINATOR_H_ */
