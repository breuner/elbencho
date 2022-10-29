#ifndef HTTPSERVICESWS_H_
#define HTTPSERVICESWS_H_

#include <server_http.hpp>
#include "HTTPService.h"

using HttpServer = SimpleWeb::Server<SimpleWeb::HTTP>;

/**
 * Runs the http service to accept requests from central master coordinator.
 *
 * This is based on Simple Web Server:
 * https://gitlab.com/eidheim/Simple-Web-Server
 */
class HTTPServiceSWS : public HTTPService
{
	public:
		HTTPServiceSWS(ProgArgs& progArgs, WorkerManager& workerManager, Statistics& statistics) :
			HTTPService(progArgs, workerManager, statistics) {};

		virtual void startServer();

	private:
		void defineServerResources(HttpServer& server);
};

#endif /* HTTPSERVICESWS_H_ */
