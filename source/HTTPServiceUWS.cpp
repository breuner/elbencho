#ifdef ALTHTTPSVC_SUPPORT

#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <libgen.h>
#include <pwd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "HTTPServiceUWS.h"
#include "ProgException.h"
#include "toolkits/S3Tk.h"
#include "toolkits/SystemTk.h"
#include "toolkits/TranslatorTk.h"
#include "workers/RemoteWorker.h"



/**
 * Start HTTP service. Does not return on success.
 *
 * @throw ProgException on error, e.g. binding to configured port failed.
 */
void HTTPServiceUWS::startServer()
{
	checkPortAvailable();

	if(!progArgs.getRunServiceInForeground() )
		daemonize(); // daemonize process in background

	S3Tk::initS3Global(progArgs); // inits threads and thus after service daemonize

	// prepare http server and its URLs

    uWS::App uWSApp = uWS::App();

    defineServerResources(uWSApp);

    int listenPort = progArgs.getServicePort();
	uWSApp.listen(listenPort,
		[&](auto *listenSocket)
		{
			if(listenSocket)
			{
				globalListenSocket = listenSocket;
				std::cout << "Elbencho alternative service now listening. Port: " << listenPort << std::endl;
			}
			else
				std::cout << "Failed to start listening. Port: " << listenPort << std::endl;
		});


	uWSApp.run();

    std::cout << "Service stopped listening. Port: " << listenPort << std::endl;
}

/**
 * Define the resources (URL handlers) of the HTTP server.
 */
