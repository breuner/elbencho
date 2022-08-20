#ifndef HTTPSERVICEUWS_H_
#define HTTPSERVICEUWS_H_

#ifdef ALTHTTPSVC_SUPPORT

#include <App.h> // this is the main include file for uWS (MicroWebSockets)
#include "HTTPService.h"

/**
 * Runs the http service to accept requests from central master coordinator.
 *
 * This is based on uWS (Micro Web Sockets):
 * https://github.com/uNetworking/uWebSockets
 */
class HTTPServiceUWS : public HTTPService
{
	public:
		HTTPServiceUWS(ProgArgs& progArgs, WorkerManager& workerManager, Statistics& statistics) :
			HTTPService(progArgs, workerManager, statistics) {};

		virtual void startServer();

	private:
	    struct us_listen_socket_t* globalListenSocket; // init in .listen, later used to quit server

		void defineServerResources(uWS::App& uWSApp);
		void logReqAndError(uWS::HttpResponse<false>* res, std::string urlStr,
			std::string queryStr);
};

#endif // ALTHTTPSVC_SUPPORT

#endif /* HTTPSERVICEUWS_H_ */
