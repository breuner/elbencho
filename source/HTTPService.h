#ifndef HTTPSERVICE_H_
#define HTTPSERVICE_H_

#include "ProgArgs.h"
#include "Statistics.h"
#include "workers/WorkerManager.h"

/**
 * Abstract parent class for http service to accept requests from master coordinator instance.
 */
class HTTPService
{
	public:
		HTTPService(ProgArgs& progArgs, WorkerManager& workerManager, Statistics& statistics) :
			progArgs(progArgs), workerManager(workerManager), statistics(statistics) {};
		virtual ~HTTPService() {};

		virtual void startServer() = 0;

	protected:
		ProgArgs& progArgs;
		WorkerManager& workerManager;
		Statistics& statistics;

		void daemonize();
		void checkPortAvailable();
};

#endif /* HTTPSERVICE_H_ */
