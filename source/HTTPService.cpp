#include <cstdlib>
#include <fstream>
#include <libgen.h>
#include <pwd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "HTTPService.h"
#include "ProgException.h"
#include "toolkits/S3Tk.h"
#include "toolkits/SystemTk.h"
#include "workers/RemoteWorker.h"

namespace Web = SimpleWeb;

#define SERVICE_LOG_DIR			"/tmp"
#define SERVICE_LOG_FILEPREFIX	EXE_NAME
#define SERVICE_LOG_FILEMODE	(S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)


/**
 * Start HTTP service. Does not return on success.
 *
 * @throw ProgException on error, e.g. binding to configured port failed.
 */
void HTTPService::startServer()
{
	checkPortAvailable();

	if(!progArgs.getRunServiceInForeground() )
		daemonize(); // daemonize process in background

	S3Tk::initS3Global(progArgs); // inits threads and thus after service daemonize

	// prepare http server and its URLs

	HttpServer server;

	defineServerResources(server);

	server.config.port = progArgs.getServicePort(); // desired port (std::promise confirms this)
	std::promise<unsigned short> actualServerPort; // set by HttpServer after startup (0 for error)


	// run http server in separate thread to enable success message through std::promise

	std::thread serverThread([&server, &actualServerPort]()
	{
		try
		{
			// start server with callback for port
			server.start([&actualServerPort](unsigned short port)
			{
				actualServerPort.set_value(port);
			});
		}
		catch(std::exception& e)
		{
			std::cerr << "ERROR: HTTP service failed. Reason: " << e.what() << std::endl;

			try
			{
				// set port std::promise to error to notify parent thread
				/* note: this might fail if webserver encountered an error after binding to port,
				 * but that's ok, because it also means parent thread got beyond waiting for port */
				actualServerPort.set_value(0); // 0 for error
			}
			catch(std::exception& e)
			{
				std::cout << "ERROR: Failed to set service port promise to error. "
					"Reason: " << e.what() << std::endl;
			}
		}
	});

	// wait for server to start up in separate thread

	std::future<unsigned short> portFuture = actualServerPort.get_future();
	unsigned short serverPortFutureValue;

	try
	{
		// after line below, server is either listening or failed
		serverPortFutureValue = portFuture.get();
	}
	catch(std::exception& e)
	{
		serverThread.join();
		throw ProgException("Failed to get HTTP service port. "
			"Desired port: " + std::to_string(progArgs.getServicePort() ) );
	}

	// port value 0 means error
	if(!serverPortFutureValue)
	{
		serverThread.join();
		throw ProgException("HTTP service failed to listen on desired port. "
			"Port: " + std::to_string(progArgs.getServicePort() ) );
	}

	std::cout << "Service now listening. Port: " << serverPortFutureValue << std::endl;

	serverThread.join();

	std::cout << "Service stopped listening. Port: " << serverPortFutureValue << std::endl;
}

/**
 * Define the resources (URL handlers) of the HTTP server.
 */