void HTTPServiceUWS::defineServerResources(uWS::App& uWSApp)
{
	/* NOTE: uWS::HttpRequest dies when the handler returns (important for async onAbort and onData,
		which run after handler return in case of POST method). uWS::HttpResponse lives until either
	 	 onAbort was called or a reply was sent via res->end() */

	// get info for testing (used by humans in browser, not used by master instance)
	uWSApp.get(HTTPCLIENTPATH_INFO,
		[&](uWS::HttpResponse<false>* res, uWS::HttpRequest* req)
	{
		logReqAndError(res, std::string(req->getUrl() ), std::string(req->getQuery() ) );

		std::stringstream stream;

		stream << "<h1>Request from " << res->getRemoteAddressAsText() << " " <<
			"(Host: " << req->getHeader("host") << ")" << "</h1>";

		stream << "<h2>Header Fields</h2>";
		for(uWS::HttpRequest::HeaderIterator iter = req->begin(); iter != req->end(); ++iter)
		{
			stream << iter.ptr->key << ": " << iter.ptr->value << "<br/>" << std::endl;
		}

		res->end(stream.str() );
	});

	// get http service protocol version
	uWSApp.get(HTTPCLIENTPATH_PROTOCOLVERSION,
		[&](uWS::HttpResponse<false>* res, uWS::HttpRequest* req)
	{
		logReqAndError(res, std::string(req->getUrl() ), std::string(req->getQuery() ) );

		res->end(HTTP_PROTOCOLVERSION);
	});

	// get live statistics
	uWSApp.get(HTTPCLIENTPATH_STATUS,
		[&](uWS::HttpResponse<false>* res, uWS::HttpRequest* req)
	{
		logReqAndError(res, std::string(req->getUrl() ), std::string(req->getQuery() ) );

		// corking submits everything (incl. HTTP headers) in a single chunk for efficiency
		res->cork(
			[this, res]()
		{
			std::stringstream stream;

			bpt::ptree tree;

			statistics.updateLiveCPUUtil();

			statistics.getLiveStatsAsPropertyTree(tree);

			bpt::write_json(stream, tree, true);

			res->end(stream.str() );
		});
	});

	// get final results after completion of benchmark phase
	uWSApp.get(HTTPCLIENTPATH_BENCHRESULT,
		[&](uWS::HttpResponse<false>* res, uWS::HttpRequest* req)
	{
		logReqAndError(res, std::string(req->getUrl() ), std::string(req->getQuery() ) );

		try
		{
			std::stringstream stream;

			bpt::ptree tree;

			statistics.getBenchResultAsPropertyTree(tree);

			bpt::write_json(stream, tree, true);

			statistics.printPhaseResults(); // show results when running in foreground

			std::cout << std::endl;

			res->end(stream.str() );
		}
		catch(const std::exception& e)
		{
			std::stringstream stream;
			stream << e.what();
			res->writeStatus("400 Bad Request");
			res->end(stream.str() );
			std::cerr << stream.str() << std::endl;
		}
	});

	// receive input files for following prepare phase, such as custom tree mode files
	uWSApp.post(HTTPCLIENTPATH_PREPAREFILE,
		[&](uWS::HttpResponse<false>* res, uWS::HttpRequest* req)
	{
		logReqAndError(res, std::string(req->getUrl() ), std::string(req->getQuery() ) );

		try
		{
			// check protocol version for compatibility

			std::string_view masterProtoVer = req->getQuery(XFER_PREP_PROTCOLVERSION);
			if(masterProtoVer.empty() )
				throw ProgException("Missing parameter: " XFER_PREP_PROTCOLVERSION);

			if(masterProtoVer != HTTP_PROTOCOLVERSION)
				throw ProgException(std::string("Protocol version mismatch. ") +
					"Service version: " HTTP_PROTOCOLVERSION "; "
					"Received master version: " + std::string(masterProtoVer) );

			// get and prepare filename

			std::string_view clientFilenameVal = req->getQuery(XFER_PREP_FILENAME);
			if(clientFilenameVal.empty() )
				throw ProgException("Missing parameter: " XFER_PREP_FILENAME);

			std::string clientFilenameStr(clientFilenameVal );

			// (basename() wants a modifyable dup)
			char* filenameDup = strdup(clientFilenameStr.c_str() );
			if(!filenameDup)
				throw ProgException("Failed to alloc mem for filename dup: " + clientFilenameStr);

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

			// check if output file can be created and trunc if it exists

			std::ofstream fileOutStream(path.c_str(),
				std::ofstream::out | std::ofstream::trunc);

			if(!fileOutStream)
				throw ProgException("Opening upload file for truncation failed: " + path);

			fileOutStream.close();

			// incrementally receive posted data chunks
			// (onData gets called after handler return, when uWS::HttpRequest no longer exists)
			res->onData(
				[res, path](std::string_view chunk, bool isLastChunk)
			{
				LOGGER(Log_DEBUG, "HTTP: Receiving " <<
					(isLastChunk ? "final" : "non-final") << " " << "chunk." << " " <<
					"Length: " << chunk.length() << std::endl);

				try
				{
					// open upload file under given name in upload dir

					std::ofstream fileOutStream(path.c_str(),
						std::ofstream::out | std::ofstream::app);
					if(!fileOutStream)
						throw ProgException("Opening upload file failed: " + path);

					// save uploaded file contents

					fileOutStream << std::string(chunk);

					// close file and final error check

					fileOutStream.close();

					if(!isLastChunk)
						return;

					if(!fileOutStream)
						throw ProgException("Saving upload file failed: " + path);

					res->end("");
				}
				catch(const std::exception& e)
				{
					std::stringstream stream;

					stream << "File preparation phase error: " << e.what() << std::endl;

					std::cerr << stream.str();

					res->writeStatus("400 Bad Request");
					res->end(stream.str() );
				}
			}); // end of onData handler


			// note: intentionally no more code here. the end is handled in onData/onAbort.

		}
		catch(const std::exception& e)
		{
			std::stringstream stream;

			stream << "File preparation phase error: " << e.what() << std::endl;

			std::cerr << stream.str();

			res->writeStatus("400 Bad Request");
			res->end(stream.str() );
		}
	});

	// prepare new benchmark phase: transfer ProgArgs, prepare workers, reply when all ready
	uWSApp.post(HTTPCLIENTPATH_PREPAREPHASE,
		[&](uWS::HttpResponse<false>* res, uWS::HttpRequest* req)
	{
		logReqAndError(res, std::string(req->getUrl() ), std::string(req->getQuery() ) );

		try
		{
			// check protocol version for compatibility

			std::string_view masterProtoVer = req->getQuery(XFER_PREP_PROTCOLVERSION);
			if(masterProtoVer.empty() )
				throw ProgException("Missing parameter: " XFER_PREP_PROTCOLVERSION);

			if(masterProtoVer != HTTP_PROTOCOLVERSION)
				throw ProgException(std::string("Protocol version mismatch. ") +
					"Service version: " HTTP_PROTOCOLVERSION "; "
					"Received master version: " + std::string(masterProtoVer) );

			// print prep phase to log

			std::time_t currentTime = std::time(NULL);

			std::cout << "Preparing new benchmark phase... "
				"(ISO DATE: " << std::put_time(std::localtime(&currentTime), "%FT%T%z") << ")" <<
				std::endl;

			// incrementally receive posted data chunks
			// (onData gets called after handler return, when uWS::HttpRequest no longer exists)

			std::string recvChunkStr; // our "state" between multiple onData calls via std::move

			res->onData(
				[this, res, recvChunkStr = std::move(recvChunkStr) ]
					(std::string_view chunk, bool isLastChunk) mutable
			{
				LOGGER(Log_DEBUG, "HTTP: Receiving " <<
					(isLastChunk ? "final" : "non-final") << " " << "chunk." << " " <<
					"Length: " << chunk.length() << std::endl);

				recvChunkStr.append(chunk.data(), chunk.length() );

				if(!isLastChunk)
					return;

				try
				{
					// read config values as json

					std::stringstream recvJsonStream(recvChunkStr);

					bpt::ptree recvTree;
					bpt::read_json(recvJsonStream, recvTree);

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

					res->end(replyStream.str() );
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

					std::cerr << stream.str();

					res->writeStatus("400 Bad Request");
					res->end(stream.str() );
				}
			}); // end of onData handler

			// note: intentionally no more code here. the end is handled in onData/onAbort.

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

			std::cerr << stream.str();

			res->writeStatus("400 Bad Request");
			res->end(stream.str() );
		}
	});

	// start benchmark phase after successful preparation
	uWSApp.get(HTTPCLIENTPATH_STARTPHASE,
		[&](uWS::HttpResponse<false>* res, uWS::HttpRequest* req)
	{
		logReqAndError(res, std::string(req->getUrl() ), std::string(req->getQuery() ) );

		std::stringstream stream;

		BenchPhase benchPhase;
		std::string benchID;

		std::string_view benchPhaseCodeParam = req->getQuery(XFER_START_BENCHPHASECODE);
		if(!benchPhaseCodeParam.empty() )
			benchPhase = (BenchPhase)std::stoi(std::string(benchPhaseCodeParam) );
		else
		{ // bench phase is a required parameter
			stream << "Missing parameter: " XFER_START_BENCHPHASECODE;
			res->writeStatus("400 Bad Request");
			res->end(stream.str() );
			return;
		}

		std::string_view benchIDParam = req->getQuery(XFER_START_BENCHID);
		if(!benchIDParam.empty() )
			benchID = std::string(benchIDParam);

		statistics.updateLiveCPUUtil();

		workerManager.startNextPhase(benchPhase, benchID.empty() ? NULL : &benchID);

		res->end(LoggerBase::getErrHistory() );
	});

	// interrupt any running benchmark and reset everything, reply when all done
	uWSApp.get(HTTPCLIENTPATH_INTERRUPTPHASE,
		[&](uWS::HttpResponse<false>* res, uWS::HttpRequest* req)
	{
		logReqAndError(res, std::string(req->getUrl() ), std::string(req->getQuery() ) );

		bool quitAfterInterrupt = false;

		// check if XFER_INTERRUPT_QUIT is given

		std::string_view benchPhaseCodeParam = req->getQuery(XFER_INTERRUPT_QUIT);
		if(benchPhaseCodeParam.data() != nullptr)
			quitAfterInterrupt = true;

		workerManager.interruptAndNotifyWorkers();

		workerManager.joinAllThreads();

		progArgs.resetBenchPath();

		// cork to send everything at once before we quit
		res->cork( [this, res]() { res->end(LoggerBase::getErrHistory() ); } );

		if(quitAfterInterrupt)
		{
			LOGGER(Log_NORMAL, "Shutting down as requested by client. "
				"Client: " << res->getRemoteAddressAsText() << std::endl);

			// this will make the blocking server.start() call exit
			// uWS terminates when socket is closed
			us_listen_socket_close(0 /*ssl*/, globalListenSocket);
		}
	});
}

/**
 * Log incoming HTTP request and register error handler.
 *
 * We copy strings here because the onAborted handler might get called after request handler return,
 * in which case uWS::HttpRequest no longer exists.
 *
 * @urlStr the URL of the HTTP request.
 * @queryStr the parameters of the HTTP request.
 */
void HTTPServiceUWS::logReqAndError(uWS::HttpResponse<false>* res, std::string urlStr,
	std::string queryStr)
{
	Logger(Log_VERBOSE) << "HTTP: " << urlStr << "?" << queryStr << std::endl;

	// attach abort handler to be aware of disconnects during async tasks (e.g. onData)
	res->onAborted(
		[urlStr, queryStr]()
	{
		std::cerr << "Connection was closed before request completed: " <<
			"HTTP: " << urlStr << "?" << queryStr << std::endl;
	});
}

#endif // ALTHTTPSVC_SUPPORT
