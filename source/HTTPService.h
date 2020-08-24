#ifndef HTTPSERVICE_H_
#define HTTPSERVICE_H_

#include <server_http.hpp>
#include "ProgArgs.h"
#include "Statistics.h"
#include "WorkerManager.h"

using HttpServer = SimpleWeb::Server<SimpleWeb::HTTP>;

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

		void defineServerResources(HttpServer& server);
		void daemonize();
		void checkPortAvailable();
};

#endif /* HTTPSERVICE_H_ */
