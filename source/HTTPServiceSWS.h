// SPDX-FileCopyrightText: 2020-2025 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

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

        void defineServerResourceInfo(HttpServer& server);
        void defineServerResourceProtocolVersion(HttpServer& server);
        void defineServerResourceStatus(HttpServer& server);
        void defineServerResourceBenchResult(HttpServer& server);
        void defineServerResourcePrepareFile(HttpServer& server);
        void defineServerResourcePreparePhase(HttpServer& server);
        void defineServerResourceStartPhase(HttpServer& server);
        void defineServerResourceInterruptPhase(HttpServer& server);
        void defineServerResourceErrorHandler(HttpServer& server);
};

#endif /* HTTPSERVICESWS_H_ */