void HTTPService::defineServerResources(HttpServer& server)
{
	// get info for testing (not used by master)
	server.resource[HTTPSERVERPATH_INFO]["GET"] =
		[](std::shared_ptr<HttpServer::Response> response,
			std::shared_ptr<HttpServer::Request> request)
	{
		Logger(Log_VERBOSE) << "HTTP: " << request->path << "?" <<
			request->query_string << std::endl;

		std::stringstream stream;
		stream << "<h1>Request from " << request->remote_endpoint().address().to_string()  <<
			":" << request->remote_endpoint().port() << "</h1>";

		stream << request->method << " " << request->path << " HTTP/" << request->http_version;

		stream << "<h2>Query Fields</h2>";
		auto query_fields = request->parse_query_string();
		for(auto &field : query_fields)
		stream << field.first << ": " << field.second << "<br>";

		stream << "<h2>Header Fields</h2>";
		for(auto &field : request->header)
		stream << field.first << ": " << field.second << "<br>";

		response->write(stream);
	};

	// get http service protocol version
	server.resource[HTTPSERVERPATH_PROTOCOLVERSION]["GET"] =
		[&, this](std::shared_ptr<HttpServer::Response> response,
			std::shared_ptr<HttpServer::Request> request)
	{
		Logger(Log_VERBOSE) << "HTTP: " << request->path << "?" <<
			request->query_string << std::endl;

		response->write(HTTP_PROTOCOLVERSION);
	};

	// get live statistics
	server.resource[HTTPSERVERPATH_STATUS]["GET"] =
		[&, this](std::shared_ptr<HttpServer::Response> response,
			std::shared_ptr<HttpServer::Request> request)
	{
		Logger(Log_VERBOSE) << "HTTP: " << request->path << "?" <<
			request->query_string << std::endl;

		std::stringstream stream;

		bpt::ptree tree;

		statistics.updateLiveCPUUtil();

		statistics.getLiveStatsAsPropertyTree(tree);

		bpt::write_json(stream, tree, true);

		response->write(stream);
	};

	// get final results after completion of benchmark phase
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

			std::cout << std::endl;

			response->write(stream);
		}
		catch(const std::exception& e)
		{
			std::stringstream stream;
			stream << e.what();
			response->write(Web::StatusCode::client_error_bad_request, stream);
			std::cerr << stream.str() << std::endl;
		}
	};

	// receive input files for following prepare phase, such as custom tree mode files
	server.resource[HTTPSERVERPATH_PREPAREFILE]["POST"] =
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

			// get and prepare filename

			iter = query_fields.find(XFER_PREP_FILENAME);
			if(iter == query_fields.end() )
				throw ProgException("Missing parameter: " XFER_PREP_FILENAME);

			char* filenameDup = strdup(iter->second.c_str() );
			if(!filenameDup)
				throw ProgException("Failed to alloc mem for filename dup: " + iter->second);

			// note: basename() ensures that there is no "../" or subdirs in the given filename
			std::string filename = basename(filenameDup);
			unsigned short servicePort = progArgs.getServicePort();
			std::string path = SERVICE_UPLOAD_BASEPATH(servicePort) + "/" + filename;

			free(filenameDup);

			// print file transfer phase to log

			std::time_t currentTime = std::time(NULL);

			std::cout << "Receiving tree file from master... "
				"(ISO DATE: " << std::put_time(std::localtime(&currentTime), "%FT%T%z") << ")" <<
				std::endl;

			// prepare our upload directory

			int mkRes = mkdir(SERVICE_UPLOAD_BASEPATH(servicePort).c_str(), 0777);
			if( (mkRes == -1) && (errno != EEXIST) )
				throw ProgException("Failed to create service tmp dir: " +
					SERVICE_UPLOAD_BASEPATH(servicePort) );

			// open upload file under given name in upload dir

			std::ofstream fileOutStream(path.c_str(),
				std::ofstream::out | std::ofstream::trunc);
			if(!fileOutStream)
				throw ProgException("Opening upload file failed: " + path);

			// save uploaded file contents

			fileOutStream << request->content.rdbuf();

			// close file and final error check

			fileOutStream.close();

			if(!fileOutStream)
				throw ProgException("Saving upload file failed: " + path);

			// prepare response. (just signal successful completion of upload via empty reply.)

			response->write("");
		}
		catch(const std::exception& e)
		{
			std::stringstream stream;

			stream << "File preparation phase error: " << e.what() << std::endl;

			response->write(Web::StatusCode::client_error_bad_request, stream);

			//std::cerr << "File preparation phase error: " << stream.str() << std::endl;
		}
	};

	// prepare new benchmark phase: transfer ProgArgs, prepare workers, reply when all ready
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

			// print prep phase to log

			std::time_t currentTime = std::time(NULL);

			std::cout << "Preparing new benchmark phase... "
				"(ISO DATE: " << std::put_time(std::localtime(&currentTime), "%FT%T%z") << ")" <<
				std::endl;

			// read config values as json

			bpt::ptree recvTree;
			bpt::read_json(request->content, recvTree);

			// prepare environment for new benchmarks

			/* (we update progArgs and workers have pointers to progArgs (e.g. pathFDs), so kill any
				running workers first) */
			workerManager.interruptAndNotifyWorkers();
			workerManager.joinAllThreads();
			workerManager.cleanupThreads();

			progArgs.resetBenchPath();

			LoggerBase::clearErrHistory();

			progArgs.setFromPropertyTreeForService(recvTree);

			workerManager.prepareThreads();

			// print user-defined label

			if(!progArgs.getBenchLabel().empty() )
				std::cout << "LABEL: " << progArgs.getBenchLabel() << std::endl;

			std::cout << std::endl; // blank line after iso date & label

			// prepare response

			std::stringstream replyStream;
			bpt::ptree replyTree;

			progArgs.getBenchPathInfoTree(replyTree);
			replyTree.put(XFER_PREP_ERRORHISTORY, LoggerBase::getErrHistory() );

			bpt::write_json(replyStream, replyTree, true);

			response->write(replyStream);
		}
		catch(const std::exception& e)
		{
			/* we will not get another interrupt or stop from master when prep fails, because the
				corresponding RemoteWorker on master terminates on prep error reply, so we need to
				clean up and release everything here before replying. */

			workerManager.interruptAndNotifyWorkers();
			workerManager.joinAllThreads();

			progArgs.resetBenchPath();

			std::stringstream stream;

			stream << "Preparation phase error: " << e.what() << std::endl <<
				LoggerBase::getErrHistory();

			response->write(Web::StatusCode::client_error_bad_request, stream);

			//std::cerr << "Preparation phase error: " << stream.str() << std::endl;
		}
	};

	// start benchmark phase after successful preparation
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

		statistics.updateLiveCPUUtil();

		workerManager.startNextPhase(benchPhase, benchID.empty() ? NULL : &benchID);

		response->write(LoggerBase::getErrHistory() );
	};

	// interrupt any running benchmark and reset everything, reply when all done
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

		workerManager.interruptAndNotifyWorkers();

		workerManager.joinAllThreads();

		progArgs.resetBenchPath();

		response->write(LoggerBase::getErrHistory() );

		if(quitAfterInterrupt)
		{
			response->send(); // (otherwise send() would only happen after server.stop() below)

			LOGGER(Log_NORMAL, "Shutting down as requested by client. "
				"Client: " << request->remote_endpoint().address().to_string() << std::endl);

			// this will make the blocking server.start() call exit
			server.stop();
		}
	};

	// handle server/protocol errors like disconnect
	server.on_error = [](std::shared_ptr<HttpServer::Request> request, const Web::error_code& ec)
	{
		/* connection timeouts will also call this handler with Web::error_code set to
		   SimpleWeb::errc::operation_canceled */

		if( (ec.category() == boost::asio::error::misc_category) &&
			(ec.value() == boost::asio::error::eof) )
		{
			LOGGER(Log_VERBOSE, "HTTP client disconnect. " <<
				"Client: " << request->remote_endpoint().address().to_string() << ":" <<
				request->remote_endpoint().port() << std::endl);
			return;
		}

		LOGGER(Log_NORMAL, "HTTP server error. Message: " << ec.message() << "; "
			"Client: " << request->remote_endpoint().address().to_string() << ":" <<
			request->remote_endpoint().port() << "; "
			"ErrorCode: " << ec.value() << "; "
			"ErrorCategory: " << ec.category().name() << std::endl);
	};

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
	std::string logfile;

	if(getenv("TMP") != NULL)
		logfile = getenv("TMP"); // default for linux
	else
	if(getenv("TEMP") != NULL)
		logfile = getenv("TEMP"); // default for windows
	else
		logfile = SERVICE_LOG_DIR;

	logfile += std::string("/") + SERVICE_LOG_FILEPREFIX + "_" +
		SystemTk::getUsername() + "_" +
		"p" + std::to_string(progArgs.getServicePort() ) + "." // port
		"log";

	int logFileFD = open(logfile.c_str(), O_CREAT | O_WRONLY | O_APPEND, SERVICE_LOG_FILEMODE);
	if(logFileFD == -1)
	{
		std::cerr << "ERROR: Failed to open logfile. Path: " << logfile << "; "
			"SysErr: " << strerror(errno) << std::endl;
		exit(1);
	}

	// try to get exclusive lock on logfile to make sure no other instance is using it
	int lockRes = flock(logFileFD, LOCK_EX | LOCK_NB);
	if(lockRes == -1)
	{
		if(errno == EWOULDBLOCK)
			std::cerr << "ERROR: Unable to get exclusive lock on logfile. Probably another "
				"instance is running and using the same service port. "
				"Path: " << logfile << "; "
				"Port: " << progArgs.getServicePort() << std::endl;
		else
			std::cerr << "ERROR: Failed to get exclusive lock on logfile. "
				"Path: " << logfile << "; "
				"Port: " << progArgs.getServicePort() << "; "
				"SysErr: " << strerror(errno) << std::endl;

		exit(1);
	}

	std::cout << "Daemonizing into background... Logfile: " << logfile << std::endl;

	// file locked, so trunc to 0 to delete messages from old instances
	int truncRes = ftruncate(logFileFD, 0);
	if(truncRes == -1)
	{
		std::cerr << "ERROR: Failed to truncate logfile. Path: " << logfile << "; "
			"SysErr: " << strerror(errno) << std::endl;
		exit(1);
	}

	int devNullFD = open("/dev/null", O_RDONLY);
	if(devNullFD == -1)
	{
		std::cerr << "ERROR: Failed to open /dev/null to daemonize. "
			"SysErr: " << strerror(errno) << std::endl;
		exit(1);
	}

	// close stdin and replace by /dev/null
	int stdinDup = dup2(devNullFD, STDIN_FILENO);
	if(stdinDup == -1)
	{
		std::cerr << "ERROR: Failed to replace stdin to daemonize. "
			"SysErr: " << strerror(errno) << std::endl;
		exit(1);
	}

	// close stdout and replace by logfile
	int stdoutDup = dup2(logFileFD, STDOUT_FILENO);
	if(stdoutDup == -1)
	{
		std::cerr << "ERROR: Failed to replace stdout to daemonize. "
			"SysErr: " << strerror(errno) << std::endl;
		exit(1);
	}

	// close stderr and replace by logfile
	int stderrDup = dup2(logFileFD, STDERR_FILENO);
	if(stderrDup == -1)
	{
		std::cerr << "ERROR: Failed to replace stderr to daemonize. "
			"SysErr: " << strerror(errno) << std::endl;
		exit(1);
	}

	// close temporary FDs
	close(devNullFD);
	close(logFileFD);

	int daemonRes = daemon(0 /* nochdir */, 1 /* noclose */);
	if(daemonRes == -1)
	{
		std::cerr << "ERROR: Failed to daemonize into background. "
			"SysErr: " << strerror(errno) << std::endl;
		exit(1);
	}

	// if we got here, we successfully daemonized into background

	LOGGER(Log_NORMAL, "Running in background. PID: " << getpid() << std::endl);

	const StringVec& benchPathsServiceOverrideVec = progArgs.getBenchPathsServiceOverride();
	if(!benchPathsServiceOverrideVec.empty() )
	{
		std::string logMsg = "NOTE: Benchmark paths given. These paths will be used instead of any "
			"path list provided by master. Paths: ";

		for(std::string path : benchPathsServiceOverrideVec)
			logMsg += "\"" + path + "\" ";

		LOGGER(Log_NORMAL, logMsg << std::endl);
	}

	std::string gpuIDsServiceOverride = progArgs.getGPUIDsServiceOverride();
	if(!gpuIDsServiceOverride.empty() )
		LOGGER(Log_NORMAL, "NOTE: GPU IDs given. These GPU IDs will be used instead of any "
			"GPU ID list provided by master. GPU IDs: " << gpuIDsServiceOverride << std::endl);
}

