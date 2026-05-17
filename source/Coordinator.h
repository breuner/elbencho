// SPDX-FileCopyrightText: 2020-2025 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

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

        static void registerInterruptSignalHandlers();
        static void handleInterruptSignal(int signal);

	private:
		ProgArgs& progArgs;
		WorkerManager workerManager;
		Statistics statistics{progArgs, workerManager};


		void waitForUserDefinedStartTime();
		void waitForServicesReady();
		void checkInterruptionBetweenPhases();
		void runBenchmarkPhase(BenchPhase newBenchPhase);
		void runSyncAndDropCaches();
		void runBenchmarks();
		void rotateHosts();
};

#endif /* COORDINATOR_H_ */
