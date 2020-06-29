#ifndef HTTPSERVICE_H_
#define HTTPSERVICE_H_

#include "ProgArgs.h"
#include "Statistics.h"
#include "WorkerManager.h"

/**
 * Runs the http service to accept requests from central master coordinator.
 */
class HTTPService
{
	public:
		HTTPService(ProgArgs& progArgs, WorkerManager& workerManager, Statistics& statistics) :
			progArgs(progArgs), workerManager(workerManager), statistics(statistics) {};

		void startServer();

	private:
		ProgArgs& progArgs;
		WorkerManager& workerManager;
		Statistics& statistics;

		void daemonize();
};

#endif /* HTTPSERVICE_H_ */