/**
 * Check if desired TCP port is available, so that an error message can be printed before
 * daemonizing.
 *
 * @throw ProgException if port not available or other error occured.
 */
void HTTPService::checkPortAvailable()
{
	unsigned short port = progArgs.getServicePort();
	struct sockaddr_in sockAddr;
	int listenBacklogSize = 1;

	int sockFD = socket(AF_INET, SOCK_STREAM, 0);
	if(sockFD == -1)
		throw ProgException(std::string("Unable to create socket to check port availability. ") +
			"SysErr: " + strerror(errno) );

	/* note: reuse option is important because http server sock will be in TIME_WAIT state for
		quite a while after stopping the service via --quit */

	int enableAddrReuse = 1;

	int setOptRes = setsockopt(
		sockFD, SOL_SOCKET, SO_REUSEADDR, &enableAddrReuse, sizeof(enableAddrReuse) );
	if(setOptRes == -1)
	{
		int errnoCopy = errno; // close() below could change errno

		close(sockFD);

		throw ProgException(std::string("Unable to enable socket address reuse. ") +
			"SysErr: " + strerror(errnoCopy) );
	}

	sockAddr.sin_family = AF_INET;
	sockAddr.sin_addr.s_addr = INADDR_ANY;
	sockAddr.sin_port = htons(port);

	int bindRes = bind(sockFD, (struct sockaddr*) &sockAddr, sizeof(sockAddr) );
	if(bindRes == -1)
	{
		int errnoCopy = errno; // close() below could change errno

		close(sockFD);

		throw ProgException(std::string("Unable to bind to desired port. ") +
			"Port: " + std::to_string(port) + "; "
			"SysErr: " + strerror(errnoCopy) );
	}

	int listenRes = listen(sockFD, listenBacklogSize);
	if(listenRes == -1)
	{
		int errnoCopy = errno; // close() below could change errno

		close(sockFD);

		throw ProgException(std::string("Unable to listen on desired port. ") +
			"Port: " + std::to_string(port) + "; "
			"SysErr: " + strerror(errnoCopy) );
	}

	close(sockFD);
}

