#include <server_http.hpp>
#include <sys/file.h>
#include <unistd.h>
#include "HTTPService.h"
#include "ProgException.h"
#include "RemoteWorker.h"

#define SERVICE_LOG_DIR			"/tmp"
#define SERVICE_LOG_FILEPREFIX	EXE_NAME "-service."
#define SERVICE_LOG_FILEMODE	(S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)


using HttpServer = SimpleWeb::Server<SimpleWeb::HTTP>;


/**
 * Start HTTP service. Does not return on success.
 *
 * @throw ProgException on error, e.g. binding to configured port failed.
 */
void HTTPService::startServer()
{
	HttpServer server;
	server.config.port = progArgs.getServicePort();

	server.resource[HTTPSERVERPATH_INFO]["GET"] =
		[](std::shared_ptr<HttpServer::Response> response,
			std::shared_ptr<HttpServer::Request> request)
	{
		Logger(Log_VERBOSE) << "HTTP: " << request->path << "?" <<
			request->query_string << std::endl;

		std::stringstream stream;
		stream << "<h1>Request from " << request->remote_endpoint_address()  <<
			":" << request->remote_endpoint_port() << "</h1>";

		stream << request->method << " " << request->path << " HTTP/" <<
			request->http_version;

		stream << "<h2>Query Fields</h2>";
		auto query_fields = request->parse_query_string();
		for(auto &field : query_fields)
		stream << field.first << ": " << field.second << "<br>";

		stream << "<h2>Header Fields</h2>";
		for(auto &field : request->header)
		stream << field.first << ": " << field.second << "<br>";

		response->write(stream);
	};

	server.resource[HTTPSERVERPATH_PROTOCOLVERSION]["GET"] =
		[&, this](std::shared_ptr<HttpServer::Response> response,
			std::shared_ptr<HttpServer::Request> request)
	{
		Logger(Log_VERBOSE) << "HTTP: " << request->path << "?" <<
			request->query_string << std::endl;

		response->write(HTTP_PROTOCOLVERSION);
	};

	server.resource[HTTPSERVERPATH_STATUS]["GET"] =
		[&, this](std::shared_ptr<HttpServer::Response> response,
			std::shared_ptr<HttpServer::Request> request)
	{
		Logger(Log_VERBOSE) << "HTTP: " << request->path << "?" <<
			request->query_string << std::endl;

		std::stringstream stream;

		bpt::ptree tree;

		statistics.getLiveStatsAsPropertyTree(tree);

		bpt::write_json(stream, tree, true);

		response->write(stream);
	};

	server.resource[HTTPSERVERPATH_BENCHRESULT]["GET"] =
		[&, this](std::shared_ptr<HttpServer::Response> response,
			std::shared_ptr<HttpServer::Request> request)
	{
		Logger(Log_VERBOSE) << "HTTP: " << request->path << "?" <<
			request->query_string << std::endl;

		try
		{
			std::stringstream stream;

			bpt::ptree tree;

			statistics.getBenchResultAsPropertyTree(tree);

			bpt::write_json(stream, tree, true);

			statistics.printPhaseResults(); // show results when running in foreground

			response->write(stream);
		}
		catch(const std::exception& e)
		{
			std::stringstream stream;
			stream << e.what();
			response->write(Web::StatusCode::client_error_bad_request, stream);
			std::cerr << stream.str() << std::endl; // todo remove
		}
	};

	server.resource[HTTPSERVERPATH_PREPAREPHASE]["POST"] =
		[&, this](std::shared_ptr<HttpServer::Response> response,
			std::shared_ptr<HttpServer::Request> request)
	{
		Logger(Log_VERBOSE) << "HTTP: " << request->path << "?" <<
			request->query_string << std::endl;

		try
		{
			// check protocol version for compatibility

			auto query_fields = request->parse_query_string();

			auto iter = query_fields.find(XFER_PREP_PROTCOLVERSION);
			if(iter == query_fields.end() )
				throw ProgException("Missing parameter: " XFER_PREP_PROTCOLVERSION);

			std::string masterProtoVer = iter->second;
			if(masterProtoVer != HTTP_PROTOCOLVERSION)
				throw ProgException("Protocol version mismatch. "
					"Service version: " HTTP_PROTOCOLVERSION "; "
					"Received master version: " + masterProtoVer);

			// read config values as json

			bpt::ptree recvTree;
			bpt::read_json(request->content, recvTree);

			// prepare environment for new benchmarks

			/* (we update progArgs and workers have pointers to progArgs (e.g. pathFDs), so kill any
				running workers first) */
			workerManager.interruptAndNotifyWorkers();
			workerManager.joinAllThreads();

			LoggerBase::clearErrHistory();

			progArgs.setFromPropertyTree(recvTree);

			workerManager.prepareThreads();

			// prepare response

			std::stringstream replyStream;
			bpt::ptree replyTree;

			replyTree.put(XFER_PREP_BENCHPATHTYPE, progArgs.getBenchPathType() );
			replyTree.put(XFER_PREP_ERRORHISTORY, LoggerBase::getErrHistory() );

			bpt::write_json(replyStream, replyTree, true);

			response->write(replyStream);
		}
		catch(const std::exception& e)
		{
			std::stringstream stream;
			stream << e.what();
			response->write(Web::StatusCode::client_error_bad_request, stream);
			std::cerr << stream.str() << std::endl; // todo remove
		}
	};

	server.resource[HTTPSERVERPATH_STARTPHASE]["GET"] =
		[&, this](std::shared_ptr<HttpServer::Response> response,
			std::shared_ptr<HttpServer::Request> request)
	{
		Logger(Log_VERBOSE) << "HTTP: " << request->path << "?" <<
			request->query_string << std::endl;

		std::stringstream stream;

		auto query_fields = request->parse_query_string();

		BenchPhase benchPhase;
		std::string benchID;

		auto iter = query_fields.find(XFER_START_BENCHPHASECODE);
		if(iter != query_fields.end() )
			benchPhase = (BenchPhase)std::stoi(iter->second);
		else
		{ // bench phase is a required parameter
			stream << "Missing parameter: " XFER_START_BENCHPHASECODE;
			response->write(Web::StatusCode::client_error_bad_request, stream);
			return;
		}

		iter = query_fields.find(XFER_START_BENCHID);
		if(iter != query_fields.end() )
			benchID = iter->second;

		workerManager.startNextPhase(benchPhase, benchID.empty() ? NULL : &benchID);

		response->write(LoggerBase::getErrHistory() );
	};

	server.resource[HTTPSERVERPATH_INTERRUPTPHASE]["GET"] =
		[&, this](std::shared_ptr<HttpServer::Response> response,
			std::shared_ptr<HttpServer::Request> request)
	{
		Logger(Log_VERBOSE) << "HTTP: " << request->path << "?" <<
			request->query_string << std::endl;

		auto query_fields = request->parse_query_string();

		bool quitAfterInterrupt = false;

		auto iter = query_fields.find(XFER_INTERRUPT_QUIT);
		if(iter != query_fields.end() )
			quitAfterInterrupt = true;

		size_t numWorkersDone;

		bool areWorkersDone = workerManager.checkWorkersDone(&numWorkersDone);

		if(areWorkersDone)
			workerManager.startNextPhase(BenchPhase_TERMINATE);
		else
			workerManager.interruptAndNotifyWorkers();

		workerManager.joinAllThreads();

		progArgs.resetBenchPath();

		response->write(LoggerBase::getErrHistory() );

		if(quitAfterInterrupt)
		{
			LOGGER(Log_NORMAL, "Shutting down as requested by client. "
				"Client: " << request->remote_endpoint_address() << std::endl);

			exit(0);
		}
	};

	server.on_error = [](std::shared_ptr<HttpServer::Request> request, const Web::error_code& ec)
	{
		// handle server/protocol errors like disconnect here

		/* connection timeouts will also call this handler with ec set to
		   SimpleWeb::errc::operation_canceled */

		if( (ec.category() == boost::asio::error::misc_category) &&
			(ec.value() == boost::asio::error::eof) )
		{
			LOGGER(Log_VERBOSE, "HTTP client disconnect. " <<
				"Client: " << request->remote_endpoint_address() << ":" <<
				request->remote_endpoint_port() << std::endl);
			return;
		}

		LOGGER(Log_NORMAL, "HTTP server error. Message: " << ec.message() << "; "
			"Client: " << request->remote_endpoint_address() << ":" <<
			request->remote_endpoint_port() << "; "
			"ErrorCode: " << ec.value() << "; "
			"ErrorCategory: " << ec.category().name() << std::endl);
	};

	if(!progArgs.getRunServiceInForeground() )
		daemonize(); // daemonize process in background

	try
	{
		// start http server. call won't return if all goes well.
		server.start();
	}
	catch(std::exception& e)
	{
		throw ProgException(std::string("HTTP service failed. Reason: ") + e.what() );
	}
}

/**
 * Daemonize this process into background and switch stdout/stderr to logfile.
 *
 * Will exit(1) the process on error.
 *
 * Note: No other threads may be running when daemon() is called, otherwise things will get into
 * undefined state.
 */
void HTTPService::daemonize()
{
	std::string logfile = SERVICE_LOG_DIR "/"
		SERVICE_LOG_FILEPREFIX + std::to_string(progArgs.getServicePort() ) + ".log";

	int logFileFD = open(logfile.c_str(), O_CREAT | O_WRONLY | O_APPEND, SERVICE_LOG_FILEMODE);
	if(logFileFD == -1)
	{
		ERRLOGGER(Log_NORMAL, "Failed to open logfile. Path: " << logfile << "; "
			"SysErr: " << strerror(errno) << std::endl);
		exit(1);
	}

	// try to get exclusive lock on logfile to make sure no other instance is using it
	int lockRes = flock(logFileFD, LOCK_EX | LOCK_NB);
	if(lockRes == -1)
	{
		if(errno == EWOULDBLOCK)
			ERRLOGGER(Log_NORMAL, "Unable to get exclusive lock on logfile. Probably another "
				"instance is running and using the same service port. "
				"Path: " << logfile << "; "
				"Port: " << progArgs.getServicePort() << std::endl);
		else
			ERRLOGGER(Log_NORMAL, "Failed to get exclusive lock on logfile. "
				"Path: " << logfile << "; "
				"Port: " << progArgs.getServicePort() << "; "
				"SysErr: " << strerror(errno) << std::endl);

		exit(1);
	}

	std::cout << "Daemonizing into background... Logfile: " << logfile << std::endl;

	// file locked, so trunc to 0 to delete messages from old instances
	int truncRes = ftruncate(logFileFD, 0);
	if(truncRes == -1)
	{
		ERRLOGGER(Log_NORMAL, "Failed to truncate logfile. Path: " << logfile << "; "
			"SysErr: " << strerror(errno) << std::endl);
		exit(1);
	}

	int devNullFD = open("/dev/null", O_RDONLY);
	if(devNullFD == -1)
	{
		ERRLOGGER(Log_NORMAL, "Failed to open /dev/null to daemonize. "
			"SysErr: " << strerror(errno) << std::endl);
		exit(1);
	}

	// close stdin and replace by /dev/null
	int stdinDup = dup2(devNullFD, STDIN_FILENO);
	if(stdinDup == -1)
	{
		ERRLOGGER(Log_NORMAL, "Failed to replace stdin to daemonize. "
			"SysErr: " << strerror(errno) << std::endl);
		exit(1);
	}

	// close stdout and replace by logfile
	int stdoutDup = dup2(logFileFD, STDOUT_FILENO);
	if(stdoutDup == -1)
	{
		ERRLOGGER(Log_NORMAL, "Failed to replace stdout to daemonize. "
			"SysErr: " << strerror(errno) << std::endl);
		exit(1);
	}

	// close stdout and replace by logfile
	int stderrDup = dup2(logFileFD, STDERR_FILENO);
	if(stderrDup == -1)
	{
		ERRLOGGER(Log_NORMAL, "Failed to replace stderr to daemonize. "
			"SysErr: " << strerror(errno) << std::endl);
		exit(1);
	}

	// close temporary FDs
	close(devNullFD);
	close(logFileFD);

	int daemonRes = daemon(0 /* nochdir */, 1 /* noclose */);
	if(daemonRes == -1)
	{
		ERRLOGGER(Log_NORMAL, "Failed to daemonize into background. "
			"SysErr: " << strerror(errno) << std::endl);
		exit(1);
	}

	// if we got here, we successfully daemonized into background

	LOGGER(Log_NORMAL, "Running in background. PID: " << getpid() << std::endl);
}
