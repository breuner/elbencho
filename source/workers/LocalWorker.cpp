// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <iterator>
#include <string>
#include <sys/mman.h>
#include <sys/socket.h>

#include "Common.h"
#include "LocalWorker.h"
#include "Logger.h"
#include "PathStore.h"
#include "toolkits/FileTk.h"
#include "toolkits/spdk/SpdkNvmeClient.h"
#include "toolkits/random/RandAlgoSelectorTk.h"
#include "toolkits/StringTk.h"
#include "toolkits/TranslatorTk.h"
#include "WorkerException.h"
#include "WorkersSharedData.h"
#include "workers/Worker.h"

#ifdef CUDA_SUPPORT
    #include <cuda_runtime.h>
#endif

#define MKDIR_MODE						0777
#define AIO_MAX_WAIT_SEC				5
#define AIO_MAX_EVENTS					4  // max number of events to retrieve in io_getevents()
#define NETBENCH_CONNECT_TIMEOUT_SEC	20 // max time for servers to wait and clients to retry
#define NETBENCH_RECEIVE_TIMEOUT_SEC	20 // max time to wait for incoming data on client & server
#define NETBENCH_SHORT_POLL_TIMEOUT_SEC	2  // time to check for interrupts in longer poll wait loops


SocketVec LocalWorker::serverSocketVec; // singleton netbench sockets vec for all local threads


LocalWorker::LocalWorker(WorkersSharedData* workersSharedData, size_t workerRank) :
	Worker(workersSharedData, workerRank), opsLog(workersSharedData->progArgs, workerRank),
    s3Mode(*this)
{
	nullifyPhaseFunctionPointers();

	fileHandles.fdVec.resize(1);
	fileHandles.cuFileHandleDataVec.resize(1);
}

LocalWorker::~LocalWorker()
{
	/* note: Most of the cleanup is done in cleanup() instead of here, because we need to release
	 	handles etc. before the destructor is called. This is because in service mode, this object
		needs to survive to provide statistics and might only get deleted when the next benchmark
		run starts, but we can't keep the handles until then. */
}


/**
 * Entry point for the thread.
 * Kick off the work that this worker has to do. Each phase is sychronized to wait for notification
 * by coordinator.
 *
 * Ensure that cleanup() is called when this method finishes.
 */
void LocalWorker::run()
{
	try
	{
		buuids::uuid currentBenchID = buuids::nil_uuid();

		// preparation phase
		preparePhase();

		// signal coordinator that our preparations phase is done
		phaseFinished = true; // before incNumWorkersDone(), as Coordinator can reset after done inc
		incNumWorkersDone();

		for( ; ; )
		{
			// wait for coordinator to set new bench ID to signal us that we are good to start
			waitForNextPhase(currentBenchID);

			currentBenchID = workersSharedData->currentBenchID;
			bool doInfiniteIOLoop = progArgs->getDoInfiniteIOLoop();

			do // for infinite I/O loop
			{
				initThreadPhaseVars();
				initPhaseFileHandleVecs();
				initPhaseRWOffsetGen();
				initPhaseFunctionPointers();

                // (note: order of bench phases is defined in Coordinator::runBenchmarks() )
				switch(workersSharedData->currentBenchPhase)
				{
					case BenchPhase_TERMINATE:
					{
						LOGGER(Log_DEBUG, "Terminating as requested. Rank: " << workerRank << "; "
							"(Offset: " << progArgs->getRankOffset() << ")" << std::endl);
						incNumWorkersDone();
						return;
					} break;

					case BenchPhase_CREATEDIRS:
					case BenchPhase_DELETEDIRS:
					case BenchPhase_STATDIRS:
					{
                        if(progArgs->getBenchPathType() != BenchPathType_DIR)
                            throw WorkerException("Directory creation and deletion are not "
                                "available in file and block device mode.");

                        if(progArgs->getBenchMode() == BenchMode_HDFS)
                            hdfsDirModeIterateDirs();
                        else
                        if(progArgs->getBenchMode() == BenchMode_S3)
                            s3Mode.iterateBuckets();
                        else
                            progArgs->getTreeFilePath().empty() ?
                                dirModeIterateDirs() : dirModeIterateCustomDirs();
					} break;

                    case BenchPhase_GET_S3_BUCKET_MD:
                    case BenchPhase_PUT_S3_BUCKET_MD:
                    case BenchPhase_DEL_S3_BUCKET_MD:
                    {
                        s3Mode.iterateBuckets();
                    } break;

					case BenchPhase_CREATEFILES:
					case BenchPhase_READFILES:
					{
                        if(progArgs->getBenchPathType() == BenchPathType_DIR)
                        {
                            if(progArgs->getBenchMode() == BenchMode_NETBENCH)
                                netbenchDoTransfer();
                            else
                            if(progArgs->getBenchMode() == BenchMode_HDFS)
                                hdfsDirModeIterateFiles();
                            else
                            if(progArgs->getBenchMode() == BenchMode_S3)
                                progArgs->getTreeFilePath().empty() ?
                                    s3Mode.iterateObjects() : s3Mode.iterateCustomObjects();
                            else
                                progArgs->getTreeFilePath().empty() ?
                                    dirModeIterateFiles() : dirModeIterateCustomFiles();
                        }
                        else
                        { // posix file/bdev mode & spdk mode
                            if(progArgs->getUseRandomOffsets() || progArgs->getUseStridedAccess() )
                                fileModeIterateFilesRand();
                            else
                                fileModeIterateFilesSeq();
                        }
					} break;

                    case BenchPhase_S3MPUCOMPLETE:
                    {
                        s3Mode.iterateAndCompleteMpuIDs();
                    } break;

                    case BenchPhase_GET_S3_OBJECT_MD:
                    case BenchPhase_PUT_S3_OBJECT_MD:
                    case BenchPhase_DEL_S3_OBJECT_MD:
                    {
                        progArgs->getTreeFilePath().empty() ?
                            s3Mode.iterateObjects() : s3Mode.iterateCustomObjects();
                    } break;

					case BenchPhase_STATFILES:
					{
                        if(progArgs->getBenchPathType() != BenchPathType_DIR)
                            throw WorkerException("File stat operation not available in file and "
                                "block device mode.");

                        if(progArgs->getBenchMode() == BenchMode_HDFS)
                            hdfsDirModeIterateFiles();
                        else
                        if(progArgs->getBenchMode() == BenchMode_S3)
                            progArgs->getTreeFilePath().empty() ?
                                s3Mode.iterateObjects() : s3Mode.iterateCustomObjects();
                        else
                            progArgs->getTreeFilePath().empty() ?
                                dirModeIterateFiles() : dirModeIterateCustomFiles();
					} break;

					case BenchPhase_PUTBUCKETACL:
					case BenchPhase_GETBUCKETACL:
					{
						s3Mode.iterateBuckets();
					} break;

					case BenchPhase_PUTOBJACL:
					case BenchPhase_GETOBJACL:
					{
						progArgs->getTreeFilePath().empty() ?
							s3Mode.iterateObjects() : s3Mode.iterateCustomObjects();
					} break;

					case BenchPhase_LISTOBJECTS:
					{
						s3Mode.listObjects();
					} break;

					case BenchPhase_LISTOBJPARALLEL:
					{
						if(!progArgs->getTreeFilePath().empty() )
							throw WorkerException("Parallel object listing is not available in "
								"custom tree mode.");

						s3Mode.listObjParallel();
					} break;

					case BenchPhase_MULTIDELOBJ:
					{
						s3Mode.listAndMultiDeleteObjects();
					} break;

					case BenchPhase_DELETEFILES:
					{
                        if(progArgs->getBenchPathType() == BenchPathType_DIR)
                        {
                            if(progArgs->getBenchMode() == BenchMode_HDFS)
                                hdfsDirModeIterateFiles();
                            else
                            if(progArgs->getBenchMode() == BenchMode_S3)
                                progArgs->getTreeFilePath().empty() ?
                                    s3Mode.iterateObjects() : s3Mode.iterateCustomObjects();
                            else
                                progArgs->getTreeFilePath().empty() ?
                                    dirModeIterateFiles() : dirModeIterateCustomFiles();
                        }
                        else
                            fileModeDeleteFiles();
					} break;

					case BenchPhase_SYNC:
					{
						anyModeSync();
						doInfiniteIOLoop = false;
					} break;

					case BenchPhase_DROPCACHES:
					{
						anyModeDropCaches();
						doInfiniteIOLoop = false;
					} break;

					default:
					{ // should never happen
						throw WorkerException("Unknown/invalid next phase type: " +
							std::to_string(workersSharedData->currentBenchPhase) );
					} break;

				} // end of switch

				checkInterruptionRequest(); // for infinite loop workers with no work

			} while(doInfiniteIOLoop && workerGotPhaseWork); // end of infinite loop

			// let coordinator know that we are done
			finishPhase();

		} // end of for loop

	}
	catch(WorkerInterruptedException& e)
	{
		// whoever interrupted us will have a reason for it, so we don't print at normal level here
		ErrLogger(Log_DEBUG, progArgs->getRunAsService() ) << "Interrupted exception. " <<
			"WorkerRank: " << workerRank << std::endl;

		/* check if called twice. (happens on interrupted waitForNextPhase() while other workers
			haven't finished the previous phase, i.e. during the end game of a phase.) */
		if(!phaseFinished)
		{
			s3Mode.abortUnfinishedSharedUploads(); // abort unfinished S3 uploads
			finishPhase(); // let coordinator know that we are done
		}

		return;
	}
	catch(std::exception& e)
	{
		ErrLogger(Log_NORMAL, progArgs->getRunAsService() ) << e.what() << std::endl;

		s3Mode.abortUnfinishedSharedUploads(); // abort unfinished S3 uploads
	}

	incNumWorkersDoneWithError();
}

/**
 * Run all the preparations in the run() method that are needed before we can announce readiness
 * for an actual benchmark run.
 */
void LocalWorker::preparePhase()
{
    applyNumaAndCoreBinding();

    opsLog.openLogFile();

    initThreadFDVec();
    initThreadCuFileHandleDataVec();
    initThreadMmapVec();

    allocIOBuffer();
    allocGPUIOBuffer();

    prepareCustomTreePathStores();

    initAsyncIOSlots();
    initLibAio();
    s3Mode.init();
    initHDFS();
    initNetBench();
    initSpdk();
}

/**
 * Update finish time values, then signal coordinator that we're done.
 *
 * Nothing should run after this, because coordinator will assume that it can reset things after
 * the done counter has been increased inside this function.
 */
void LocalWorker::finishPhase()
{
	if(!workerGotPhaseWork)
		elapsedUSecVec.resize(0);
	else
	{
		std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

		std::chrono::microseconds elapsedDurationUSec =
			std::chrono::duration_cast<std::chrono::microseconds>
			(now - workersSharedData->phaseStartT);
		uint64_t finishElapsedUSec = elapsedDurationUSec.count();

		elapsedUSecVec.resize(1);
		elapsedUSecVec[0] = finishElapsedUSec;
	}

	phaseFinished = true; // before incNumWorkersDone() because Coordinator can reset after inc

	incNumWorkersDone();
}

void LocalWorker::initLibAio()
{
#ifdef LIBAIO_SUPPORT

    const size_t maxIODepth = progArgs->getIODepth();

    if(maxIODepth < 2)
        return; // no libaio needed

    libaioContext.ioContext = (io_context_t)0; // zeroing required by io_queue_init
    libaioContext.iocbVec.resize(maxIODepth);
    libaioContext.iocbPointerVec.resize(maxIODepth);
    libaioContext.ioStartTimeVec.resize(maxIODepth);

    int initRes = io_queue_init(maxIODepth, &libaioContext.ioContext);
    IF_UNLIKELY(initRes)
        throw WorkerException(std::string("Initializing async IO (io_queue_init) failed. ") +
            "SysErr: " + strerror(-initRes) ); // (io_queue_init returns negative errno)

#endif // LIBAIO_SUPPORT
}

void LocalWorker::uninitLibAio()
{
#ifdef LIBAIO_SUPPORT

    const size_t maxIODepth = progArgs->getIODepth();

    if(maxIODepth < 2)
        return; // no libaio needed

    if(libaioContext.ioContext != NULL)
        io_queue_release(libaioContext.ioContext);

#endif // LIBAIO_SUPPORT
}

void LocalWorker::initHDFS()
{
#ifdef HDFS_SUPPORT

	if(!progArgs->getUseHDFS() )
		return; // nothing to do

	hdfsFSHandle = hdfsConnect("default", 0);
	if(!hdfsFSHandle)
		throw WorkerException("Unable to connect to HDFS using \"default\" config.");

#endif // HDFS_SUPPORT
}

void LocalWorker::uninitHDFS()
{
#ifdef HDFS_SUPPORT

	if(!progArgs->getUseHDFS() )
		return; // nothing to do

	if(!hdfsFSHandle)
		return; // nothing to do

	hdfsDisconnect(hdfsFSHandle);

	hdfsFSHandle = NULL;

#endif // HDFS_SUPPORT
}

/**
 * Wrapper to initialize network benchmark mode servers and clients.
 */
void LocalWorker::initNetBench()
{
	if(!progArgs->getRunAsService() || (progArgs->getBenchMode() != BenchMode_NETBENCH) )
		return; // nothing to do

	const size_t hostIndex =
		progArgs->getRankOffset() / progArgs->getNumThreads(); // zero-based
	const bool hostIsServer = (hostIndex < progArgs->getNumNetBenchServers() );

	if(hostIsServer)
		initNetBenchServer();
	else
		initNetBenchClient();
}

/**
 * Initialize network benchmark mode server. First thread of a server will open service port +1000
 * to accept conns from clients. First worker of a server accepts connections for all worker
 * threads.
 */
void LocalWorker::initNetBenchServer()
{
	if(!progArgs->getRunAsService() || (progArgs->getBenchMode() != BenchMode_NETBENCH) )
		return; // nothing to do

	const unsigned listenTimeoutMS = NETBENCH_CONNECT_TIMEOUT_SEC * 1000;
	const unsigned short listenPort = progArgs->getServicePort() + 1000;

	const size_t localWorkerRank = workerRank - progArgs->getRankOffset();

	const size_t hostIndex =
		progArgs->getRankOffset() / progArgs->getNumThreads(); // zero-based
	const bool hostIsServer = (hostIndex < progArgs->getNumNetBenchServers() );
	const size_t numClients = (progArgs->getNumDataSetThreads() / progArgs->getNumThreads() ) -
		progArgs->getNumNetBenchServers();

	if(hostIsServer && (localWorkerRank != 0) )
		return; // nothing to do. (only first worker of server accepts all client conns)

	const unsigned numConnsTotal = (numClients * progArgs->getNumThreads() );

	unsigned numConnsToWaitFor = (numConnsTotal / progArgs->getNumNetBenchServers() );

	if( (numConnsTotal % progArgs->getNumNetBenchServers() ) &&
		(hostIndex < (numConnsTotal % progArgs->getNumNetBenchServers() ) ) )
		numConnsToWaitFor += 1;

	// prepare listen socket

	BasicSocket listenSock(AF_INET, SOCK_STREAM);

	// (note: buf size has to be set before listen() to be applied to new accepted sockets)
	if(progArgs->getSockRecvBufSize() )
	{
		LOGGER(Log_VERBOSE, "Changing sock recv buf size. Old value: " <<
			listenSock.getSoRcvBuf() << std::endl);

		listenSock.setSoRcvBuf(progArgs->getSockRecvBufSize() );
	}

	if(progArgs->getSockSendBufSize() )
	{
		LOGGER(Log_VERBOSE, "Changing sock send buf size. Old value: " <<
			listenSock.getSoSndBuf() << std::endl);

		listenSock.setSoSndBuf(progArgs->getSockSendBufSize() );
	}

	listenSock.setSoReuseAddr(true);
	listenSock.bind(listenPort);

	listenSock.listen();

	// wait for incoming connections

	LOGGER(Log_VERBOSE, "netbench init: "
		"Listening for client connections at: " << listenPort << std::endl);

	while(serverSocketVec.size() < numConnsToWaitFor)
	{
		bool haveIncomingConn = listenSock.waitForIncomingData(listenTimeoutMS);
		if(!haveIncomingConn) // timeout
			throw WorkerException("Timed out waiting for client connections. "
				"Received connections: " + std::to_string(serverSocketVec.size() ) + "; "
				"Expected connections: " + std::to_string(numConnsToWaitFor) + "; "
				"Timeout in ms: " + std::to_string(listenTimeoutMS) );

		struct sockaddr_in peer;
		socklen_t peerStructSize = sizeof(peer);
		BasicSocket* newSocket = (BasicSocket*)listenSock.accept(
			(struct sockaddr*)&peer, &peerStructSize);

		LOGGER(Log_VERBOSE, "netbench init: "
			"Accepted new connection from: " << newSocket->getPeername() << "; "
			"Connected: " << (serverSocketVec.size() + 1) << " / " << numConnsToWaitFor <<
			std::endl);

		newSocket->setSoKeepAlive(true);
		newSocket->setTcpNoDelay(true);

		serverSocketVec.push_back(newSocket);
	}
}

/**
 * Initialize network benchmark client mode. Each client worker opens one connection. Client threads
 * connect round-robin to the different servers, so that a single client with multiple threads can
 * talk to multiple servers.
 */
void LocalWorker::initNetBenchClient()
{
	const size_t localWorkerRank = workerRank - progArgs->getRankOffset();

	// prepare socket

	clientSocket = new BasicSocket(AF_INET, SOCK_STREAM);

	clientSocket->setSoKeepAlive(true);
	clientSocket->setTcpNoDelay(true);

	if(progArgs->getSockRecvBufSize() )
	{
		LOGGER(Log_VERBOSE, "Changing sock recv buf size. Old value: " <<
			clientSocket->getSoRcvBuf() << std::endl);

		clientSocket->setSoRcvBuf(progArgs->getSockRecvBufSize() );
	}

	if(progArgs->getSockSendBufSize() )
	{
		LOGGER(Log_VERBOSE, "Changing sock send buf size. Old value: " <<
			clientSocket->getSoSndBuf() << std::endl);

		clientSocket->setSoSndBuf(progArgs->getSockSendBufSize() );
	}

	if(!progArgs->getNetDevsVec().empty() )
	{ // round-robin binding of sockets to user-given network devices
		unsigned netDevIdx = (localWorkerRank % progArgs->getNetDevsVec().size() );
		clientSocket->setSoBindToDevice(progArgs->getNetDevsVec()[netDevIdx].c_str() );
	}

	/* note: clientWorkerRank and serverOffset have to remain in sync with the number of
		 connections that each server expects. */

	/* clients connect round-robin to different servers
		(next client continues where the previous client stopped) */

	const NetBenchServerAddrVec& serversVec = progArgs->getNetBenchServers();
	const size_t clientWorkerRank = workerRank -
		(serversVec.size() * progArgs->getNumThreads() );
	const size_t serversOffset = clientWorkerRank % serversVec.size();
	const NetBenchServerAddr& serverAddr = serversVec[serversOffset];

	// start time for connection retry timeout
	std::chrono::steady_clock::time_point connectStartT = std::chrono::steady_clock::now();

	// connection attempt(s)
	for( ; ; )
	{
		try
		{
			LOGGER(Log_DEBUG, "netbench init: "
				"Connecting to: " << serverAddr.host.c_str() << ":" <<
				serverAddr.port << "; " <<
				"WorkerRank: " << workerRank << std::endl);

			clientSocket->connect(serverAddr.host.c_str(), serverAddr.port);

			LOGGER(Log_VERBOSE, "netbench init: "
				"Established connection to: " << clientSocket->getPeername() << "; " <<
				"WorkerRank: " << workerRank << std::endl);

			break; // connection successful if no exception thrown
		}
		catch(SocketConnectException& e)
		{ // server might just not be ready yet, thus retry if not timed out yet

			// calculate elapsed time to check retry timeout
			std::chrono::steady_clock::time_point connectEndT =
				std::chrono::steady_clock::now();
			std::chrono::seconds connectElapsedSecs =
				std::chrono::duration_cast<std::chrono::seconds>
				(connectEndT - connectStartT);

			if(connectElapsedSecs.count() > NETBENCH_CONNECT_TIMEOUT_SEC)
				throw;

			// wait 500ms before the next retry to avoid flooding
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
		}

	} // end of connection retry loop
}

/**
 * Cleanup client sockets from netbench mode. Server sockets are shared across all threads and
 * thus get cleaned up in uninitNetBenchAfterPhaseDone().
 */
void LocalWorker::uninitNetBench()
{
	if(!progArgs->getRunAsService() || (progArgs->getBenchMode() != BenchMode_NETBENCH) )
		return; // nothing to do

	const size_t localWorkerRank = workerRank - progArgs->getRankOffset();
	const size_t hostIndex =
		progArgs->getRankOffset() / progArgs->getNumThreads(); // zero-based
	const bool hostIsServer = (hostIndex < progArgs->getNumNetBenchServers() );

	if(hostIsServer && (localWorkerRank != 0) )
		return; // nothing to do. (only first worker of server closes all client conns)

	if(hostIsServer)
	{
		/* this cleanup happens in uninitNetBenchAfterPhaseDone() because some workers might still
		 	 be running at this point. */
	}

	if(!hostIsServer && (clientSocket != NULL) )
	{ // this is a client => just one socket to disconnect
		try { clientSocket->shutdown(); } catch(...) {}
		delete(clientSocket);
		clientSocket = NULL;
	}
}

/**
 * Late cleanup for shared server sockets. Client sockets get cleaned up in uninitNetBench() because
 * they are not shared across all workers.
 */
void LocalWorker::uninitNetBenchAfterPhaseDone()
{
	if(!progArgs->getRunAsService() || (progArgs->getBenchMode() != BenchMode_NETBENCH) )
		return; // nothing to do

	const size_t localWorkerRank = workerRank - progArgs->getRankOffset();
	const size_t hostIndex =
		progArgs->getRankOffset() / progArgs->getNumThreads(); // zero-based
	const bool hostIsServer = (hostIndex < progArgs->getNumNetBenchServers() );

	if(hostIsServer && (localWorkerRank != 0) )
		return; // nothing to do. (only first worker of server closes all client conns)

	if(hostIsServer)
	{
		for(Socket* sock : serverSocketVec)
		{
			try { sock->shutdown(); } catch(...) {}
			delete(sock); // destructor contains close()
		}

		serverSocketVec.clear();
		serverSocketVec.shrink_to_fit();
	}

}

/**
 * Parse spdk config and establish connections.
 *
 * @throw WorkerException on error
 */
void LocalWorker::initSpdk()
{
    if(progArgs->getBenchMode() != BenchMode_SPDK)
        return; // nothing to do

#ifndef SPDK_SUPPORT

    throw WorkerException("SPDK initialization requested but this executable was built without "
        "SPDK support");

#else // SPDK_SUPPORT

    auto& spdkClient = spdkContext.spdkClient;

    bool initRes = spdkClient.init(progArgs->getSpdkConfJSON() );
    if(!initRes)
        throw WorkerException("SPDK initialization in worker thread failed.");

    const IntVec& benchPathNsIDs = progArgs->getBenchPathSpdkNsIds();
    UInt32Vec nsIDs = spdkClient.getNamespaceIds();

    if(nsIDs.empty() || benchPathNsIDs.empty() )
        throw WorkerException("SPDK init phase did not discover any usable namespaces.");

    LOGGER(Log_DEBUG, "Discovered namespaces via spdk. "
        "Count: " << nsIDs.size() << "; "
        "workerRank: " << workerRank << std::endl);

    // check namespace discovery for consistency with ProgArgs discovery...

    spdkContext.sectorSize = spdkClient.getNamespaceSectorSize(benchPathNsIDs[0] );

    for(size_t i=0; i < benchPathNsIDs.size(); i++)
    {
        int nsID = progArgs->getBenchPathSpdkNsIds()[i];
        std::string nsName = spdkClient.getNamespaceName(nsID);
        std::string nsUuid = spdkClient.getNamespaceUuid(nsID);
        std::string nsNguid = spdkClient.getNamespaceNguid(nsID);
        std::string benchPathName = progArgs->getBenchPaths()[i];
        uint32_t sectorSize = spdkClient.getNamespaceSectorSize(nsID);

        if(nsName.empty() )
            throw WorkerException("Namespace discovery inconsistency. Namespace ID not found: " +
                std::to_string(nsID) );

        // benchPathName may be the numeric ID, the human-friendly name, or the namespace's
        // UUID/NGUID (see ProgArgs::prepareSpdk() ), so accept any of those as a match here
        if(!StringTk::hasOnlyDigits(benchPathName) && (nsName != benchPathName) &&
            (nsUuid != benchPathName) && (nsNguid != benchPathName) )
            throw WorkerException("Namespace discovery inconsistency. Namespace ID changed. "
                "NamespaceID: " + std::to_string(nsID) + "; "
                "NamespaceName: '" + nsName + "'; "
                "ExpectedName: '" + benchPathName + "'");

        if(sectorSize != spdkContext.sectorSize)
            throw WorkerException("Worker found different namespace sector sizes. "
                "This is not supported.");
    }

    /* preallocate one IoContext per ioDepth slot for spdkAioBlockSized(), mirroring
        initLibAio()'s iocbVec and as the sync IoContext for spdkReadWrapper()/spdkWriteWrapper() */
    const size_t maxIODepth = std::max<size_t>(progArgs->getIODepth(), 1);

    spdkContext.ioContextVec.resize(maxIODepth);
    spdkContext.ioStartTimeVec.resize(maxIODepth);
    spdkContext.completedVec.reserve(maxIODepth);

    for(size_t i=0; i < maxIODepth; i++)
    {
        spdkContext.ioContextVec[i] = std::make_unique<SpdkNvmeClient::IoContext>();
        spdkContext.ioContextVec[i]->userData = reinterpret_cast<void*>(i);
    }

#endif // SPDK_SUPPORT
}

/**
 * Disconnect spdk connections and free resources.
 */
void LocalWorker::uninitSpdk()
{
#ifdef SPDK_SUPPORT

    if(progArgs->getBenchMode() != BenchMode_SPDK)
        return; // nothing to do

    spdkContext.spdkClient.disconnect();

#endif // SPDK_SUPPORT
}

/**
 * If progArgs::useNoFDSharing is set, initialize threadFDVec with separate open files in file/bdev
 * mode. Otherwise do nothing.
 *
 * Note: It's assumed that progArgs did all the basic checks (e.g. to find out if all paths refer
 * to the same type) and that progArgs also takes care of truncate options.
 *
 * @throw WorkerException on error, e.g. if open failed.
 */
void LocalWorker::initThreadFDVec()
{
	if(!progArgs->getUseNoFDSharing() )
		return; // shared FDs, so nothing to do

	if(progArgs->getBenchPathType() == BenchPathType_DIR)
		return; // nothing to do in dir mode

	const StringVec& benchPathsVec = progArgs->getBenchPaths();

	fileHandles.threadFDVec.reserve(benchPathsVec.size() );

	// check if each given path exists as dir and add it to pathFDsVec
	// note: keep flags in sync with ProgArgs::prepareBenchPathFDsVec
	for(std::string path : benchPathsVec)
	{
		int fd;
		int openFlags = 0;

		if(progArgs->getRunCreateFilesPhase() || progArgs->getRunDeleteFilesPhase() )
			openFlags |= O_RDWR;
		else
			openFlags |= O_RDONLY;

#if !defined(__APPLE__)
        if(progArgs->getUseDirectIO() )
            openFlags |= O_DIRECT;
#endif // !apple

		if(progArgs->getRunCreateFilesPhase() )
			openFlags |= O_CREAT;

	    OPLOG_PRE_OP("open", path.c_str(), 0, 0);

        fd = open(path.c_str(), openFlags, MKFILE_MODE);

	    OPLOG_POST_OP("open", path.c_str(), 0, 0, fd == -1);

		if(fd == -1)
			throw WorkerException("Unable to open benchmark path: " + path + "; "
				"SysErr: " + strerror(errno) );

		fileHandles.threadFDVec.push_back(fd);
	}

}

/**
 * If progArgs::useNoFDSharing is set, close FDs of threadFDVec. Otherwise do nothing.
 */
void LocalWorker::uninitThreadFDVec()
{
	for(int fd : fileHandles.threadFDVec)
	{
        OPLOG_PRE_OP("close", std::to_string(fd), 0, 0);

        int closeRes = close(fd);

        OPLOG_POST_OP("close", std::to_string(fd), 0, 0, closeRes == -1);

		if(closeRes == -1)
			ERRLOGGER(Log_NORMAL, "Error on file close. "
				"FD: " << fd << "; "
				"SysErr: " << strerror(errno) << std::endl);
	}

	fileHandles.threadFDVec.resize(0);
}

/**
 * Similar to threadFDVec, here we init thread-local cuFile handles for progArgs::useNoFDSharing.
 *
 * @throw WorkerException if cuFile handle registration fails.
 */
void LocalWorker::initThreadCuFileHandleDataVec()
{
	for(int fd : fileHandles.threadFDVec)
	{
		// add new element to vec and reference it
		fileHandles.threadCuFileHandleDataVec.resize(
			fileHandles.threadCuFileHandleDataVec.size() + 1);
		CuFileHandleData& cuFileHandleData =
			fileHandles.threadCuFileHandleDataVec[fileHandles.threadCuFileHandleDataVec.size() - 1];

		if(!progArgs->getUseCuFile() )
			continue; // no registration to be done if cuFile API is not used

		// note: cleanup won't be a prob if reg not done, as CuFileHandleData can handle that case

		cuFileHandleData.registerHandle<WorkerException>(fd);
	}
}

/**
 * Deregsiter threadCuFileHandleVec entries.
 */
void LocalWorker::uninitThreadCuFileHandleDataVec()
{
	for(CuFileHandleData& cuFileHandleData : fileHandles.threadCuFileHandleDataVec)
		cuFileHandleData.deregisterHandle();

	fileHandles.threadCuFileHandleDataVec.resize(0); // reset vec before reuse in service mode
}

/**
 * Init fileHandles.mmapVec from progArgs->getBenchPathFDs() if this is a random phase with files
 * as bench paths. Otherwise the init will happen later in initPhaseFileHandleVecs().
 */
void LocalWorker::initThreadMmapVec()
{
	if(!progArgs->getUseMmap() )
		return; // nothing to do

	if(progArgs->getBenchPathType() == BenchPathType_DIR)
		return; // init will happen in initPhaseFileHandleVecs()

	if(!progArgs->getUseRandomOffsets() && !progArgs->getUseStridedAccess() )
		return; // init will happen in initPhaseFileHandleVecs()

	fileHandles.mmapVec = progArgs->getMmapVec();
}

/**
 * Unmap fileHandles.mmapVec entries after a run of possibly multiple phases.
 */
void LocalWorker::uninitThreadMmapVec()
{
	if( (progArgs->getBenchPathType() != BenchPathType_DIR) &&
		(progArgs->getUseRandomOffsets() || progArgs->getUseStridedAccess() ) )
	{ // random file/bdev mode: we copied mappings from progArgs, so don't unmap here
		fileHandles.mmapVec.resize(0);

		return;
	}

	// if we got here then we're not in random file/bdev mode, so we did our own mapping in worker

	for(char*& mmapPtr : fileHandles.mmapVec)
	{
		if(mmapPtr == MAP_FAILED)
			continue;

        int unmapRes = munmap(mmapPtr, progArgs->getFileEndOffset() );

		if(unmapRes == -1)
			ERRLOGGER(Log_NORMAL, "File memory unmap failed. "
				"SysErr: " << strerror(errno) << std::endl);

		mmapPtr = (char*)MAP_FAILED;
	}

	fileHandles.mmapVec.resize(0);
}


/**
 * Init thread-local phase values.
 */
void LocalWorker::initThreadPhaseVars()
{
    const BenchPhase globalBenchPhase = workersSharedData->currentBenchPhase;
    const size_t localWorkerRank = workerRank - progArgs->getRankOffset();
    const bool isRWMixedReader = ( (globalBenchPhase == BenchPhase_CREATEFILES) &&
        (localWorkerRank < progArgs->getNumRWMixReadThreads() ) );

    /* in rwmix threads mode, we set benchPhase to BenchPhase_READFILES, because the corresponding
       workers will only be reading */
    if(isRWMixedReader)
        benchPhase = BenchPhase_READFILES;
    else
        benchPhase = globalBenchPhase;
}

/**
 * Init fileHandle vectors for current phase.
 */
void LocalWorker::initPhaseFileHandleVecs()
{
    const BenchPathType benchPathType = progArgs->getBenchPathType();
    const BenchMode benchMode = progArgs->getBenchMode();

    const bool journalEnabled = progArgs->isJournalingEnabled();

    fileHandles.errorFDVecIdx = -1; // clear ("-1" means "not set")

    // reset/clear all vecs
    fileHandles.fdVec.resize(0);
    fileHandles.fdVecPtr = NULL;
    fileHandles.cuFileHandleDataVec.resize(0);
    fileHandles.cuFileHandleDataPtrVec.resize(0);
    fileHandles.journalPtrVec.resize(0);


    if(benchPathType == BenchPathType_DIR)
    {
        /* in dir mode, there is only one currently active file per worker.
            files will be dynamically opened in the dir mode file iter method and their fd will be
            stored in fileHandles.fdVec[0] */

        fileHandles.fdVec.resize(1);
        fileHandles.fdVec[0] = -1; // clear/reset

        fileHandles.fdVecPtr = &fileHandles.fdVec;

        // fileHandles.cuFileHandleDataVec[0] will be used for current file

        fileHandles.cuFileHandleDataVec.resize(1);

        fileHandles.cuFileHandleDataPtrVec.push_back(&fileHandles.cuFileHandleDataVec[0] );

        fileHandles.mmapVec.resize(1, (char*)MAP_FAILED);
    }
    else
    if(!progArgs->getUseRandomOffsets() && !progArgs->getUseStridedAccess() )
    {
        /* in sequential file/bdev mode, there is only one currently active file per worker.
            original file FDs will be taken from progArgs or fileHandles.threadFDVec, but FD will be
            copied to fileHandles.fdVec[0] for the single current file. */

        fileHandles.fdVec.resize(1);
        fileHandles.fdVec[0] = -1; // clear/reset, will be set dynamically to current file

        fileHandles.fdVecPtr = &fileHandles.fdVec;

        fileHandles.cuFileHandleDataPtrVec.resize(1); // set dynamically to current file

        if(journalEnabled)
            fileHandles.journalPtrVec.resize(1, nullptr); // set dynamically to current file

        fileHandles.mmapVec.resize(1, (char*)MAP_FAILED); /* fileModeIterateFilesSeq() will do the
            mmap() dynamically when this thread switches to a new file */
    }
    else
    {
        // in random file/bdev mode, rwBlockSized/aioBlockSized randomly select FDs from given set

        // init fileHandles.fdVecPtr
        if(benchMode == BenchMode_SPDK)
            fileHandles.fdVecPtr = &progArgs->getBenchPathSpdkNsIds();
        else
        if(fileHandles.threadFDVec.empty() )
            fileHandles.fdVecPtr = &progArgs->getBenchPathFDs();
        else
            fileHandles.fdVecPtr = &fileHandles.threadFDVec;

        CuFileHandleDataVec& cuFileHandleDataVec = fileHandles.threadCuFileHandleDataVec.empty() ?
            progArgs->getCuFileHandleDataVec() : fileHandles.threadCuFileHandleDataVec;

        for(size_t i=0; i < cuFileHandleDataVec.size(); i++)
            fileHandles.cuFileHandleDataPtrVec.push_back(&(cuFileHandleDataVec[i]) );

        /* journals are indexed like progArgs' bench paths, which is also the order of all the
            possible fdVecPtr targets, so journalPtrVec can be indexed like fdVecPtr */
        if(journalEnabled)
            for(size_t i=0; i < fileHandles.fdVecPtr->size(); i++)
                fileHandles.journalPtrVec.push_back(progArgs->getJournalForTarget(i) );

        // note: nothing for fileHandles.mmapVec here; init was done in initThreadMmapVec
    }
}

/**
 * Prepare read/write file contents offset generator for dirModeIterateFiles and
 * fileModeIterateFiles.
 *
 * Note: rwOffsetGen will always be set to an object here (even if phase is not read or write) to
 * prevent extra NULL pointer checks in file/dir loops.
 */
void LocalWorker::initPhaseRWOffsetGen()
{
    const size_t blockSize = progArgs->getBlockSize();
    const BlockSizeMix& blockSizeMix = progArgs->getBlockSizeMix();
    const uint64_t fileSize = progArgs->getFileSize();
    const bool isWritePhase = (workersSharedData->currentBenchPhase == BenchPhase_CREATEFILES);

    /* in dir mode randAmount is file size, for file/bdev it's the total amount for this thread
        across all given files/bdevs */
    const uint64_t randomAmount = (progArgs->getBenchPathType() == BenchPathType_DIR) ?
        fileSize : progArgs->getRandomAmount() / progArgs->getNumDataSetThreads();

    // init random algos
    randBlockVarAlgo = RandAlgoSelectorTk::stringToAlgo(progArgs->getBlockVarianceAlgo() );
    randBlockVarReseed = std::make_unique<RandAlgoXoshiro256ss>();

    /* init algo for random offsets within files (and, if a block size mix is given, for the
        weighted block size draw, reusing the same RNG instance) */
    randOffsetAlgo = RandAlgoSelectorTk::stringToAlgo(progArgs->getRandOffsetAlgo().empty() ?
        RANDALGO_BALANCED_SEQUENTIAL_STR : progArgs->getRandOffsetAlgo() );

    // note: in some cases these defs get overridden per-file later (e.g. for custom tree)

    if(progArgs->getDoReverseSeqOffsets() || s3Mode.getDoReverseSeqFallback() ) // seq backward
        rwOffsetGen = std::make_unique<OffsetGenReverseSeq>(
            fileSize, 0, blockSize);
    else
    if(!progArgs->getUseRandomOffsets() && !progArgs->getUseStridedAccess() ) // sequential forward
        rwOffsetGen = std::make_unique<OffsetGenSequential>(
            fileSize, 0, blockSizeMix, *randOffsetAlgo);
    else
    if(progArgs->getUseRandomUnaligned() ) // random unaligned
    {
        rwOffsetGen = std::make_unique<OffsetGenRandom>(randomAmount, *randOffsetAlgo,
            fileSize, 0, blockSizeMix);
    }
    else // random aligned
    {
        if(!progArgs->getRandOffsetAlgo().empty() || !isWritePhase ||
            progArgs->isBlockSizeMixDefined() )
            rwOffsetGen = std::make_unique<OffsetGenRandomAligned>(randomAmount, *randOffsetAlgo,
                fileSize, 0, blockSizeMix);
        else
        { // random aligned writes without explicit algo selection => use full coverage algo
            rwOffsetGen = std::make_unique<OffsetGenRandomAlignedFullCoverageV2>(
                randomAmount, fileSize, 0, blockSize);
        }
    } // end of random aligned
}

/**
 * Just set all phase-dependent function pointers to NULL.
 */
void LocalWorker::nullifyPhaseFunctionPointers()
{
	funcRWBlockSized = NULL;
	funcPositionalWrite = NULL;
	funcPositionalRead = NULL;
	funcAioRwPrepper = NULL;
	funcPreWriteCudaMemcpy = NULL;
	funcPostReadCudaMemcpy = NULL;
	funcPreWriteCudaMemcpy = NULL;
	funcPreWriteBlockModifier = NULL;
	funcPostReadBlockChecker = NULL;
	funcCuFileHandleReg = NULL;
	funcCuFileHandleDereg = NULL;
	funcRWRateLimiter = NULL;
}

/**
 * Prepare read/write function pointers for dirModeIterateFiles and fileModeIterateFiles.
 */
void LocalWorker::initPhaseFunctionPointers()
{
    const size_t ioDepth = progArgs->getIODepth();
    const bool useHDFS = (progArgs->getBenchMode() == BenchMode_HDFS);
    const bool useSPDK = (progArgs->getBenchMode() == BenchMode_SPDK);
    const bool useMmap = progArgs->getUseMmap();
    const bool useCuFileAPI = progArgs->getUseCuFile();
    const bool useGpuDirectIO = useCuFileAPI || s3Mode.isGpuDirect(); // data lands in gpu bufs
    const BenchPathType benchPathType = progArgs->getBenchPathType();
    const bool integrityCheckEnabled = (progArgs->getIntegrityCheckSalt() != 0);
    const bool areGPUsGiven = !progArgs->getGPUIDsVec().empty();
    const bool doDirectVerify = progArgs->getDoDirectVerify();
    const bool doReadInline = progArgs->getDoReadInline();
    const unsigned blockVariancePercent = progArgs->getBlockVariancePercent();
    const unsigned rwMixReadPercent = progArgs->getRWMixReadPercent();
    const uint64_t perThreadWriteRateLimitBps = progArgs->getLimitWriteBps();
    const uint64_t perThreadReadRateLimitBps = progArgs->getLimitReadBps();
    const size_t numRWMixReadThreads = progArgs->getNumRWMixReadThreads();
    const size_t numRWMixWriteThreads = progArgs->getNumThreads() - numRWMixReadThreads;
    const unsigned rwMixThreadsReadPercent = progArgs->getRWMixThreadsReadPercent();
    const size_t blockSize = progArgs->getBlockSize();
    const BenchPhase globalBenchPhase = workersSharedData->currentBenchPhase;

    nullifyPhaseFunctionPointers(); // set all function pointers to NULL


    // independent of whether current phase is read or write...
    // (these need to be set above the phase-dependent settings because those can override

    if(useHDFS)
        funcPositionalWrite = &LocalWorker::hdfsWriteWrapper;
    else
    if(useSPDK)
        funcPositionalWrite = &LocalWorker::spdkWriteWrapper;
    else
    if(useMmap)
        funcPositionalWrite = &LocalWorker::mmapWriteWrapper;
    else
    if(useCuFileAPI)
        funcPositionalWrite = &LocalWorker::cuFileWriteWrapper;
    else
        funcPositionalWrite = &LocalWorker::pwriteWrapper;

    if(useHDFS)
        funcPositionalRead = &LocalWorker::hdfsReadWrapper;
    else
    if(useSPDK)
        funcPositionalRead = &LocalWorker::spdkReadWrapper;
    else
    if(useMmap)
        funcPositionalRead = &LocalWorker::mmapReadWrapper;
    else
    if(useCuFileAPI)
        funcPositionalRead = &LocalWorker::cuFileReadWrapper;
    else
        funcPositionalRead = &LocalWorker::preadWrapper;

    // phase-dependent settings...

    if(benchPhase == BenchPhase_CREATEFILES)
    {
        if(ioDepth == 1)
            funcRWBlockSized = &LocalWorker::rwBlockSized;
#ifdef SPDK_SUPPORT
        else
        if(useSPDK)
            funcRWBlockSized = &LocalWorker::spdkAioBlockSized;
#endif // SPDK_SUPPORT
        else
            funcRWBlockSized = &LocalWorker::aioBlockSized;

        funcAioRwPrepper = (ioDepth == 1) ? NULL : &LocalWorker::aioWritePrepper;

        if(rwMixReadPercent && funcAioRwPrepper)
            funcAioRwPrepper = &LocalWorker::aioRWMixPrepper;

        funcPreWriteCudaMemcpy = (areGPUsGiven && !useGpuDirectIO) ?
            &LocalWorker::cudaMemcpyGPUToHost : &LocalWorker::noOpCudaMemcpy;
        funcPostReadCudaMemcpy = &LocalWorker::noOpCudaMemcpy;

        if(areGPUsGiven && integrityCheckEnabled)
            funcPreWriteCudaMemcpy = &LocalWorker::cudaMemcpyHostToGPU;

        if(integrityCheckEnabled)
            funcPreWriteBlockModifier = &LocalWorker::preWriteIntegrityCheckFillBuf;
        else
        if(blockVariancePercent && areGPUsGiven)
            funcPreWriteBlockModifier = &LocalWorker::preWriteBufRandRefillCuda;
        else
        if(blockVariancePercent)
            funcPreWriteBlockModifier = &LocalWorker::preWriteBufRandRefill;
        else
            funcPreWriteBlockModifier = &LocalWorker::noOpIntegrityCheck;

        funcPostReadBlockChecker = &LocalWorker::noOpIntegrityCheck;

        if(doDirectVerify || doReadInline)
        {
            if(useSPDK)
                funcPositionalWrite = &LocalWorker::spdkWriteAndReadWrapper;
            else
            if(!useCuFileAPI)
                funcPositionalWrite = &LocalWorker::pwriteAndReadWrapper;
            else
            {
                funcPositionalWrite = &LocalWorker::cuFileWriteAndReadWrapper;
                funcPostReadCudaMemcpy = &LocalWorker::cudaMemcpyGPUToHost;
            }

            if(doDirectVerify)
                funcPostReadBlockChecker = &LocalWorker::postReadIntegrityCheckVerifyBuf;
        }

        // rate limiter / balancer

        if(numRWMixReadThreads && rwMixThreadsReadPercent)
        { // rate balancer between reader and writer threads
            rateLimiterRWMixThreads.initStart(
                rwMixThreadsReadPercent, numRWMixReadThreads, numRWMixWriteThreads, blockSize);

            funcRWRateLimiter = &LocalWorker::preRWRateBalanceLimiterForWriters;
        }
        else
        if(perThreadWriteRateLimitBps)
        { // plain per-thread rate limiter
            funcRWRateLimiter = &LocalWorker::preRWRateLimiter;
            rateLimiter.initStart(perThreadWriteRateLimitBps);
        }
        else // no rate limit
            funcRWRateLimiter = &LocalWorker::noOpRateLimiter;
    }
    else // BenchPhase_READFILES (and others which don't use these function pointers)
    {
        // (note: this also applies to rwmixthreads readers in a write phase)

        if(ioDepth == 1)
            funcRWBlockSized = &LocalWorker::rwBlockSized;
#ifdef SPDK_SUPPORT
        else
        if(useSPDK)
            funcRWBlockSized = &LocalWorker::spdkAioBlockSized;
#endif // SPDK_SUPPORT
        else
            funcRWBlockSized = &LocalWorker::aioBlockSized;

        funcAioRwPrepper = (ioDepth == 1) ? NULL : &LocalWorker::aioReadPrepper;

        funcPreWriteCudaMemcpy = &LocalWorker::noOpCudaMemcpy;
        funcPostReadCudaMemcpy = (areGPUsGiven && !useGpuDirectIO) ?
            &LocalWorker::cudaMemcpyHostToGPU : &LocalWorker::noOpCudaMemcpy;

        if(useGpuDirectIO && integrityCheckEnabled)
            funcPostReadCudaMemcpy = &LocalWorker::cudaMemcpyGPUToHost;

        funcPreWriteBlockModifier = &LocalWorker::noOpIntegrityCheck;
        funcPostReadBlockChecker = integrityCheckEnabled ?
            &LocalWorker::postReadIntegrityCheckVerifyBuf : &LocalWorker::noOpIntegrityCheck;

        // rate limiter / balancer

        if(numRWMixReadThreads && rwMixThreadsReadPercent &&
            (globalBenchPhase == BenchPhase_CREATEFILES) )
        { // rate balancer between reader and writer threads
            rateLimiterRWMixThreads.initStart(
                rwMixThreadsReadPercent, numRWMixReadThreads, numRWMixWriteThreads, blockSize);

            funcRWRateLimiter = &LocalWorker::preRWRateBalanceLimiterForReaders;
        }
        else
        if(perThreadReadRateLimitBps)
        { // plain per-thread rate limiter
            funcRWRateLimiter = &LocalWorker::preRWRateLimiter;
            rateLimiter.initStart(perThreadReadRateLimitBps);
        }
        else // no rate limit
            funcRWRateLimiter = &LocalWorker::noOpRateLimiter;
    }

    // independent of whether current phase is read or write...

    if(useCuFileAPI)
    {
        funcCuFileHandleReg = (benchPathType == BenchPathType_DIR) ?
            &LocalWorker::dirModeCuFileHandleReg : &LocalWorker::noOpCuFileHandleReg;
        funcCuFileHandleDereg = (benchPathType == BenchPathType_DIR) ?
            &LocalWorker::dirModeCuFileHandleDereg : &LocalWorker::noOpCuFileHandleDereg;
    }
    else
    {
        funcCuFileHandleReg = &LocalWorker::noOpCuFileHandleReg;
        funcCuFileHandleDereg = &LocalWorker::noOpCuFileHandleDereg;
    }

    if(progArgs->isJournalingEnabled() )
    { /* overrides whatever the phase-dependent blocks above assigned ("--verify" is rejected
         together with journaling, and block variance is implicitly disabled for it, so neither
         can be what those blocks picked). this also applies in a read phase,
         because a rwmix read that gets converted into an initializing write still needs the fill
         function. both functions no-op for the direction they don't apply to. */
        funcPreWriteBlockModifier = &LocalWorker::preWriteJournalFillBuf;
        funcPostReadBlockChecker = &LocalWorker::postReadJournalVerifyBuf;

        /* the journal decides read vs write per I/O, so the async prepper must follow that
            decision instead of re-deriving it (as aioRWMixPrepper would) */
        if(ioDepth > 1)
            funcAioRwPrepper = &LocalWorker::aioJournalPrepper;
    }
}

/**
 * Allocate aligned I/O buffer and fill with random data.
 *
 * @throw WorkerException if allocation fails.
 */
void LocalWorker::allocIOBuffer()
{
    if(!progArgs->getBlockSize() )
        return; // nothing to do here

    if( (progArgs->getBenchMode() == BenchMode_S3) && !progArgs->getRunCreateFilesPhase() &&
        progArgs->getS3Args().getUseS3FastRead() )
        return; // nothing to do if read to /dev/null is set and no writes to be done

    // alloc number of IO buffers matching iodepth
    for(size_t i=0; i < progArgs->getIODepth(); i++)
    {
        char* ioBuf;

        if(progArgs->getBenchMode() == BenchMode_SPDK)
        {
            #ifdef SPDK_SUPPORT
                ioBuf = (char*)SpdkNvmeClient::allocDmaBuf(progArgs->getBlockSize() );

                if(!ioBuf)
                    throw WorkerException("DMA buffer allocation for SPDK I/O failed. "
                        "Buffer size: " + std::to_string(progArgs->getBlockSize() ) + "; "
                        "Consider raising \"mem_size_mb\" in the SPDK JSON config file." );
            #endif // SPDK_SUPPORT
        }
        else
        {
            // alloc I/O buffer appropriately aligned for O_DIRECT
            int allocAlignedRes = posix_memalign( (void**)&ioBuf, sysconf(_SC_PAGESIZE),
                progArgs->getBlockSize() );

            if(allocAlignedRes)
                throw WorkerException("Aligned memory allocation failed. "
                    "Buffer size: " + std::to_string(progArgs->getBlockSize() ) + "; "
                    "Page size: " + std::to_string(sysconf(_SC_PAGESIZE) ) + "; "
                    "SysErr: " + strerror(allocAlignedRes) ); // yes, not errno here
        }

        ioBufVec.push_back(ioBuf);

        // fill buffer with random data to ensure it's really alloc'ed (and not "sparse")
        RandAlgoXoshiro256ss randGen;
        randGen.fillBuf(ioBuf, progArgs->getBlockSize() );
    }

    LOGGER(Log_DEBUG, "Allocated IO buffers for ioBufVec. "
        "Rank: " << workerRank << "; "
        "Number of buffers: " << ioBufVec.size() << std::endl);

    if(progArgs->isJournalingEnabled() )
    { /* per-thread scratch buffer for postReadJournalVerifyBuf(); sized to the largest possible
         single I/O so it can hold any journal block's tiled content for comparison */
        journalVerifyBuf = (char*)malloc(progArgs->getBlockSizeMix().getMaxSize() );

        if(!journalVerifyBuf)
            throw WorkerException("Allocation of journal verification buffer failed. "
                "Size: " + std::to_string(progArgs->getBlockSizeMix().getMaxSize() ) );

        /* one journal I/O context per in-flight I/O, allocated once and reused, so the journaled
            hot path never allocates. (constructed at final size rather than resize()d, because
            JournalRangeGuard deletes its copy ctor and therefore has no move ctor either, so a
            resize() of the enclosing vector would not compile.) */
        journalIOSlots = std::vector<JournalBlockIO>(
            std::max<size_t>(progArgs->getIODepth(), 1) ); // (same lower bound as
                                                           // initAsyncIOSlots(), see there)
    }
}

/**
 * Allocate GPU I/O buffer and fill with random data.
 *
 * @throw WorkerException if allocation fails.
 */
void LocalWorker::allocGPUIOBuffer()
{
	if(!progArgs->getBlockSize() )
		return; // nothing to do here

	if(progArgs->getGPUIDsVec().empty() )
	{ // gpu bufs won't be accessed, but gpuIOBufVec elems might be passed e.g. to noOpCudaMemcpy
		gpuIOBufVec.resize(progArgs->getIODepth(), NULL);
		return;
	}

#ifndef CUDA_SUPPORT

	throw WorkerException("GPU given, but this executable was built without CUDA support.");

#else // CUDA_SUPPORT

	size_t gpuIndex = workerRank % progArgs->getGPUIDsVec().size();

	gpuID = progArgs->getGPUIDsVec()[gpuIndex];

	LOGGER(Log_DEBUG, "Initializing GPU buffer. "
		"Rank: " << workerRank << "; "
		"GPU ID: " << gpuID << std::endl);

	// set the GPU that this worker thread will use
	cudaError_t setDevRes = cudaSetDevice(gpuID);

	if(setDevRes != cudaSuccess)
		throw WorkerException("Setting CUDA device failed. "
			"GPU ID: " + std::to_string(gpuID) + "; "
			"CUDA Error: " + cudaGetErrorString(setDevRes) );

	curandStatus_t createGenRes = curandCreateGenerator(&gpuRandGen, CURAND_RNG_PSEUDO_DEFAULT);
	if(createGenRes != CURAND_STATUS_SUCCESS)
		throw WorkerException("Initialization of GPU/CUDA random number generator failed. "
			"curand error code: " + std::to_string(createGenRes) );

	curandStatus_t seedRes = curandSetPseudoRandomGeneratorSeed(gpuRandGen,
		( (uint64_t)std::random_device()() << 32) | (uint32_t)std::random_device()() );
	if(seedRes != CURAND_STATUS_SUCCESS)
		throw WorkerException("Seeding GPU/CUDA random number generator failed. "
			"curand error code: " + std::to_string(seedRes) );

	// alloc number of GPU IO buffers matching iodepth
	for(size_t i=0; i < progArgs->getIODepth(); i++)
	{
		void* gpuIOBuf;

		cudaError_t allocRes = cudaMalloc(&gpuIOBuf, progArgs->getBlockSize() );

		if(allocRes != cudaSuccess)
			throw WorkerException("GPU memory allocation failed. "
				"Buffer size: " + std::to_string(progArgs->getBlockSize() ) + "; "
				"GPU ID: " + std::to_string(gpuID) + "; "
				"CUDA Error: " + cudaGetErrorString(allocRes) );

		gpuIOBufVec.push_back( (char*)gpuIOBuf);

		if(progArgs->getUseCuHostBufReg() )
		{
			cudaError_t regRes = cudaHostRegister(ioBufVec[i], progArgs->getBlockSize(),
				cudaHostRegisterDefault);

			if(regRes != cudaSuccess)
				throw WorkerException("Registration of host buffer via cudaHostRegister failed. "
					"Buffer size: " + std::to_string(progArgs->getBlockSize() ) + "; "
					"CUDA Error: " + cudaGetErrorString(regRes) );
		}

		cudaError_t copyRes = cudaMemcpy(gpuIOBuf, ioBufVec[i], progArgs->getBlockSize(),
			cudaMemcpyHostToDevice);

		if(copyRes != cudaSuccess)
			throw WorkerException("Initialization of GPU buffer via cudaMemcpy failed. "
				"Buffer size: " + std::to_string(progArgs->getBlockSize() ) + "; "
				"GPU ID: " + std::to_string(gpuID) + "; "
				"CUDA Error: " + cudaGetErrorString(copyRes) );
	}

    LOGGER(Log_DEBUG, "Allocated GPU buffers for gpuIOBufVec. "
        "Rank: " << workerRank << "; "
        "Number of buffers: " << gpuIOBufVec.size() << std::endl);

#endif // CUDA_SUPPORT

#ifndef CUFILE_SUPPORT

	if(progArgs->getUseCuFile() )
		throw WorkerException("cuFile API requested, but this executable was built without cuFile "
			"support.");

#else // CUFILE_SUPPORT

	// register GPU buffer for DMA (in s3 mode this is up to the transport)
	for(char* gpuIOBuf : gpuIOBufVec)
	{
		if(!progArgs->getUseCuFile() || !gpuIOBuf || !progArgs->getUseGPUBufReg() ||
            (progArgs->getBenchMode() == BenchMode_S3) )
			continue;

		CUfileError_t registerRes = cuFileBufRegister(gpuIOBuf, progArgs->getBlockSize(), 0);

		if(registerRes.err != CU_FILE_SUCCESS)
			throw WorkerException(std::string(
				"GPU DMA buffer registration via cuFileBufRegister failed. ") +
				"GPU ID: " + std::to_string(gpuID) + "; "
				"cuFile Error: " + CUFILE_ERRSTR(registerRes.err) );
	}

#endif // CUFILE_SUPPORT
}

/**
 * Size the per-slot vectors of the async I/O engines, so that they don't have to be allocated in
 * the hot I/O path. (aioBlockSized() gets called once per file in dir mode, so a per-call
 * allocation would happen once per file.)
 *
 * This is called only once per worker from preparePhase(), which is valid because
 * progArgs::ioDepth is fixed for the whole run.
 *
 * Note: intentionally not inside any "#ifdef" and not skipped for "ioDepth < 2", because
 * aioBlockSized()/spdkAioBlockSized() would silently do nothing at all with empty vectors (no
 * slot to submit into => no pending I/O => completion loop never runs), while still returning
 * the full byte count as if all I/O had been done.
 */
void LocalWorker::initAsyncIOSlots()
{
    const size_t maxIODepth = std::max<size_t>(progArgs->getIODepth(), 1);

    asyncFreeSlots.resize(maxIODepth);
    asyncIsReadVec.resize(maxIODepth);
    asyncIsRWMixReadVec.resize(maxIODepth);
}

/**
 * Reset the per-call state of the async I/O engines. Called at the start of each
 * aioBlockSized()/spdkAioBlockSized() call.
 *
 * This is required, not just cosmetic: the engines also have error return paths and throwing
 * paths (e.g. io_submit failure or a failed journal verification), which leave slots taken and
 * possibly a stashed pending draw behind. A leftover pending draw would submit the offset of a
 * previous call (i.e. of a previous file) and thus also make the offset generator accounting
 * drift, so don't be tempted to drop this.
 */
void LocalWorker::resetAsyncIOSlots()
{
    // (drive the loop off the vector itself, so this can't write out of bounds)
    const size_t numSlots = asyncFreeSlots.size();

    for(size_t i = 0; i < numSlots; i++)
        asyncFreeSlots[i] = numSlots - 1 - i; // so that slot 0 gets used first

    journalPendingDraw.isValid = false;
}

/**
 * Prepare paths for custom tree mode for this worker.
 *
 * This also loads the s3 mpu sharing file for services, because this requires the custom tree but
 * has to be done before randomization of the objects.
 *
 * @throw WorkerException on error
 */
void LocalWorker::prepareCustomTreePathStores()
{
	if(progArgs->getTreeFilePath().empty() )
		return; // nothing to do here

	const bool throwOnSmallerThanBlockSize = !progArgs->getNoDirectIOCheck() &&
		progArgs->getUseDirectIO() && progArgs->getUseRandomOffsets();

	const size_t numDataSetThreads = progArgs->getNumDataSetThreads();

	progArgs->getCustomTreeDirs().getWorkerSublistNonShared(workerRank,
		numDataSetThreads, false, customTreeDirs);

	progArgs->getCustomTreeFilesNonShared().getWorkerSublistNonShared(workerRank,
		numDataSetThreads, throwOnSmallerThanBlockSize, customTreeFiles);

	if(!s3Mode.prepareCustomTreePathStores(throwOnSmallerThanBlockSize) )
	{ // this is the normal case: fair sharing across all services/workers based on blocksize

        if(progArgs->getUseCustomTreeRoundRobin() )
            progArgs->getCustomTreeFilesShared().getWorkerSublistSharedRoundRobin(workerRank,
                progArgs->getNumDataSetThreads(), throwOnSmallerThanBlockSize, customTreeFiles);
        else
            progArgs->getCustomTreeFilesShared().getWorkerSublistShared(workerRank,
                progArgs->getNumDataSetThreads(), throwOnSmallerThanBlockSize, customTreeFiles);
	}

	if(progArgs->getUseCustomTreeRandomize() )
		customTreeFiles.randomShuffle();
}

/**
 * Release all allocated objects, handles etc.
 *
 * This needs to be called when run() ends. The things in here would usually be done in the
 * LocalWorker destructor, but especially in service mode we need the LocalWorker object to still
 * exist (to query phase results) while all ressources need to be released, because the LocalWorker
 * object will only be deleted when and if the next benchmark starts.
 */
void LocalWorker::cleanup()
{
	// delete rwOffsetGen (unique ptr) to eliminate any references to progArgs data etc.
	rwOffsetGen.reset();

    uninitSpdk();
	uninitNetBench();
	uninitHDFS();
	s3Mode.uninit();
    uninitLibAio();

	// reset custom tree mode path store
	customTreeFiles.clear();

#ifdef CUFILE_SUPPORT
	// deregister GPU buffers for DMA (in s3 mode this is up to the transport)
	for(char* gpuIOBuf : gpuIOBufVec)
	{
		if(!progArgs->getUseCuFile() || !gpuIOBuf || !progArgs->getUseGPUBufReg() ||
            (progArgs->getBenchMode() == BenchMode_S3) )
			continue;

		CUfileError_t deregRes = cuFileBufDeregister(gpuIOBuf);

		if(deregRes.err != CU_FILE_SUCCESS)
			ERRLOGGER(Log_VERBOSE, "GPU DMA buffer deregistration via cuFileBufDeregister failed. "
				"GPU ID: " << gpuID << "; "
				"cuFile Error: " << CUFILE_ERRSTR(deregRes.err) << std::endl);
	}
#endif

#ifdef CUDA_SUPPORT
	// cuda-free gpu memory buffers
	for(char* gpuIOBuf : gpuIOBufVec)
	{
		if(gpuIOBuf)
			cudaFree(gpuIOBuf);
	}

	// cuda-unregister host buffers
	if(progArgs->getUseCuHostBufReg() && !progArgs->getGPUIDsVec().empty() )
	{
		for(char* ioBuf : ioBufVec)
		{
			if(!ioBuf)
				continue;

			cudaError_t unregRes = cudaHostUnregister(ioBuf);

			if(unregRes != cudaSuccess)
				ERRLOGGER(Log_VERBOSE,
					"CPU DMA buffer deregistration via cudaHostUnregister failed. "
					"GPU ID: " << gpuID << "; "
					"CUDA Error: " << cudaGetErrorString(unregRes) << std::endl);
		}
	}

	if(gpuRandGen)
	{
		curandDestroyGenerator(gpuRandGen);
		gpuRandGen = NULL;
	}
#endif

	// free host memory buffers
	for(char* ioBuf : ioBufVec)
    {
        if(progArgs->getBenchMode() == BenchMode_SPDK)
        {
            #ifdef SPDK_SUPPORT
                SpdkNvmeClient::freeDmaBuf(ioBuf);
            #endif // SPDK_SUPPORT
        }
        else
		    SAFE_FREE(ioBuf);
    }

	uninitThreadMmapVec();
	uninitThreadCuFileHandleDataVec();
	uninitThreadFDVec();

    SAFE_FREE(journalVerifyBuf);

	opsLog.closeLogFile();
}

/**
 * Late cleanup after all workers are done with the current phase. This gets called by the
 * WorkerManager for all threads, so it's non-parallel (in contrast to LocalWorker::cleanup() ) and
 * thus should only be used for cleanup that can't be done while some workers are still running,
 * e.g. cleanup of shared data structures for all workers.
 *
 * Note: This can be called more than once after the same phase, e.g. in service mode if user
 * sends phase interrupt request more than once.
 *
 * Note: This is called after each phase, so don't free anything here that might be used for
 * multiple phases, e.g. in standalone mode.
 */
void LocalWorker::cleanupAfterPhaseDone()
{
	uninitNetBenchAfterPhaseDone();
}

/**
 * Loop around pread/pwrite to use user-defined block size instead of full given count in one call.
 * Reads/writes the pre-allocated ioBuf. Uses rwOffsetGen for next offset and block size.
 *
 * If this->fileHandles contains multiple FDs then they will be treated as described in
 * calcFileIdxAndOffsetStriped().
 *
 * @return similar to pread/pwrite.
 */
int64_t LocalWorker::rwBlockSized()
{
	const BenchPhase globalBenchPhase = workersSharedData->currentBenchPhase;
	const unsigned rwMixReadPercent = progArgs->getRWMixReadPercent();
    const uint64_t fileSize = progArgs->getFileSize();
    const uint64_t fileOffsetBase = progArgs->getFileOffset();
    const bool isSingleFile = (fileHandles.fdVecPtr->size() == 1);
    const unsigned short fileLockType = progArgs->getFLockType();
    const bool journalEnabled = progArgs->isJournalingEnabled();

    JournalSlotsReleaser journalSlotsReleaser(*this); // for the throwing paths

	while(rwOffsetGen->getNumBytesLeftToSubmit() )
	{
        const uint64_t rwOffsetGenNext = rwOffsetGen->getNextOffset();
        const size_t currentBlockSize = rwOffsetGen->getNextBlockSizeToSubmit();
        uint64_t currentOffset;
        size_t fileHandleIdx;
        ssize_t rwRes;

        calcFileIdxAndOffsetStriped(rwOffsetGenNext, fileSize, fileOffsetBase, isSingleFile,
            fileHandleIdx, currentOffset);

        /* decide whether this I/O is a read or a write. (a read can be either a read phase read,
            a rwmix threads reader, or a rwmixpct read within a write phase.) */

        bool isRead;
        bool isRWMixRead;
        JournalBlockIO::Intent journalIntent;

		if(benchPhase == BenchPhase_READFILES)
		{ // this is a read, but could be a rwmix read thread
            isRead = true;
			isRWMixRead = (benchPhase != globalBenchPhase);

            /* a rwmix threads reader is part of a read/write mix, so it initializes
                uninitialized blocks instead of skipping them; a real read phase only ever reads.
                (see journalGetIntent() for why this matters.) */
            journalIntent = isRWMixRead ?
                JournalBlockIO::INTENT_READ_OR_INIT : JournalBlockIO::INTENT_READ_SKIP_UNINIT;
		}
		else // this is a write or rwmixpct read
		if(rwMixReadPercent &&
			( ( (workerRank + numIOPSSubmitted) % 100) < rwMixReadPercent) )
		{ // this is a rwmix read
            isRead = true;
			isRWMixRead = true;

            /* this thread writes as well, so an uninitialized block becomes an initializing
                write instead of being skipped */
            journalIntent = JournalBlockIO::INTENT_READ_OR_INIT;
		}
		else
		{ // this is a plain write
            isRead = false;
            isRWMixRead = false;
            journalIntent = JournalBlockIO::INTENT_WRITE;
		}

        if(journalEnabled)
        { /* let the journal decide & lock; this can turn a read into an initializing write or
             skip it altogether, and holds the journal lock until commit/release below. (this
             engine has only one I/O in flight, so it always gets to wait for the locks and can
             never be deferred.) */
            const JournalBlockIO::Action journalAction = journalBeginIO(0 /*slotIdx*/,
                fileHandleIdx, currentOffset, currentBlockSize, journalIntent);

            IF_UNLIKELY(journalAction == JournalBlockIO::ACTION_SKIP)
            { // nothing was ever written here, so accept as verified-ok without reading
                numIOPSSubmitted++;
                rwOffsetGen->addBytesSubmitted(currentBlockSize);
                checkInterruptionRequest();
                continue;
            }

            isRead = (journalAction == JournalBlockIO::ACTION_READ);
            isRWMixRead = isRWMixRead && isRead; // a converted write is not a rwmix read
        }

        ((*this).*funcRWRateLimiter)(currentBlockSize, isInterruptionRequested);

        std::chrono::steady_clock::time_point ioStartT = std::chrono::steady_clock::now();

        ((*this).*funcPreWriteBlockModifier)(ioBufVec[0], gpuIOBufVec[0], currentBlockSize,
            currentOffset);
        ((*this).*funcPreWriteCudaMemcpy)(ioBufVec[0], gpuIOBufVec[0], currentBlockSize);

        FileTk::flock<WorkerException>( (*fileHandles.fdVecPtr)[fileHandleIdx], fileLockType,
            currentOffset, currentBlockSize, !isRead /*isWrite*/, false /*isUnlock*/, NULL);

        rwRes = isRead ?
            ((*this).*funcPositionalRead)(
                fileHandleIdx, ioBufVec[0], currentBlockSize, currentOffset) :
            ((*this).*funcPositionalWrite)(
                fileHandleIdx, ioBufVec[0], currentBlockSize, currentOffset);

		IF_UNLIKELY(rwRes <= 0)
		{ // unexpected result
			ERRLOGGER(Log_NORMAL, "IO failed: " << "blockSize: " << currentBlockSize << "; " <<
				"currentOffset:" << currentOffset << "; " <<
				"leftToSubmit:" << rwOffsetGen->getNumBytesLeftToSubmit() << "; " <<
				"rank:" << workerRank << "; " <<
				"return code: " << rwRes << "; " <<
				"errno: " << errno << std::endl);

			fileHandles.errorFDVecIdx = fileHandleIdx;

	        FileTk::flock<WorkerException>( (*fileHandles.fdVecPtr)[fileHandleIdx], fileLockType,
	            currentOffset, currentBlockSize, true /*ignored*/, true /*isUnlock*/, NULL);

            if(journalEnabled)
                journalReleaseIO(0); // no commit, so a failed write stays "uninitialized"

            return (rwRes < 0) ?
                rwRes :
                (rwOffsetGen->getNumBytesTotal() - rwOffsetGen->getNumBytesLeftToSubmit() );
		}

        FileTk::flock<WorkerException>( (*fileHandles.fdVecPtr)[fileHandleIdx], fileLockType,
            currentOffset, currentBlockSize, true /*ignored*/, true /*isUnlock*/, NULL);

		((*this).*funcPostReadCudaMemcpy)(ioBufVec[0], gpuIOBufVec[0], currentBlockSize);
		((*this).*funcPostReadBlockChecker)(ioBufVec[0], gpuIOBufVec[0], currentBlockSize,
		    currentOffset);

        if(journalEnabled)
            journalCommitIO(0); // commits new generations for a write; releases the lock

		// calc io operation latency
		std::chrono::steady_clock::time_point ioEndT = std::chrono::steady_clock::now();
		std::chrono::microseconds ioElapsedMicroSec =
			std::chrono::duration_cast<std::chrono::microseconds>
			(ioEndT - ioStartT);

		// iops lat & num done
		if(isRWMixRead)
		{ // inc special rwmix read stats
			iopsLatHistoReadMix.addLatency(ioElapsedMicroSec.count() );
			atomicLiveOpsReadMix.numBytesDone += rwRes;
			atomicLiveOpsReadMix.numIOPSDone++;
		}
		else
		{
			iopsLatHisto.addLatency(ioElapsedMicroSec.count() );
			atomicLiveOps.numBytesDone += rwRes;
			atomicLiveOps.numIOPSDone++;
		}

		numIOPSSubmitted++;
		rwOffsetGen->addBytesSubmitted(rwRes);

		checkInterruptionRequest();
	}

	return rwOffsetGen->getNumBytesTotal();
}

/**
 * Loop around libaio read/write to use user-defined block size instead of full file size in one
 * call.
 * Reads/writes the pre-allocated ioBuf. Uses iodepth from progArgs.
 *
 * If this->fileHandles contains multiple FDs then they will be treated as a striped single range,
 * so sequential IOs would be done round-robin. Thus, this is not suitable if serial processing
 * of files is needed.
 *
 * @return similar to pread/pwrite.
 * @throw WorkerException on async IO framework errors.
 */
int64_t LocalWorker::aioBlockSized()
{
#ifndef LIBAIO_SUPPORT

	throw WorkerException("Async IO via libaio requested, but this executable was built without "
		"libaio support.");

#else // LIBAIO_SUPPORT

	const BenchPhase globalBenchPhase = workersSharedData->currentBenchPhase;
	const size_t fileHandlesVecSize = fileHandles.fdVecPtr->size();
    const uint64_t fileSize = progArgs->getFileSize();
    const uint64_t fileOffsetBase = progArgs->getFileOffset();
    const bool isSingleFile = (fileHandlesVecSize == 1);
    const unsigned short fileLockType = progArgs->getFLockType();
    const bool journalEnabled = progArgs->isJournalingEnabled();
    const unsigned rwMixReadPercent = progArgs->getRWMixReadPercent();

	size_t numPending = 0; // num requests submitted and pending for completion
	size_t numBytesDone = 0; // after successfully completed requests

	struct io_event ioEvents[AIO_MAX_EVENTS];
	struct timespec ioTimeout;

    JournalSlotsReleaser journalSlotsReleaser(*this); // for the throwing paths

    resetAsyncIOSlots(); // (members, so that the hot path doesn't allocate; see the method)

    /* stack of slots that currently have no I/O in flight. taking the next slot to submit into
        is O(1), so the completion path does not have to scan for free slots. */
    std::vector<size_t>& freeSlots = asyncFreeSlots;

    /* Draw the next offset & block size from rwOffsetGen and submit one request for it into the
       given slot. */
    /* (always_inline, because this is called from both submission sites in the hot I/O path and
        gcc otherwise emits it out-of-line, which costs a call plus closure indirection per I/O) */
    auto trySubmitSlot = [&](const size_t ioVecIdx) __attribute__( (always_inline) ) -> SubmitRes
    {
        uint64_t rwOffsetGenNext;
        size_t blockSize;

        if(!journalEnabled)
        {
            rwOffsetGenNext = rwOffsetGen->getNextOffset();
            blockSize = rwOffsetGen->getNextBlockSizeToSubmit();
        }
        else
        { // reuse a previously deferred draw, if any, so no offset gets lost or drawn twice
            JournalPendingDraw& pendingDraw = journalPendingDraw;

            if(!pendingDraw.isValid)
            {
                pendingDraw.rwOffsetGenNext = rwOffsetGen->getNextOffset();
                pendingDraw.blockSize = rwOffsetGen->getNextBlockSizeToSubmit();
                pendingDraw.isValid = true;
            }

            rwOffsetGenNext = pendingDraw.rwOffsetGenNext;
            blockSize = pendingDraw.blockSize;
        }

        uint64_t currentOffset;
        size_t fileHandlesIdx;

        calcFileIdxAndOffsetStriped(rwOffsetGenNext, fileSize, fileOffsetBase, isSingleFile,
            fileHandlesIdx, currentOffset);

        if(journalEnabled)
        { /* let the journal decide & lock before prepping the request, because it can turn a
             rwmix read into an initializing write or skip the I/O altogether. the acquired
             journal lock stays held until this slot's I/O completes. */
            const JournalBlockIO::Action journalAction = journalBeginIO(ioVecIdx, fileHandlesIdx,
                currentOffset, blockSize, journalGetIntent(rwMixReadPercent) );

            IF_UNLIKELY(journalAction == JournalBlockIO::ACTION_DEFER)
                return SubmitRes_DEFERRED; /* journal locks currently held for another in-flight
                    I/O of this thread; keep the draw and retry once that one completed */

            journalPendingDraw.isValid = false; // this draw gets used up now

            IF_UNLIKELY(journalAction == JournalBlockIO::ACTION_SKIP)
            { // nothing was ever written here, so accept as verified-ok without reading it
                numIOPSSubmitted++;
                rwOffsetGen->addBytesSubmitted(blockSize);

                return SubmitRes_SKIPPED;
            }
        }

        const int fd = (*fileHandles.fdVecPtr)[fileHandlesIdx];

        libaioContext.iocbPointerVec[ioVecIdx] = &libaioContext.iocbVec[ioVecIdx];

        ((*this).*funcAioRwPrepper)(&libaioContext.iocbVec[ioVecIdx], fd, ioBufVec[ioVecIdx],
            blockSize, currentOffset);
        libaioContext.iocbVec[ioVecIdx].data = (void*)ioVecIdx; /* the vec index of this request;
            ioctl.data is caller's private data returned after io_getevents as ioEvents[].data */

        libaioContext.ioStartTimeVec[ioVecIdx] = std::chrono::steady_clock::now();

        bool hadToWait = ((*this).*funcRWRateLimiter)(blockSize, isInterruptionRequested);
        IF_UNLIKELY(hadToWait) // invalidate start time of all pending due to rate limiter wait
            for(std::chrono::steady_clock::time_point& startT : libaioContext.ioStartTimeVec)
                startT = std::chrono::steady_clock::time_point::min();

        ((*this).*funcPreWriteBlockModifier)(ioBufVec[ioVecIdx], gpuIOBufVec[ioVecIdx], blockSize,
            currentOffset);
        ((*this).*funcPreWriteCudaMemcpy)(ioBufVec[ioVecIdx], gpuIOBufVec[ioVecIdx], blockSize);

        FileTk::flock<WorkerException>( (*fileHandles.fdVecPtr)[fileHandlesIdx], fileLockType,
            currentOffset, blockSize, libaioContext.iocbVec[ioVecIdx].aio_lio_opcode==IO_CMD_PWRITE,
            false /*isUnlock*/, NULL);

        int submitRes = io_submit(libaioContext.ioContext, 1,
            &libaioContext.iocbPointerVec[ioVecIdx] );
        IF_UNLIKELY(submitRes != 1)
        {
            FileTk::flock<WorkerException>( (*fileHandles.fdVecPtr)[fileHandlesIdx], fileLockType,
                currentOffset, blockSize, true /*ignored*/, true /*isUnlock*/, NULL);

            throw WorkerException(std::string("Async IO submission (io_submit) failed. ") +
                "NumRequests: " + std::to_string(numPending) + "; "
                "ReturnCode: " + std::to_string(submitRes) + "; "
                "SysErr: " + strerror(-submitRes) ); // (io_submit returns negative errno)
        }

        numIOPSSubmitted++;
        rwOffsetGen->addBytesSubmitted(blockSize);

        return SubmitRes_SUBMITTED;
    };

    /* Keep all free slots busy while there are bytes left to submit. A slot can stay free when
        its next journaled I/O has to wait for one of this thread's own in-flight I/Os; the next
        completion re-runs this and gets it going then. */
    auto fillIdleSlots = [&]() __attribute__( (always_inline) )
    {
        while(!freeSlots.empty() && rwOffsetGen->getNumBytesLeftToSubmit() )
        {
            const SubmitRes submitRes = trySubmitSlot(freeSlots.back() );

            if(submitRes == SubmitRes_SKIPPED)
                continue; // journal skip, so this slot is still free for the next draw

            IF_UNLIKELY(submitRes == SubmitRes_DEFERRED)
                break; /* waiting for a journal lock held by one of our own in-flight I/Os, so
                    retry after the next completion */

            freeSlots.pop_back();
            numPending++;
        }
    };

    // P H A S E 1: initial seed of io submissions up to full ioDepth

    fillIdleSlots();


	// P H A S E 2: wait for submissions to complete and submit new requests if bytes left

	while(numPending)
	{
		ioTimeout.tv_sec = AIO_MAX_WAIT_SEC;
		ioTimeout.tv_nsec = 0;

        int eventsRes = io_getevents(libaioContext.ioContext, 1, AIO_MAX_EVENTS, ioEvents,
            &ioTimeout);
		IF_UNLIKELY(!eventsRes)
		{ // timeout expired; that's ok, as we set a short timeout to check interruptions
			checkInterruptionRequest();
			continue;
		}
		else
		IF_UNLIKELY(eventsRes < 0)
		{
			throw WorkerException(std::string("Getting async IO events (io_getevents) failed. ") +
				"NumPending: " + std::to_string(numPending) + "; "
				"ReturnCode: " + std::to_string(eventsRes) + "; "
				"Wait time: " + std::to_string(AIO_MAX_WAIT_SEC) + "; "
				"Wait time left: " + std::to_string(ioTimeout.tv_sec) + "; "
				"SysErr: " + strerror(-eventsRes) ); // (io_getevents returns negative errno)
		}

		// check result of completed iocbs and reuse them if any blocks left to submit

		for(int eventIdx = 0; eventIdx < eventsRes; eventIdx++)
		{
			// ioEvents[].res2 is negative errno for aio framework errors, 0 means no error
			// ioEvents[].res is actually read/written bytes when res2==0 or negative errno
			/* note: all messy with res/res2, because defined as ulong, but examples need them
				interpreted as int for errors, which can overlap valid partial writes on 64bit */

            const size_t ioVecIdx = (size_t)ioEvents[eventIdx].data; // caller priv data is vec idx

			IF_UNLIKELY(ioEvents[eventIdx].res2 ||
				(ioEvents[eventIdx].res != ioEvents[eventIdx].obj->u.c.nbytes) )
            { /* unexpected result. (the journal guard of this slot - and of all others still in
                 flight - gets released by journalSlotsReleaser; journal cells of a failed write
                 are deliberately left at 0, i.e. "uninitialized", because a failed or partial
                 write cannot be assumed to have left the previous content intact.) */

                /* log the failed completion before any of this block's exits, so that each
                    logged submission has a counterpart also when things go wrong */
                OPLOG_POST_OP(
                    (ioEvents[eventIdx].obj->aio_lio_opcode == IO_CMD_PREAD) ?
                        "aioread" : "aiowrite",
                    std::to_string(ioEvents[eventIdx].obj->aio_fildes),
                    ioEvents[eventIdx].obj->u.c.offset, ioEvents[eventIdx].obj->u.c.nbytes,
                    true /*isError*/);

				if(ioEvents[eventIdx].res2)
					throw WorkerException(std::string("Async IO framework error. ") +
						"NumPending: " + std::to_string(numPending) + "; "
						"res: " + std::to_string(ioEvents[eventIdx].res) + "; "
						"res2: " + std::to_string(ioEvents[eventIdx].res2) + "; "
						"IO size: " + std::to_string(ioEvents[eventIdx].obj->u.c.nbytes) + "; "
						"SysErr: " + strerror(-(int)ioEvents[eventIdx].res2) );

	            FileTk::flock<WorkerException>(ioEvents[eventIdx].obj->aio_fildes, fileLockType,
	                ioEvents[eventIdx].obj->u.c.offset, ioEvents[eventIdx].obj->u.c.nbytes,
	                true /*ignored*/, true /*isUnlock*/, NULL);

				if( (int)ioEvents[eventIdx].res < 0)
				{
					errno = -(int)ioEvents[eventIdx].res; // res is negative errno
					return -1;
				}

				// partial read/write, so return what we got so far
				return (numBytesDone + ioEvents[eventIdx].res);
			}

            FileTk::flock<WorkerException>(ioEvents[eventIdx].obj->aio_fildes, fileLockType,
                ioEvents[eventIdx].obj->u.c.offset, ioEvents[eventIdx].obj->u.c.nbytes,
                true /*ignored*/, true /*isUnlock*/, NULL);

            /* counterpart to the OPLOG_PRE_OP of the aio preppers. logged here, where the I/O
                result is known, and thus before the post-read processing below - just like the
                sync wrappers (e.g. preadWrapper() ) log directly after their syscall and before
                the data verification of their caller. */
            OPLOG_POST_OP(
                (ioEvents[eventIdx].obj->aio_lio_opcode == IO_CMD_PREAD) ?
                    "aioread" : "aiowrite",
                std::to_string(ioEvents[eventIdx].obj->aio_fildes),
                ioEvents[eventIdx].obj->u.c.offset, ioEvents[eventIdx].obj->u.c.nbytes,
                false /*isError*/);

            if(journalEnabled) // make this slot's journal context the active one for the checker
                activeJournalIO = &journalIOSlots[ioVecIdx];

			((*this).*funcPostReadCudaMemcpy)(ioBufVec[ioVecIdx], gpuIOBufVec[ioVecIdx],
				ioEvents[eventIdx].obj->u.c.nbytes);
			((*this).*funcPostReadBlockChecker)( (char*)ioEvents[eventIdx].obj->u.c.buf,
				gpuIOBufVec[ioVecIdx], ioEvents[eventIdx].obj->u.c.nbytes,
				ioEvents[eventIdx].obj->u.c.offset);

            freeSlots.push_back(ioVecIdx);
            numPending--;

            if(journalEnabled)
                journalCommitIO(ioVecIdx); /* commits new generations for a write and releases
                    the journal lock that this slot held since its submission */

			// calc io operation latency
			std::chrono::steady_clock::time_point ioEndT = std::chrono::steady_clock::now();
			std::chrono::microseconds ioElapsedMicroSec =
				std::chrono::duration_cast<std::chrono::microseconds>
				(ioEndT - libaioContext.ioStartTimeVec[ioVecIdx] );

			numBytesDone += ioEvents[eventIdx].res;

			// inc special rwmix read stats
			if(	(ioEvents[eventIdx].obj->aio_lio_opcode == IO_CMD_PREAD) &&
				(globalBenchPhase == BenchPhase_CREATEFILES) )
			{ // this is a read in a write phase => inc rwmix read stats
                // don't count latency if this I/O had to wait for rate limiter
                IF_LIKELY(libaioContext.ioStartTimeVec[ioVecIdx] !=
                    std::chrono::steady_clock::time_point::min() )
                    iopsLatHistoReadMix.addLatency(ioElapsedMicroSec.count() );

				atomicLiveOpsReadMix.numBytesDone += ioEvents[eventIdx].res;
				atomicLiveOpsReadMix.numIOPSDone++;
			}
			else
			{
                // don't count latency if this I/O had to wait for rate limiter
                IF_LIKELY(libaioContext.ioStartTimeVec[ioVecIdx] !=
                    std::chrono::steady_clock::time_point::min() )
                    iopsLatHisto.addLatency(ioElapsedMicroSec.count() );

				atomicLiveOps.numBytesDone += ioEvents[eventIdx].res;
				atomicLiveOps.numIOPSDone++;
			}

			checkInterruptionRequest();

        } // end of for loop over completed iocbs

        // completed slots are free now, so get them busy again
        fillIdleSlots();

	} // end of while loop until all blocks completed

	return rwOffsetGen->getNumBytesTotal();

#endif // LIBAIO_SUPPORT
}


#ifdef SPDK_SUPPORT

#define SPDK_AIO_MAX_WAIT_MS    5000 // periodic wakeup to check for interruption requests

/**
 * Drop-in replacement for aioBlockSized() when running in SPDK mode with ioDepth > 1. Uses a
 * preallocated pool of SpdkNvmeClient::IoContext objects (spdkContext.ioContextVec, sized to
 * ioDepth in initSpdk() ) instead of per-I/O allocation, and blocks (without busy-polling) on
 * SpdkNvmeClient::waitForCompletions() instead of io_getevents().
 *
 * If this->fileHandles contains multiple namespace IDs then they will be treated as a striped
 * single range, so sequential IOs would be done round-robin, like aioBlockSized().
 *
 * @return similar to pread/pwrite.
 * @throw WorkerException on SPDK submission errors.
 */
int64_t LocalWorker::spdkAioBlockSized()
{
    const BenchPhase globalBenchPhase = workersSharedData->currentBenchPhase;
    const size_t fileHandlesVecSize = fileHandles.fdVecPtr->size();
    const uint64_t fileSize = progArgs->getFileSize();
    const uint64_t fileOffsetBase = progArgs->getFileOffset();
    const bool isSingleFile = (fileHandlesVecSize == 1);
    const unsigned rwMixReadPercent = progArgs->getRWMixReadPercent();
    const uint32_t sectorSize = spdkContext.sectorSize;
    const bool journalEnabled = progArgs->isJournalingEnabled();

    size_t numPending = 0; // num requests submitted and pending for completion
    size_t numBytesDone = 0; // after successfully completed requests

    resetAsyncIOSlots(); // (members, so that the hot path doesn't allocate; see the method)

    // per-slot bookkeeping, since IoContext itself doesn't carry the opcode; indexed the same
    // way as spdkContext.ioContextVec/ioStartTimeVec
    std::vector<uint8_t>& isReadVec = asyncIsReadVec;
    std::vector<uint8_t>& isRWMixReadVec = asyncIsRWMixReadVec;

    std::vector<SpdkNvmeClient::IoContext*>& completedVec = spdkContext.completedVec;

    JournalSlotsReleaser journalSlotsReleaser(*this); // for the throwing paths

    /* stack of slots that currently have no I/O in flight. taking the next slot to submit into
        is O(1), so the completion path does not have to scan for free slots. */
    std::vector<size_t>& freeSlots = asyncFreeSlots;

    // decide read vs write for the next request, mirroring rwBlockSized()/aioRWMixPrepper()
    auto isNextOpARead = [&]() -> bool
    {
        if(benchPhase == BenchPhase_READFILES) // also covers rwmixthreads readers
            return true;

        return rwMixReadPercent &&
            ( ( (workerRank + numIOPSSubmitted) % 100) < rwMixReadPercent);
    };

    /* Submit (or resubmit) request for the given pool slot, using rwOffsetGen for the next offset
       & block size; throws on submission error, like aioBlockSized()'s io_submit() failure path.

       @return SubmitRes_SUBMITTED when the slot is busy now; SubmitRes_SKIPPED when the journal
        decided this block needs no I/O at all (JournalBlockIO::ACTION_SKIP); SubmitRes_DEFERRED
        when the journal locks are taken (JournalBlockIO::ACTION_DEFER). The slot stays free in
        the latter two cases. */
    auto trySubmitSlot = [&](const size_t slotIdx) __attribute__( (always_inline) ) -> SubmitRes
    {
        uint64_t rwOffsetGenNext;
        size_t blockSize;

        if(!journalEnabled)
        {
            rwOffsetGenNext = rwOffsetGen->getNextOffset();
            blockSize = rwOffsetGen->getNextBlockSizeToSubmit();
        }
        else
        { // reuse a previously deferred draw, if any, so no offset gets lost or drawn twice
            JournalPendingDraw& pendingDraw = journalPendingDraw;

            if(!pendingDraw.isValid)
            {
                pendingDraw.rwOffsetGenNext = rwOffsetGen->getNextOffset();
                pendingDraw.blockSize = rwOffsetGen->getNextBlockSizeToSubmit();
                pendingDraw.isValid = true;
            }

            rwOffsetGenNext = pendingDraw.rwOffsetGenNext;
            blockSize = pendingDraw.blockSize;
        }

        uint64_t currentOffset;
        size_t fileHandlesIdx;

        calcFileIdxAndOffsetStriped(rwOffsetGenNext, fileSize, fileOffsetBase, isSingleFile,
            fileHandlesIdx, currentOffset);

        const int nsID = (*fileHandles.fdVecPtr)[fileHandlesIdx];
        const uint64_t lba = currentOffset / sectorSize;
        const uint32_t lbaCount = blockSize / sectorSize;

        bool isRead = isNextOpARead();

        if(journalEnabled)
        { /* let the journal decide & lock; it can turn a rwmix read into an initializing write or
             skip the I/O altogether. the lock stays held until this slot's I/O completes. */
            const JournalBlockIO::Action journalAction = journalBeginIO(slotIdx, fileHandlesIdx,
                currentOffset, blockSize, journalGetIntent(rwMixReadPercent) );

            IF_UNLIKELY(journalAction == JournalBlockIO::ACTION_DEFER)
                return SubmitRes_DEFERRED; /* journal locks currently held for another in-flight
                    I/O of this thread; keep the draw and retry once that one completed */

            journalPendingDraw.isValid = false; // this draw gets used up now

            IF_UNLIKELY(journalAction == JournalBlockIO::ACTION_SKIP)
            { // nothing was ever written here, so accept as verified-ok without reading it
                numIOPSSubmitted++;
                rwOffsetGen->addBytesSubmitted(blockSize);

                return SubmitRes_SKIPPED;
            }

            isRead = (journalAction == JournalBlockIO::ACTION_READ);
        }

        isReadVec[slotIdx] = isRead;
        isRWMixReadVec[slotIdx] = isRead && (globalBenchPhase == BenchPhase_CREATEFILES);

        SpdkNvmeClient::IoContext* ioCtx = spdkContext.ioContextVec[slotIdx].get();

        spdkContext.ioStartTimeVec[slotIdx] = std::chrono::steady_clock::now();

        bool hadToWait = ((*this).*funcRWRateLimiter)(blockSize, isInterruptionRequested);
        IF_UNLIKELY(hadToWait) // invalidate start time of all pending due to rate limiter wait
            for(std::chrono::steady_clock::time_point& startT : spdkContext.ioStartTimeVec)
                startT = std::chrono::steady_clock::time_point::min();

        int submitRes;

        if(isRead)
        {
            OPLOG_PRE_OP("spdkRead", spdkContext.spdkClient.getNamespaceName(nsID),
                currentOffset, blockSize);
            submitRes = spdkContext.spdkClient.read(ioCtx, nsID, lba, lbaCount,
                ioBufVec[slotIdx]);
        }
        else
        {
            ((*this).*funcPreWriteBlockModifier)(ioBufVec[slotIdx], gpuIOBufVec[slotIdx],
                blockSize, currentOffset);
            ((*this).*funcPreWriteCudaMemcpy)(ioBufVec[slotIdx], gpuIOBufVec[slotIdx], blockSize);

            OPLOG_PRE_OP("spdkWrite", spdkContext.spdkClient.getNamespaceName(nsID),
                currentOffset, blockSize);
            submitRes = spdkContext.spdkClient.write(ioCtx, nsID, lba, lbaCount,
                ioBufVec[slotIdx]);
        }

        IF_UNLIKELY(submitRes != 0)
            throw WorkerException(std::string("Async SPDK IO submission failed. ") +
                "NamespaceID: " + std::to_string(nsID) + "; "
                "LBA: " + std::to_string(lba) + "; "
                "ReturnCode: " + std::to_string(submitRes) );

        numIOPSSubmitted++;
        rwOffsetGen->addBytesSubmitted(blockSize);

        return SubmitRes_SUBMITTED;
    };

    /* Keep all free slots busy while there are bytes left to submit. A slot can stay free when
        its next journaled I/O has to wait for one of this thread's own in-flight I/Os; the next
        completion re-runs this and gets it going then. */
    auto fillIdleSlots = [&]() __attribute__( (always_inline) )
    {
        while(!freeSlots.empty() && rwOffsetGen->getNumBytesLeftToSubmit() )
        {
            const SubmitRes submitRes = trySubmitSlot(freeSlots.back() );

            if(submitRes == SubmitRes_SKIPPED)
                continue; // journal skip, so this slot is still free for the next draw

            IF_UNLIKELY(submitRes == SubmitRes_DEFERRED)
                break; /* waiting for a journal lock held by one of our own in-flight I/Os, so
                    retry after the next completion */

            freeSlots.pop_back();
            numPending++;
        }
    };

    // P H A S E 1: initial seed of io submissions up to full ioDepth

    fillIdleSlots();

    // P H A S E 2: wait for submissions to complete and submit new requests if bytes left

    while(numPending)
    {
        completedVec.clear();

        int numDone = spdkContext.spdkClient.waitForCompletions(completedVec,
            SPDK_AIO_MAX_WAIT_MS);

        IF_UNLIKELY(!numDone)
        { // timeout expired; that's ok, as we set a short timeout to check interruptions
            checkInterruptionRequest();
            continue;
        }

        for(SpdkNvmeClient::IoContext* ioCtx : completedVec)
        {
            const size_t slotIdx = reinterpret_cast<size_t>(ioCtx->userData);
            const size_t blockSize = ioCtx->lbaCount * sectorSize;
            const uint64_t currentOffset = ioCtx->lba * sectorSize;
            const bool isRead = isReadVec[slotIdx];
            const bool isRWMixRead = isRWMixReadVec[slotIdx];

            IF_UNLIKELY(!ioCtx->ioSuccess)
            { /* (the journal guard of this slot - and of all others still in flight - gets
                 released by journalSlotsReleaser; journal cells of a failed write are
                 deliberately left at 0, i.e. "uninitialized", because a failed write cannot be
                 assumed to have left the previous content intact.) */
                errno = EIO;
                return (numBytesDone) ? (int64_t)numBytesDone : -1;
            }

            if(journalEnabled) // make this slot's journal context the active one for the checker
                activeJournalIO = &journalIOSlots[slotIdx];

            if(isRead)
            {
                ((*this).*funcPostReadCudaMemcpy)(ioBufVec[slotIdx], gpuIOBufVec[slotIdx],
                    blockSize);
                ((*this).*funcPostReadBlockChecker)( (char*)ioBufVec[slotIdx],
                    gpuIOBufVec[slotIdx], blockSize, currentOffset);
            }

            freeSlots.push_back(slotIdx);
            numPending--;

            if(journalEnabled)
                journalCommitIO(slotIdx); /* commits new generations for a write and releases the
                    journal lock that this slot held since its submission */

            OPLOG_POST_OP(isRead ? "spdkRead" : "spdkWrite", ioCtx->nsHandle->fullName,
                currentOffset, blockSize, false);

            // calc io operation latency
            std::chrono::steady_clock::time_point ioEndT = std::chrono::steady_clock::now();
            std::chrono::microseconds ioElapsedMicroSec =
                std::chrono::duration_cast<std::chrono::microseconds>
                (ioEndT - spdkContext.ioStartTimeVec[slotIdx] );

            if(isRWMixRead)
            {
                // don't count latency if this I/O had to wait for rate limiter
                IF_LIKELY(spdkContext.ioStartTimeVec[slotIdx] !=
                    std::chrono::steady_clock::time_point::min() )
                    iopsLatHistoReadMix.addLatency(ioElapsedMicroSec.count() );

                atomicLiveOpsReadMix.numBytesDone += blockSize;
                atomicLiveOpsReadMix.numIOPSDone++;
            }
            else
            {
                // don't count latency if this I/O had to wait for rate limiter
                IF_LIKELY(spdkContext.ioStartTimeVec[slotIdx] !=
                    std::chrono::steady_clock::time_point::min() )
                    iopsLatHisto.addLatency(ioElapsedMicroSec.count() );

                atomicLiveOps.numBytesDone += blockSize;
                atomicLiveOps.numIOPSDone++;
            }

            numBytesDone += blockSize;

            checkInterruptionRequest();
        }

        // completed slots are free now, so get them busy again
        fillIdleSlots();
    }

    return rwOffsetGen->getNumBytesTotal();
}

#endif // SPDK_SUPPORT

/**
 * Calculate the next file and offset within file for rwBlockSized and aioBlockSized.
 *
 * If only one file in fileHandlesVec then rwOffsetGenNext is the next offset. Otherwise the set
 * of files is treated like a single virtual large file with one file after the other and all
 * files are expected to have progArgs->getFileSize() length.
 *
 * The offset generators work in a logical range starting at 0, so the user-defined minimum offset
 * ("--offset") gets added here to get the absolute offset within the file. This is the only place
 * where it gets added for the file/bdev/dir mode I/O paths.
 *
 * @fileSize progArgs->getFileSize().
 * @fileOffsetBase progArgs->getFileOffset(), the user-defined minimum offset within each file.
 * @isSingleFile true if there is only one file in fileHandlesVec.
 * @outFileOffset absolute offset within outNextFileIdx, including fileOffsetBase.
 * @outFileIdx index within fileHandlesVec.
 */
void LocalWorker::calcFileIdxAndOffsetStriped(const uint64_t rwOffsetGenNext,
    const uint64_t fileSize, const uint64_t fileOffsetBase, const bool isSingleFile,
    size_t& outFileIdx, uint64_t& outFileOffset)
{
    if(isSingleFile)
    { // single file
        outFileIdx = 0;
        outFileOffset = rwOffsetGenNext;
    }
    else
    { // multiple files => treat them as single serial block range, one file after the other
        outFileIdx = rwOffsetGenNext / fileSize;
        outFileOffset = rwOffsetGenNext % fileSize;
    }

    outFileOffset += fileOffsetBase; // add user-defined minimum offset within file

    LOGGER_DEBUG_BUILD(__func__ << ": " <<
        "workerRank: " << workerRank << "; " <<
        "rwOffsetGenNext: " << rwOffsetGenNext << "; " <<
        "fileSize: " << fileSize << "; " <<
        "fileOffsetBase: " << fileOffsetBase << "; " <<
        "isSingleFile: " << isSingleFile << "; " <<
        "outFileIdx: " << outFileIdx << "; " <<
        "outFileOffset: " << outFileOffset <<
        std::endl);
}

/**
 * Noop for the case when no integrity check selected by user.
 */
void LocalWorker::noOpIntegrityCheck(char* hostIOBuf, char* gpuIOBuf, size_t bufLen,
	off_t fileOffset)
{
	return; // noop
}

/**
 * Fill buf with unsigned 64bit values made of offset plus integrity check salt.
 *
 * @bufLen buf len to fill with checksums
 * @fileOffset file offset for buf
 */
void LocalWorker::preWriteIntegrityCheckFillBuf(char* hostIOBuf, char* gpuIOBuf, size_t bufLen,
	off_t fileOffset)
{
	const size_t checkSumLen = sizeof(uint64_t);
	const uint64_t checkSumSalt = progArgs->getIntegrityCheckSalt();

	size_t numBytesDone = 0;
	size_t numBytesLeft = bufLen;
	off_t currentOffset = fileOffset;

	/* note: fileOffset and bufLen are not guaranteed to be a multiple of uint64_t (e.g. if
	   blocksize is 1 byte). We also want to support writes and verficiation reads to use different
	   block size (bufLen). So we only copy the relevant part of the uint64_t to buf. */

	while(numBytesLeft)
	{
		/* checksum value is always calculated aligned to 8 byte block size, even if we only copy a
		   partial block. */

		// (note: after the 1st loop pass, the remaining offsets will be 8 byte aligned.)

		// 8 byte aligned offset as basis for checksum value calculation
		off_t checkSumStartOffset = currentOffset - (currentOffset % checkSumLen);

		uint64_t checkSum = checkSumStartOffset + checkSumSalt;

		char* checkSumArray = (char*)&checkSum; // byte-addressable array for checksum value
		off_t checkSumArrayStartIdx = currentOffset - checkSumStartOffset;
		size_t checkSumCopyLen = std::min(
			(uint64_t)numBytesLeft, (uint64_t)checkSumLen - checkSumArrayStartIdx);

		memcpy(&hostIOBuf[numBytesDone], &checkSumArray[checkSumArrayStartIdx], checkSumCopyLen);

		numBytesDone += checkSumCopyLen;
		numBytesLeft -= checkSumCopyLen;
		currentOffset += checkSumCopyLen;
	}
}

/**
 * Verify buffer contents as counterpart to preWriteIntegrityCheckFillBuf.
 *
 * @bufLen buf len to fill with checksums
 * @fileOffset file offset for buf
 * @throw WorkerException if verification fails.
 */
void LocalWorker::postReadIntegrityCheckVerifyBuf(char* hostIOBuf, char* gpuIOBuf, size_t bufLen,
	off_t fileOffset)
{
	IF_UNLIKELY(!bufLen)
		return;

	char* verifyBuf = (char*)malloc(bufLen);

	IF_UNLIKELY(!verifyBuf)
		throw WorkerException("Buffer alloc for verification buffer failed. "
			"Size: " + std::to_string(bufLen) );

	// fill verifyBuf with the correct data
	preWriteIntegrityCheckFillBuf(verifyBuf, gpuIOBuf, bufLen, fileOffset);

	// compare correct data to actual data
	int compareRes = memcmp(hostIOBuf, verifyBuf, bufLen);

	if(!compareRes)
	{ // buffers are equal, so all good
		free(verifyBuf);
		return;
	}

	// verification failed, find exact mismatch offset
	for(size_t i=0; i < bufLen; i++)
	{
		if(verifyBuf[i] == hostIOBuf[i])
			continue;

		// we found the exact offset for mismatch

		unsigned expectedVal = (unsigned char)verifyBuf[i];
		unsigned actualVal = (unsigned char)hostIOBuf[i];

		free(verifyBuf);

		throw WorkerException("Data verification failed. "
			"Offset: " + std::to_string(fileOffset + i) + "; "
			"Expected value: " + std::to_string(expectedVal) + "; "
			"Actual value: " + std::to_string(actualVal) );
	}
}

/**
 * Compute the journal's expected content tile for one journal block and repeat ("tile") it
 * across buf. Pure function, no member state, so it's shared verbatim by
 * preWriteJournalFillBuf() and postReadJournalVerifyBuf() to fill/verify one journal sub-block
 * at a time.
 *
 * @blockAlignedOffset absolute target offset of the start of this journal block.
 * @generation the block's content generation (1..3); never called with 0 ("never written").
 */
void LocalWorker::journalFillTileBlock(char* buf, size_t len, off_t blockAlignedOffset,
    uint64_t journalSeed, uint8_t generation)
{
    const uint64_t tileValue = Journal::computeExpectedTileValue(blockAlignedOffset, journalSeed,
        generation);
    const char* tileBytes = (const char*)&tileValue;

    for(size_t i = 0; i < len; i++)
        buf[i] = tileBytes[i % sizeof(uint64_t)];
}

/**
 * Map this worker's read/write decision for the next I/O to the corresponding journal intent.
 * (Used by the async engines, where the read/write decision otherwise only materializes inside
 * funcAioRwPrepper; the sync engine derives both from one branch instead.)
 */
JournalBlockIO::Intent LocalWorker::journalGetIntent(unsigned rwMixReadPercent)
{
    if(benchPhase == BenchPhase_READFILES)
    {
        /* A rwmix threads reader has its own benchPhase set to a read phase while the global
           phase is a write phase, so it is part of a read/write mix: it initializes
           uninitialized blocks instead of skipping them. Otherwise it would never have anything
           to verify, because each thread works on its own separate range of the dataset (see
           fileModeIterateFilesRand() ), so the writer threads never touch the ranges that the
           reader threads read. A real read phase only ever reads. */
        return (workersSharedData->currentBenchPhase == BenchPhase_READFILES) ?
            JournalBlockIO::INTENT_READ_SKIP_UNINIT : JournalBlockIO::INTENT_READ_OR_INIT;
    }

    if(rwMixReadPercent && ( ( (workerRank + numIOPSSubmitted) % 100) < rwMixReadPercent) )
    { /* a rwmixpct read; this thread writes as well, so an uninitialized block becomes an
         initializing write instead of being skipped */
        return JournalBlockIO::INTENT_READ_OR_INIT;
    }

    return JournalBlockIO::INTENT_WRITE;
}

/**
 * Let the journal decide and lock the given I/O, and make the result the active journal context
 * for this worker's journal buffer fill/verify functions.
 *
 * The acquired journal lock stays held until the slot's journalCommitIO()/journalReleaseIO(),
 * which for the async engines is when the I/O completes.
 *
 * Waiting for the journal locks is only allowed while this thread holds none of its own, so with
 * an async engine a submission can come back as JournalBlockIO::ACTION_DEFER instead (see
 * JournalBlockIO::begin() ); the caller then has to retry this I/O after one of its in-flight
 * I/Os completed.
 *
 * @slotIdx in-flight I/O slot, i.e. always 0 for the sync engine and the iodepth slot index for
 *  the async engines.
 * @return what this I/O should actually do (which may differ from the intent).
 */
JournalBlockIO::Action LocalWorker::journalBeginIO(size_t slotIdx, size_t fileHandleIdx,
    uint64_t currentOffset, size_t ioLen, JournalBlockIO::Intent intent)
{
    JournalBlockIO& journalIO = journalIOSlots[slotIdx];

    const JournalBlockIO::Action action = journalIO.begin(
        fileHandles.journalPtrVec[fileHandleIdx], currentOffset, ioLen, intent,
        !journalNumGuardsHeld /*mayBlock*/);

    if( (action == JournalBlockIO::ACTION_READ) || (action == JournalBlockIO::ACTION_WRITE) )
        journalNumGuardsHeld++; // this slot now holds a guard until commit/release

    activeJournalIO = &journalIO;

    return action;
}

/**
 * Finish a successful journaled I/O: commit a write's new generations and release the guard.
 */
void LocalWorker::journalCommitIO(size_t slotIdx)
{
    journalIOSlots[slotIdx].commit();

    journalNumGuardsHeld--;
}

/**
 * Give up a journaled I/O without committing, e.g. after a failed I/O.
 */
void LocalWorker::journalReleaseIO(size_t slotIdx)
{
    journalIOSlots[slotIdx].release();

    journalNumGuardsHeld--;
}

/**
 * Fill buf with the journal's expected content, one journal block at a time, using the
 * generations that the active journal context prepared for this I/O.
 *
 * Noop when the current I/O is a read, mirroring how preWriteBufRandRefill() skips reads.
 *
 * @bufLen buf len to fill.
 * @fileOffset absolute target offset for buf; aligned to the journal block size (guaranteed by
 *  the checks in ProgArgs::checkPathDependentArgs(), which require every block size, "--offset"
 *  and "--size" to be a multiple of "--journalblock" and reject "--norandalign").
 */
void LocalWorker::preWriteJournalFillBuf(char* hostIOBuf, char* gpuIOBuf, size_t bufLen,
    off_t fileOffset)
{
    if(!activeJournalIO->isWrite() )
        return; // this is a read in journal mode, so there is nothing to fill

    const Journal* journal = activeJournalIO->getJournal();
    const std::vector<uint8_t>& gens = activeJournalIO->getGens();
    const uint64_t journalBlockSize = journal->getJournalBlockSize();
    const uint64_t journalSeed = journal->getJournalSeed();

    size_t numBytesDone = 0;

    while(numBytesDone < bufLen)
    {
        const size_t subBlockIdx = numBytesDone / journalBlockSize;
        const size_t subBlockCopyLen = std::min(bufLen - numBytesDone, (size_t)journalBlockSize);

        journalFillTileBlock(&hostIOBuf[numBytesDone], subBlockCopyLen,
            fileOffset + (off_t)numBytesDone, journalSeed, gens[subBlockIdx] );

        numBytesDone += subBlockCopyLen;
    }
}

/**
 * Verify buffer contents against the journal's expected content, one journal block at a time,
 * skipping any block whose generation is 0 ("never written", or a prior write to it failed -
 * either way accepted as OK without verification).
 *
 * Noop when the current I/O is a write, because then the buffer trivially holds what we just
 * wrote and there is nothing to be learned from comparing it.
 *
 * @throw WorkerException if verification fails for an initialized block.
 */
void LocalWorker::postReadJournalVerifyBuf(char* hostIOBuf, char* gpuIOBuf, size_t bufLen,
    off_t fileOffset)
{
    if(activeJournalIO->isWrite() )
        return; // nothing to verify after a write

    const Journal* journal = activeJournalIO->getJournal();
    const std::vector<uint8_t>& gens = activeJournalIO->getGens();
    const uint64_t journalBlockSize = journal->getJournalBlockSize();
    const uint64_t journalSeed = journal->getJournalSeed();

    size_t numBytesDone = 0;

    while(numBytesDone < bufLen)
    {
        const size_t subBlockIdx = numBytesDone / journalBlockSize;
        const size_t subBlockCopyLen = std::min(bufLen - numBytesDone, (size_t)journalBlockSize);
        const uint8_t generation = gens[subBlockIdx];

        if(!generation)
        { // never written (or a prior write to it failed); nothing to verify, accepted as OK
            numBytesDone += subBlockCopyLen;
            continue;
        }

        journalFillTileBlock(journalVerifyBuf, subBlockCopyLen, fileOffset + (off_t)numBytesDone,
            journalSeed, generation);

        int compareRes = memcmp(&hostIOBuf[numBytesDone], journalVerifyBuf, subBlockCopyLen);

        if(compareRes)
        { // verification failed, find exact mismatch offset
            for(size_t i = 0; i < subBlockCopyLen; i++)
            {
                if(journalVerifyBuf[i] == hostIOBuf[numBytesDone + i] )
                    continue;

                throw WorkerException("Journal data verification failed. "
                    "Offset: " + std::to_string(fileOffset + numBytesDone + i) + "; "
                    "Expected value: " +
                        std::to_string( (unsigned char)journalVerifyBuf[i] ) + "; "
                    "Actual value: " +
                        std::to_string( (unsigned char)hostIOBuf[numBytesDone + i] ) );
            }
        }

        numBytesDone += subBlockCopyLen;
    }
}

/**
 * Fill buffer with given value. In contrast to memset() this can fill 64bit values to at least
 * make simple dedupe less likely among all the different non-variable block remainders.
 */
void LocalWorker::bufFill(char* buf, uint64_t fillValue, size_t bufLen)
{
	size_t numBytesDone = 0;

	for(uint64_t i=0; i < (bufLen / sizeof(uint64_t) ); i++)
	{
		uint64_t* uint64Buf = (uint64_t*)buf;
		*uint64Buf = fillValue;

		buf += sizeof(uint64_t);
		numBytesDone += sizeof(uint64_t);
	}

	if(numBytesDone == bufLen)
		return; // all done, complete buffer filled

	// we have a remainder to fill, which can only be smaller than sizeof(uint64_t)
	memcpy(buf, &fillValue, bufLen - numBytesDone);
}

/**
 * Refill some percentage of the buffer with random data. The percentage to refill is defined via
 * progArgs::blockVariancePercent.
 */
void LocalWorker::preWriteBufRandRefill(char* hostIOBuf, char* gpuIOBuf, size_t bufLen,
	off_t fileOffset)
{
	// note: this same logic is used in aioRWMixPrepper/pwriteRWMixWrapper
	if( ( (workerRank + numIOPSSubmitted) % 100) < progArgs->getRWMixReadPercent() )
		return; // this is a read in rwmix mode, so no need for refill in this round

	// refill buffer with random data

	const unsigned blockVariancePercent = progArgs->getBlockVariancePercent();
	const uint64_t varFillLen = (bufLen * blockVariancePercent) / 100;
	const size_t constFillRemainderLen = bufLen - varFillLen;

	randBlockVarAlgo->fillBuf(hostIOBuf, varFillLen);

	if(!constFillRemainderLen)
		return;

	// fill remainder of buffer with same 64bit value
	// note: rand algo is used to defeat simple dedupe across remainders of different blocks
	bufFill(&hostIOBuf[varFillLen], randBlockVarAlgo->next(), constFillRemainderLen);
}

/**
 * Refill some percentage of the GPU buffer with random data. The percentage to refill is defined
 * via progArgs::blockVariancePercent.
 */
void LocalWorker::preWriteBufRandRefillCuda(char* hostIOBuf, char* gpuIOBuf, size_t bufLen,
	off_t fileOffset)
{
#ifndef CUDA_SUPPORT

	throw WorkerException("preWriteBufRandRefillCuda called, but this executable was built without "
		"CUDA support.");

#else // CUDA_SUPPORT

	// note: this same logic is used in aioRWMixPrepper/pwriteRWMixWrapper
	if( ( (workerRank + numIOPSSubmitted) % 100) < progArgs->getRWMixReadPercent() )
		return; // this is a read in rwmix mode, so no need for refill in this round

	// refill buffer with random data

	const unsigned blockVariancePercent = progArgs->getBlockVariancePercent();
	uint64_t varFillLen = (bufLen * blockVariancePercent) / 100;

	if(varFillLen % sizeof(int) ) // curandGenerate can only fill full 32bit values
		varFillLen -= (varFillLen % sizeof(int) );

	const size_t constFillRemainderLen = bufLen - varFillLen;

	curandStatus_t randGenRes = curandGenerate(gpuRandGen, (unsigned int*)gpuIOBuf,
		varFillLen / sizeof(int) );

	IF_UNLIKELY(randGenRes != CURAND_STATUS_SUCCESS)
		throw WorkerException("Random number generation via GPU/CUDA failed. "
			"curand error code: " + std::to_string(randGenRes) );

	if(!constFillRemainderLen)
		return;

	/* fill remainder of host buffer with same 64bit value and copy over to gpu (in lack of a good
		way of filling a buffer with a 64bit value directly on the gpu) */
	// note: rand algo is used to defeat simple dedupe across remainders of different blocks
	bufFill(&hostIOBuf[varFillLen], randBlockVarAlgo->next(), constFillRemainderLen);
	cudaMemcpyHostToGPU(&hostIOBuf[varFillLen], &gpuIOBuf[varFillLen], constFillRemainderLen);

#endif // CUDA_SUPPORT
}


/**
 * Simple wrapper for io_prep_pwrite().
 */
void LocalWorker::aioWritePrepper(struct iocb* iocb, int fd, void* buf, size_t count,
	long long offset)
{
#ifndef LIBAIO_SUPPORT

	throw WorkerException("Async IO via libaio requested, but this executable was built without "
		"libaio support.");

#else // LIBAIO_SUPPORT

    OPLOG_PRE_OP("aiowrite", std::to_string(fd), offset, count);

	io_prep_pwrite(iocb, fd, buf, count, offset);

#endif // LIBAIO_SUPPORT
}

/**
 * Simple wrapper for io_prep_pread().
 */
void LocalWorker::aioReadPrepper(struct iocb* iocb, int fd, void* buf, size_t count,
	long long offset)
{
#ifndef LIBAIO_SUPPORT

	throw WorkerException("Async IO via libaio requested, but this executable was built without "
		"libaio support.");

#else // LIBAIO_SUPPORT

    OPLOG_PRE_OP("aioread", std::to_string(fd), offset, count);

	io_prep_pread(iocb, fd, buf, count, offset);

#endif // LIBAIO_SUPPORT
}

/**
 * Within a write phase, send user-defined pecentage of block reads for mixed r/w.
 *
 * Parameters are similar to io_prep_p{write,read}.
 */
void LocalWorker::aioRWMixPrepper(struct iocb* iocb, int fd, void* buf, size_t count,
	long long offset)
{
#ifndef LIBAIO_SUPPORT

	throw WorkerException("Async IO via libaio requested, but this executable was built without "
		"libaio support.");

#else // LIBAIO_SUPPORT

	// example: 40% means 40 out of 100 submitted blocks will be reads, the remaining 60 are writes

	/* note: keep in mind that this also needs to work with lots of small files, so percentage needs
		to work between different files. (numIOPSSubmitted ensures that below; numIOPSDone would not
		work for this because aio would not inc counter directly on submission.) */

	// note: workerRank is used to have skew between different worker threads
	// note: this same logic is used in preWriteBufRandRefill/preWriteBufRandRefillFast
	if( ( (workerRank + numIOPSSubmitted) % 100) >= progArgs->getRWMixReadPercent() )
    {
        OPLOG_PRE_OP("aiowrite", std::to_string(fd), offset, count);

        io_prep_pwrite(iocb, fd, buf, count, offset);
    }
    else
    {
        OPLOG_PRE_OP("aioread", std::to_string(fd), offset, count);

        io_prep_pread(iocb, fd, buf, count, offset);
    }

#endif // LIBAIO_SUPPORT
}

/**
 * Prep a read or write based on what the journal decided for this I/O (see
 * JournalBlockIO::begin() ), which is why this replaces aioWritePrepper/aioReadPrepper/
 * aioRWMixPrepper whenever journaling is enabled: an uninitialized block can turn a rwmix read
 * into an initializing write, so the decision cannot be re-derived here.
 *
 * Parameters are similar to io_prep_p{write,read}.
 */
void LocalWorker::aioJournalPrepper(struct iocb* iocb, int fd, void* buf, size_t count,
    long long offset)
{
#ifndef LIBAIO_SUPPORT

    throw WorkerException("Async IO via libaio requested, but this executable was built without "
        "libaio support.");

#else // LIBAIO_SUPPORT

    if(activeJournalIO->isWrite() )
    {
        OPLOG_PRE_OP("aiowrite", std::to_string(fd), offset, count);

        io_prep_pwrite(iocb, fd, buf, count, offset);
    }
    else
    {
        OPLOG_PRE_OP("aioread", std::to_string(fd), offset, count);

        io_prep_pread(iocb, fd, buf, count, offset);
    }

#endif // LIBAIO_SUPPORT
}

/**
 * Noop for cases where no rate limit selected by user.
 *
 * @return true if we had to wait, false if we are good to go immediately.
 */
bool LocalWorker::noOpRateLimiter(size_t rwSize, std::atomic_bool& isInterruptionRequested)
{
    return false; // noop
}

/**
 * Rate limiter before writes/reads in case rate limit was selected by user.
 *
 * @return true if we had to wait, false if we are good to go immediately.
 */
bool LocalWorker::preRWRateLimiter(size_t rwSize, std::atomic_bool& isInterruptionRequested)
{
    return rateLimiter.wait(rwSize);
}

/**
 * Rate limiter before reads in case rwmix threads rate balance was selected by user.
 *
 * @return true if we had to wait, false if we are good to go immediately.
 */
bool LocalWorker::preRWRateBalanceLimiterForReaders(size_t rwSize,
    std::atomic_bool& isInterruptionRequested)
{
    return rateLimiterRWMixThreads.waitRead(rwSize, isInterruptionRequested);
}

/**
 * Rate limiter before writes in case rwmix threads rate balance was selected by user.
 *
 * @return true if we had to wait, false if we are good to go immediately.
 */
bool LocalWorker::preRWRateBalanceLimiterForWriters(size_t rwSize,
    std::atomic_bool& isInterruptionRequested)
{
    return rateLimiterRWMixThreads.waitWrite(rwSize, isInterruptionRequested);
}

/**
 * Noop for cases where preWriteCudaMemcpy & postReadCudaMemcpy are not appropriate.
 */
void LocalWorker::noOpCudaMemcpy(void* hostIOBuf, void* gpuIOBuf, size_t count)
{
	return; // noop
}

/**
 * Copy hostIOBuf to gpuIOBuf. This is e.g. to simulate transfer of file contents to GPU buffer
 * after file read.
 *
 * @count number of bytes to copy.
 * @throw WorkerException if buffer copy fails.
 */
void LocalWorker::cudaMemcpyGPUToHost(void* hostIOBuf, void* gpuIOBuf, size_t count)
{
#ifdef CUDA_SUPPORT

	cudaError_t copyRes = cudaMemcpy(hostIOBuf, gpuIOBuf, count, cudaMemcpyDeviceToHost);

	IF_UNLIKELY(copyRes != cudaSuccess)
		throw WorkerException("Initialization of GPU buffer via memcpy failed. "
			"Buffer size: " + std::to_string(progArgs->getBlockSize() ) + "; "
			"Byte count: " + std::to_string(count) + "; "
			"GPU ID: " + std::to_string(gpuID) + "; "
			"CUDA Error: " + cudaGetErrorString(copyRes) );

#endif // CUDA_SUPPORT
}

/**
 * Copy hostIOBuf to gpuIOBuf. This is e.g. to simulate transfer of file contents to GPU buffer
 * after a file read.
 *
 * @count number of bytes to copy.
 * @throw WorkerException if buffer copy fails.
 */
void LocalWorker::cudaMemcpyHostToGPU(void* hostIOBuf, void* gpuIOBuf, size_t count)
{
#ifdef CUDA_SUPPORT

	cudaError_t copyRes = cudaMemcpy(gpuIOBuf, hostIOBuf, count, cudaMemcpyHostToDevice);

	IF_UNLIKELY(copyRes != cudaSuccess)
		throw WorkerException("Initialization of GPU buffer via memcpy failed. "
			"Buffer size: " + std::to_string(progArgs->getBlockSize() ) + "; "
			"Byte count: " + std::to_string(count) + "; "
			"GPU ID: " + std::to_string(gpuID) + "; "
			"CUDA Error: " + cudaGetErrorString(copyRes) );

#endif // CUDA_SUPPORT
}

/**
 * Noop cuFile handle register for cases where executable is built without CUFILE_SUPPORT or where
 * cuFile API was not selected by user.
 */
void LocalWorker::noOpCuFileHandleReg(int fd, CuFileHandleData& handleData)
{
	return; // noop
}

/**
 * Noop cuFile handle /deregister for cases where executable is built without CUFILE_SUPPORT or
 * where cuFile API was not selected by user.
 */
void LocalWorker::noOpCuFileHandleDereg(CuFileHandleData& handleData)
{
	return; // noop
}

/**
 * cuFile handle register as preparation for cuFileRead/Write in dir mode. Call cuFileHandleDereg
 * when done with file access.
 *
 * @fd posix file handle to register for cuFile access
 * @outHandleData the registered handle on success, in which case outHandleData.isCuFileRegistered
 * 		will be set to true.
 * @throw WorkerException if registration fails.
 */
void LocalWorker::dirModeCuFileHandleReg(int fd, CuFileHandleData& outHandleData)
{
	outHandleData.registerHandle<WorkerException>(fd);
}

/**
 * Counterpart to cuFileHandleReg to deregister a file handle in dir mode. This is safe to call even
 * if registration was not called or if it failed, based on handleData.isCuFileRegistered.
 *
 * @handleData the same that was passed to cuFileHandleReg before.
 */
void LocalWorker::dirModeCuFileHandleDereg(CuFileHandleData& handleData)
{
	handleData.deregisterHandle();
}

/**
 * Wrapper for positional sync read.
 */
ssize_t LocalWorker::preadWrapper(size_t fileHandleIdx, void* buf, size_t nbytes, off_t offset)
{
	const int fd = (*fileHandles.fdVecPtr)[fileHandleIdx];

	OPLOG_PRE_OP("pread", std::to_string(fd), offset, nbytes);

	ssize_t preadRes = pread(fd, buf, nbytes, offset);

	OPLOG_POST_OP("pread", std::to_string(fd), offset, nbytes, preadRes == -1);

	return preadRes;
}

/**
 * Wrapper for positional sync write.
 */
ssize_t LocalWorker::pwriteWrapper(size_t fileHandleIdx, void* buf, size_t nbytes, off_t offset)
{
	const int fd = (*fileHandles.fdVecPtr)[fileHandleIdx];

	OPLOG_PRE_OP("pwrite", std::to_string(fd), offset, nbytes);

	ssize_t pwriteRes = pwrite(fd, buf, nbytes, offset);

	OPLOG_POST_OP("pwrite", std::to_string(fd), offset, nbytes, pwriteRes <= 0);

	return pwriteRes;
}

/**
 * Wrapper for positional sync write followed by an immediate read of the same block.
 */
ssize_t LocalWorker::pwriteAndReadWrapper(size_t fileHandleIdx, void* buf, size_t nbytes,
	off_t offset)
{
	const int fd = (*fileHandles.fdVecPtr)[fileHandleIdx];

	OPLOG_PRE_OP("pwrite", std::to_string(fd), offset, nbytes);

	ssize_t pwriteRes = pwrite(fd, buf, nbytes, offset);

	OPLOG_POST_OP("pwrite", std::to_string(fd), offset, nbytes, pwriteRes <= 0);

	IF_UNLIKELY(pwriteRes <= 0)
		return pwriteRes;

	OPLOG_PRE_OP("pread", std::to_string(fd), offset, nbytes);

	ssize_t preadRes = pread(fd, buf, pwriteRes, offset);

	OPLOG_POST_OP("pread", std::to_string(fd), offset, nbytes, preadRes == -1);

	return preadRes;
}

/**
 * Within a write phase, send user-defined pecentage of block reads for mixed r/w.
 *
 * Parameters and return value are similar to p{write,read}.
 */
ssize_t LocalWorker::pwriteRWMixWrapper(size_t fileHandleIdx, void* buf, size_t nbytes,
    off_t offset)
{
	// example: 40% means 40 out of 100 submitted blocks will be reads, the remaining 60 are writes

	/* note: keep in mind that this also needs to work with lots of small files, so percentage needs
		to work between different files. (numIOPSSubmitted ensures that below; numIOPSDone would not
		work for this because aio would not inc counter directly on submission.) */

	const int fd = (*fileHandles.fdVecPtr)[fileHandleIdx];

	ssize_t ioRes;

	// note: workerRank is used to have skew between different worker threads
	if( ( (workerRank + numIOPSSubmitted) % 100) >= progArgs->getRWMixReadPercent() )
	{
		OPLOG_PRE_OP("pwrite", std::to_string(fd), offset, nbytes);

		ioRes = pwrite(fd, buf, nbytes, offset);

		OPLOG_POST_OP("pwrite", std::to_string(fd), offset, nbytes, ioRes <= 0);
	}
	else
	{
		OPLOG_PRE_OP("pread", std::to_string(fd), offset, nbytes);

		ioRes = pread(fd, buf, nbytes, offset);

		OPLOG_POST_OP("pread", std::to_string(fd), offset, nbytes, ioRes == -1);
	}

	return ioRes;
}

/**
 * Wrapper for positional sync cuFile read.
 *
 * @fd ignored, using cuFileHandleData.cfr_handle instead.
 * @buf ignored, using gpuIOBufVec[0] instead.
 */
ssize_t LocalWorker::cuFileReadWrapper(size_t fileHandleIdx, void* buf, size_t nbytes, off_t offset)
{
#ifndef CUFILE_SUPPORT
	throw WorkerException("cuFileReadWrapper called, but this executable was built without cuFile "
		"API support");
#else
	OPLOG_PRE_OP("cuFileRead", std::to_string(fileHandleIdx), offset, nbytes);

	ssize_t ioRes = cuFileRead(fileHandles.cuFileHandleDataPtrVec[fileHandleIdx]->cfr_handle,
		gpuIOBufVec[0], nbytes, offset, 0);

	OPLOG_POST_OP("cuFileRead", std::to_string(fileHandleIdx), offset, nbytes, ioRes == -1);

	return ioRes;
#endif
}

/**
 * Wrapper for positional sync cuFile write.
 *
 * @fd ignored, using cuFileHandleData.cfr_handle instead.
 * @buf ignored, using gpuIOBufVec[0] instead.
 */
ssize_t LocalWorker::cuFileWriteWrapper(size_t fileHandleIdx, void* buf, size_t nbytes, off_t offset)
{
#ifndef CUFILE_SUPPORT
	throw WorkerException("cuFileWriteWrapper called, but this executable was built without cuFile "
		"API support");
#else
	OPLOG_PRE_OP("cuFileWrite", std::to_string(fileHandleIdx), offset, nbytes);

	ssize_t ioRes = cuFileWrite(fileHandles.cuFileHandleDataPtrVec[fileHandleIdx]->cfr_handle,
		gpuIOBufVec[0], nbytes, offset, 0);

	OPLOG_POST_OP("cuFileWrite", std::to_string(fileHandleIdx), offset, nbytes, ioRes <= 0);

	return ioRes;
#endif
}

/**
 * Wrapper for positional sync cuFile write followed by an immediate cuFile read of the same block.
 */
ssize_t LocalWorker::cuFileWriteAndReadWrapper(size_t fileHandleIdx, void* buf, size_t nbytes,
	off_t offset)
{
#ifndef CUFILE_SUPPORT
	throw WorkerException("cuFileWriteAndReadWrapper called, but this executable was built without "
		"cuFile API support");
#else
	OPLOG_PRE_OP("cuFileWrite", std::to_string(fileHandleIdx), offset, nbytes);

	ssize_t writeRes =
		cuFileWrite(fileHandles.cuFileHandleDataPtrVec[fileHandleIdx]->cfr_handle,
			gpuIOBufVec[0], nbytes, offset, 0);

	OPLOG_POST_OP("cuFileWrite", std::to_string(fileHandleIdx), offset, nbytes, writeRes <= 0);

	IF_UNLIKELY(writeRes <= 0)
		return writeRes;

	OPLOG_PRE_OP("cuFileRead", std::to_string(fileHandleIdx), offset, nbytes);

	ssize_t readRes = cuFileRead(fileHandles.cuFileHandleDataPtrVec[fileHandleIdx]->cfr_handle,
		gpuIOBufVec[0], writeRes, offset, 0);

	OPLOG_POST_OP("cuFileRead", std::to_string(fileHandleIdx), offset, nbytes, readRes == -1);

	return readRes;
#endif
}

/**
 * Within a write phase, send user-defined pecentage of block reads for mixed r/w.
 *
 * Parameters and return value are similar to p{write,read}.
 */
ssize_t LocalWorker::cuFileRWMixWrapper(size_t fileHandleIdx, void* buf, size_t nbytes, off_t offset)
{
#ifndef CUFILE_SUPPORT
	throw WorkerException("cuFileRWMixWrapper called, but this executable was built without cuFile "
		"API support");
#else
	// example: 40% means 40 out of 100 submitted blocks will be reads, the remaining 60 are writes

	/* note: keep in mind that this also needs to work with lots of small files, so percentage needs
		to work between different files. (numIOPSSubmitted ensures that below; numIOPSDone would not
		work for this because aio would not inc counter directly on submission.) */

	ssize_t ioRes;

	// note: workerRank is used to have skew between different worker threads
	if( ( (workerRank + numIOPSSubmitted) % 100) >= progArgs->getRWMixReadPercent() )
	{
		OPLOG_PRE_OP("cuFileWrite", std::to_string(fileHandleIdx), offset, nbytes);

		ioRes = cuFileWrite(fileHandles.cuFileHandleDataPtrVec[fileHandleIdx]->cfr_handle,
			gpuIOBufVec[0], nbytes, offset, 0);

		OPLOG_POST_OP("cuFileWrite", std::to_string(fileHandleIdx), offset, nbytes, ioRes <= 0);
	}
	else
	{
		OPLOG_PRE_OP("cuFileRead", std::to_string(fileHandleIdx), offset, nbytes);

		ioRes = cuFileRead(fileHandles.cuFileHandleDataPtrVec[fileHandleIdx]->cfr_handle,
			gpuIOBufVec[0], nbytes, offset, 0);

		OPLOG_POST_OP("cuFileRead", std::to_string(fileHandleIdx), offset, nbytes, ioRes == -1);
}

	return ioRes;
#endif
}

/**
 * Wrapper for positional sync read for HDFS.
 */
ssize_t LocalWorker::hdfsReadWrapper(size_t fileHandleIdx, void* buf, size_t nbytes, off_t offset)
{
#ifndef HDFS_SUPPORT
	throw WorkerException(std::string(__func__) + "called, but built without hdfs support");
#else
	OPLOG_PRE_OP("hdfsPread", std::to_string(fileHandleIdx), offset, nbytes);

	ssize_t ioRes = hdfsPread(hdfsFSHandle, hdfsFileHandle, offset, buf, nbytes);

	OPLOG_POST_OP("hdfsPread", std::to_string(fileHandleIdx), offset, nbytes, ioRes == -1);

	return ioRes;
#endif // HDFS_SUPPORT
}

/**
 * Wrapper for positional sync write for HDFS.
 *
 * HDFS does not support seeking for writes, so offset is ignored.
 */
ssize_t LocalWorker::hdfsWriteWrapper(size_t fileHandleIdx, void* buf, size_t nbytes, off_t offset)
{
#ifndef HDFS_SUPPORT
	throw WorkerException(std::string(__func__) + "called, but built without hdfs support");
#else
	OPLOG_PRE_OP("hdfsWrite", std::to_string(fileHandleIdx), offset, nbytes);

	ssize_t ioRes = hdfsWrite(hdfsFSHandle, hdfsFileHandle, buf, nbytes);

	OPLOG_POST_OP("hdfsWrite", std::to_string(fileHandleIdx), offset, nbytes, ioRes <= 0);

	return ioRes;
#endif // HDFS_SUPPORT
}

/**
 * Wrapper for positional sync read via mmap.
 */
ssize_t LocalWorker::mmapReadWrapper(size_t fileHandleIdx, void* buf, size_t nbytes, off_t offset)
{
	memcpy(buf, &(fileHandles.mmapVec[fileHandleIdx][offset]), nbytes);

	return nbytes;
}

/**
 * Wrapper for positional sync write via mmap.
 */
ssize_t LocalWorker::mmapWriteWrapper(size_t fileHandleIdx, void* buf, size_t nbytes, off_t offset)
{
	memcpy(&(fileHandles.mmapVec[fileHandleIdx][offset]), buf, nbytes);

	return nbytes;
}

/**
 * Wrapper for positional spdk sync read. Only ever called via rwBlockSized(), i.e. at ioDepth==1
 * (see initPhaseFunctionPointers() ), so reusing spdkContext.ioContextVec[0]/completedVec here
 * never races with spdkAioBlockSized(), which is the ioDepth>1 counterpart on the same thread.
 */
ssize_t LocalWorker::spdkReadWrapper(size_t fileHandleIdx, void* buf, size_t nbytes, off_t offset)
{
#ifndef SPDK_SUPPORT
    throw WorkerException(std::string(__func__) + "called, but built without spdk support");
#else
    const int nsID = (*fileHandles.fdVecPtr)[fileHandleIdx];
    const uint64_t lba = offset / spdkContext.sectorSize;
    const uint32_t lbaCount = nbytes / spdkContext.sectorSize;

    SpdkNvmeClient::IoContext* ioCtx = spdkContext.ioContextVec[0].get();

    OPLOG_PRE_OP("spdkRead", spdkContext.spdkClient.getNamespaceName(nsID), offset, nbytes);

    int ioSubmitRes = spdkContext.spdkClient.read(ioCtx, nsID, lba, lbaCount, buf);

    IF_LIKELY(ioSubmitRes == 0)
    {
        do
        {
            spdkContext.completedVec.clear();
            spdkContext.spdkClient.waitForCompletions(spdkContext.completedVec,
                -1 /*block indefinitely*/);
        } while(!ioCtx->done.load(std::memory_order_acquire) );
    }

    const bool isIOSuccess = (ioSubmitRes == 0) && ioCtx->ioSuccess;

    OPLOG_POST_OP("spdkRead", spdkContext.spdkClient.getNamespaceName(nsID), offset, nbytes,
        !isIOSuccess);

    IF_UNLIKELY(!isIOSuccess)
    {
        errno = ioSubmitRes ? -ioSubmitRes : EIO; // spdk returns negative errno if submit fails
        return -1;
    }

    return nbytes;
#endif // SPDK_SUPPORT
}

/**
 * Wrapper for positional spdk sync write. See spdkReadWrapper() above for details.
 */
ssize_t LocalWorker::spdkWriteWrapper(size_t fileHandleIdx, void* buf, size_t nbytes, off_t offset)
{
#ifndef SPDK_SUPPORT
    throw WorkerException(std::string(__func__) + "called, but built without spdk support");
#else
    const int nsID = (*fileHandles.fdVecPtr)[fileHandleIdx];
    const uint64_t lba = offset / spdkContext.sectorSize;
    const uint32_t lbaCount = nbytes / spdkContext.sectorSize;

    SpdkNvmeClient::IoContext* ioCtx = spdkContext.ioContextVec[0].get();

    OPLOG_PRE_OP("spdkWrite", spdkContext.spdkClient.getNamespaceName(nsID), offset, nbytes);

    int ioSubmitRes = spdkContext.spdkClient.write(ioCtx, nsID, lba, lbaCount, buf);

    IF_LIKELY(ioSubmitRes == 0)
    {
        do
        {
            spdkContext.completedVec.clear();
            spdkContext.spdkClient.waitForCompletions(spdkContext.completedVec,
                -1 /*block indefinitely*/);
        } while(!ioCtx->done.load(std::memory_order_acquire) );
    }

    const bool isIOSuccess = (ioSubmitRes == 0) && ioCtx->ioSuccess;

    OPLOG_POST_OP("spdkWrite", spdkContext.spdkClient.getNamespaceName(nsID), offset, nbytes,
        !isIOSuccess);

    IF_UNLIKELY(!isIOSuccess)
    {
        errno = ioSubmitRes ? -ioSubmitRes : EIO; // spdk returns negative errno if submit fails
        return -1;
    }

    return nbytes;
#endif // SPDK_SUPPORT
}

/**
 * Wrapper for positional spdk sync write followed by an immediate read of the same block.
 */
ssize_t LocalWorker::spdkWriteAndReadWrapper(size_t fileHandleIdx, void* buf, size_t nbytes,
    off_t offset)
{
    ssize_t writeRes = spdkWriteWrapper(fileHandleIdx, buf, nbytes, offset);
    IF_UNLIKELY(writeRes <= 0)
        return writeRes;

    return spdkReadWrapper(fileHandleIdx, buf, nbytes, offset);
}


/**
 * Iterate over all directories to create or remove them.
 *
 * @throw WorkerException on error.
 */
void LocalWorker::dirModeIterateDirs()
{
	if(progArgs->getNumDirs() == 0)
		return; // nothing to do

	std::array<char, PATH_BUF_LEN> currentPath;
	const size_t numDirs = progArgs->getNumDirs();
	const IntVec& pathFDs = progArgs->getBenchPathFDs();
	const StringVec& pathVec = progArgs->getBenchPaths();
	const bool ignoreDelErrors = progArgs->getDoDirSharing() ?
		true : progArgs->getIgnoreDelErrors(); // in dir share mode, all workers mk/del all dirs
	const size_t workerDirRank = progArgs->getDoDirSharing() ? 0 : workerRank; /* for dir sharing,
		all workers use the dirs of worker rank 0 */

	// create rank dir inside each pathFD
	if(benchPhase == BenchPhase_CREATEDIRS)
	{
		for(unsigned pathFDsIndex = 0; pathFDsIndex < pathFDs.size(); pathFDsIndex++)
		{
			// create rank dir for current pathFD...

			checkInterruptionRequest();

			// generate path
			int printRes = snprintf(currentPath.data(), PATH_BUF_LEN, "r%zu", workerDirRank);
			IF_UNLIKELY(printRes >= PATH_BUF_LEN)
				throw WorkerException("mkdir path too long for static buffer. "
					"Buffer size: " + std::to_string(PATH_BUF_LEN) + "; "
					"workerRank: " + std::to_string(workerRank) );

		    OPLOG_PRE_OP("mkdirat", pathVec[pathFDsIndex] + "/" + currentPath.data(), 0, 0);

            int mkdirRes = mkdirat(pathFDs[pathFDsIndex], currentPath.data(), MKDIR_MODE);

		    OPLOG_POST_OP("mkdirat", pathVec[pathFDsIndex] + "/" + currentPath.data(), 0, 0,
		        mkdirRes == -1);

			if( (mkdirRes == -1) && (errno != EEXIST) )
				throw WorkerException(std::string("Rank directory creation failed. ") +
					"Path: " + pathVec[pathFDsIndex] + "/" + currentPath.data() + "; "
					"SysErr: " + strerror(errno) );
		}
	}

	// create user-specified number of directories round-robin across all given bench paths
	for(size_t dirIndex = 0; dirIndex < numDirs; dirIndex++)
	{
		checkInterruptionRequest();

		// generate current dir path
		int printRes = snprintf(currentPath.data(), PATH_BUF_LEN, "r%zu/d%zu",
			workerDirRank, dirIndex);
		IF_UNLIKELY(printRes >= PATH_BUF_LEN)
			throw WorkerException("mkdir path too long for static buffer. "
				"Buffer size: " + std::to_string(PATH_BUF_LEN) + "; "
				"dirIndex: " + std::to_string(dirIndex) + "; "
				"workerRank: " + std::to_string(workerRank) );

		unsigned pathFDsIndex = (workerRank + dirIndex) % pathFDs.size();

		std::chrono::steady_clock::time_point ioStartT = std::chrono::steady_clock::now();

		if(benchPhase == BenchPhase_CREATEDIRS)
		{ // create dir
            OPLOG_PRE_OP("mkdirat", pathVec[pathFDsIndex] + "/" + currentPath.data(), 0, 0);

            int mkdirRes = mkdirat(pathFDs[pathFDsIndex], currentPath.data(), MKDIR_MODE);

            OPLOG_POST_OP("mkdirat", pathVec[pathFDsIndex] + "/" + currentPath.data(), 0, 0,
                mkdirRes == -1);

			if( (mkdirRes == -1) && (errno != EEXIST) )
				throw WorkerException(std::string("Directory creation failed. ") +
					"Path: " + pathVec[pathFDsIndex] + "/" + currentPath.data() + "; "
					"SysErr: " + strerror(errno) );
		}

		if(benchPhase == BenchPhase_DELETEDIRS)
		{ // remove dir
		    OPLOG_PRE_OP("unlinkat", pathVec[pathFDsIndex] + "/" + currentPath.data(), 0, 0);

		    int rmdirRes = unlinkat(pathFDs[pathFDsIndex], currentPath.data(), AT_REMOVEDIR);

            OPLOG_POST_OP("unlinkat", pathVec[pathFDsIndex] + "/" + currentPath.data(), 0, 0,
                rmdirRes == -1);

			if( (rmdirRes == -1) && ( (errno != ENOENT) || !ignoreDelErrors) )
				throw WorkerException(std::string("Directory deletion failed. ") +
					"Path: " + pathVec[pathFDsIndex] + "/" + currentPath.data() + "; "
					"SysErr: " + strerror(errno) );
		}

		// calc entry operations latency
		std::chrono::steady_clock::time_point ioEndT = std::chrono::steady_clock::now();
		std::chrono::microseconds ioElapsedMicroSec =
			std::chrono::duration_cast<std::chrono::microseconds>
			(ioEndT - ioStartT);

		entriesLatHisto.addLatency(ioElapsedMicroSec.count() );

		atomicLiveOps.numEntriesDone++;
	} // end of for loop


	// delete rank dir inside each pathFD
	if(benchPhase == BenchPhase_DELETEDIRS)
	{
		for(unsigned pathFDsIndex = 0; pathFDsIndex < pathFDs.size(); pathFDsIndex++)
		{
			// delete rank dir for current pathFD...

			checkInterruptionRequest();

			// generate path
			int printRes = snprintf(currentPath.data(), PATH_BUF_LEN, "r%zu", workerDirRank);
			IF_UNLIKELY(printRes >= PATH_BUF_LEN)
				throw WorkerException("mkdir path too long for static buffer. "
					"Buffer size: " + std::to_string(PATH_BUF_LEN) + "; "
					"workerRank: " + std::to_string(workerRank) );

            OPLOG_PRE_OP("unlinkat", pathVec[pathFDsIndex] + "/" + currentPath.data(), 0, 0);

			int rmdirRes = unlinkat(pathFDs[pathFDsIndex], currentPath.data(), AT_REMOVEDIR);

            OPLOG_POST_OP("unlinkat", pathVec[pathFDsIndex] + "/" + currentPath.data(), 0, 0,
                rmdirRes == -1);

			if( (rmdirRes == -1) && ( (errno != ENOENT) || !ignoreDelErrors) )
				throw WorkerException(std::string("Directory deletion failed. ") +
					"Path: " + pathVec[pathFDsIndex] + "/" + currentPath.data() + "; "
					"SysErr: " + strerror(errno) );
		}
	}

}

/**
 * In directory mode with custom tree, for creation we iterate over a fair share per worker and
 * and create parents as needed (because there is no communication between workers that
 * guarantees that all parents have been created). For deletion, first worker of each instance
 * iterates over all dirs remove them (because we have no way to guarantee otherwise that all
 * subdirs under a certain dir have been deleted).
 *
 * Note: With a custom tree, multiple benchmark paths are not supported (because otherwise we
 * 	can't ensure in file creation phase that the matching parent dir has been created for the
 * 	current bench path).
 *
 * @throw WorkerException on error.
 */
void LocalWorker::dirModeIterateCustomDirs()
{
	const int benchPathFD = progArgs->getBenchPathFDs()[0];
	const std::string benchPathStr = progArgs->getBenchPaths()[0];
	const bool ignoreDelErrors = true; // in custom tree mode, all workers mk/del all dirs
	const PathList& customTreePaths = (benchPhase == BenchPhase_DELETEDIRS) ?
		progArgs->getCustomTreeDirs().getPaths() : customTreeDirs.getPaths();
	const bool reverseOrder = (benchPhase == BenchPhase_DELETEDIRS);
	const size_t localWorkerRank = workerRank - progArgs->getRankOffset();
	const bool thisWorkerDoesDelDirs = progArgs->getIsServicePathShared() ?
		(workerRank == 0) : (localWorkerRank == 0);

	IF_UNLIKELY(customTreePaths.empty() )
		return; // nothing to do here

	if( (benchPhase == BenchPhase_DELETEDIRS) && !thisWorkerDoesDelDirs)
	{ // only first worker iterates over all dirs for delete, others do nothing
		workerGotPhaseWork = false;
		return;
	}

	/* note on reverse: dirs are ordered by path length, so that parent dirs come before their
		subdirs. for tree removal, we need to remove subdirs first, hence the reverse order */

	PathList::const_iterator forwardIter = customTreePaths.cbegin();
	PathList::const_reverse_iterator reverseIter = customTreePaths.crbegin();

	// create user-specified directories round-robin across all given bench paths
	for( ; ; )
	{
		checkInterruptionRequest();

		const PathStoreElem& currentPathElem = reverseOrder ? *reverseIter : *forwardIter;

		std::chrono::steady_clock::time_point ioStartT = std::chrono::steady_clock::now();

		if(benchPhase == BenchPhase_CREATEDIRS)
		{ // create dir
			int mkdirRes = FileTk::mkdiratBottomUp(
				benchPathFD, currentPathElem.path.c_str(), MKDIR_MODE);

			if( (mkdirRes == -1) && (errno != EEXIST) )
				throw WorkerException(std::string("Directory creation failed. ") +
					"Path: " + benchPathStr + "/" + currentPathElem.path + "; "
					"SysErr: " + strerror(errno) );
		}

		if(benchPhase == BenchPhase_DELETEDIRS)
		{ // remove dir
            OPLOG_PRE_OP("unlinkat", benchPathStr + "/" + currentPathElem.path, 0, 0);

		    int rmdirRes = unlinkat(benchPathFD, currentPathElem.path.c_str(), AT_REMOVEDIR);

            OPLOG_POST_OP("unlinkat", benchPathStr + "/" + currentPathElem.path, 0, 0,
                rmdirRes == -1);

			if( (rmdirRes == -1) && ( (errno != ENOENT) || !ignoreDelErrors) )
				throw WorkerException(std::string("Directory deletion failed. ") +
					"Path: " + benchPathStr + "/" + currentPathElem.path + "; "
					"SysErr: " + strerror(errno) );
		}

		// calc entry operations latency. (for create, this includes open/rw/close.)
		std::chrono::steady_clock::time_point ioEndT = std::chrono::steady_clock::now();
		std::chrono::microseconds ioElapsedMicroSec =
			std::chrono::duration_cast<std::chrono::microseconds>
			(ioEndT - ioStartT);

		entriesLatHisto.addLatency(ioElapsedMicroSec.count() );

		atomicLiveOps.numEntriesDone++;

		// advance iterator and check for end of list
		if(reverseOrder)
		{
			reverseIter++;
			if(reverseIter == customTreePaths.crend() )
				break;
		}
		else
		{
			forwardIter++;
			if(forwardIter == customTreePaths.cend() )
				break;
		}
	} // end of for loop
}

/**
 * This is for directory mode. Iterate over all files to create/read/remove them.
 * By default, this uses a unique dir per worker and fills up each dir before moving on to the next.
 * If dir sharing is enabled, all workers will use dirs of rank 0.
 *
 * @throw WorkerException on error.
 */
void LocalWorker::dirModeIterateFiles()
{
	const bool haveSubdirs = (progArgs->getNumDirs() > 0);
	const size_t numDirs = haveSubdirs ? progArgs->getNumDirs() : 1; // set 1 to run dir loop once
	const size_t numFiles = progArgs->getNumFiles();
	const uint64_t fileSize = progArgs->getFileSize();
    const uint64_t fileOffset = progArgs->getFileOffset(); // user-defined min offset within file
	const IntVec& pathFDs = progArgs->getBenchPathFDs();
	const StringVec& pathVec = progArgs->getBenchPaths();
	const int openFlags = getDirModeOpenFlags(benchPhase);
	std::array<char, PATH_BUF_LEN> currentPath;
	const size_t workerDirRank = progArgs->getDoDirSharing() ? 0 : workerRank; /* for dir sharing,
		all workers use the dirs of worker rank 0 */
	const BenchPhase globalBenchPhase = workersSharedData->currentBenchPhase;
	const size_t localWorkerRank = workerRank - progArgs->getRankOffset();
	const bool isRWMixedReader = ( (globalBenchPhase == BenchPhase_CREATEFILES) &&
		(localWorkerRank < progArgs->getNumRWMixReadThreads() ) );
	const bool useMmap = progArgs->getUseMmap();
	const bool doStatInline = progArgs->getDoStatInline();

	int& fd = fileHandles.fdVec[0];
	CuFileHandleData& cuFileHandleData = fileHandles.cuFileHandleDataVec[0];

	// walk over each unique dir per worker

	for(size_t dirIndex = 0; dirIndex < numDirs; dirIndex++)
	{
		// occasional interruption check
		IF_UNLIKELY( (dirIndex % INTERRUPTION_CHECK_INTERVAL) == 0)
			checkInterruptionRequest();

		// fill up this dir with all files before moving on to the next dir

		for(size_t fileIndex = 0; fileIndex < numFiles; fileIndex++)
		{
			// occasional interruption check
			IF_UNLIKELY( (fileIndex % INTERRUPTION_CHECK_INTERVAL) == 0)
				checkInterruptionRequest();

			// generate current dir path
			int printRes;

			if(haveSubdirs)
				printRes = snprintf(currentPath.data(), PATH_BUF_LEN, "r%zu/d%zu/r%zu-f%zu",
					workerDirRank, dirIndex, workerRank, fileIndex);
			else
				printRes = snprintf(currentPath.data(), PATH_BUF_LEN, "r%zu-f%zu",
					workerRank, fileIndex);

			IF_UNLIKELY(printRes >= PATH_BUF_LEN)
				throw WorkerException("file path too long for static buffer. "
					"Buffer size: " + std::to_string(PATH_BUF_LEN) + "; "
					"workerRank: " + std::to_string(workerRank) + "; "
					"dirIndex: " + std::to_string(dirIndex) + "; "
					"fileIndex: " + std::to_string(fileIndex) );

			unsigned pathFDsIndex = (workerRank + dirIndex) % pathFDs.size();

			rwOffsetGen->reset(); // reset for next file

			std::chrono::steady_clock::time_point ioStartT = std::chrono::steady_clock::now();

			if( (benchPhase == BenchPhase_CREATEFILES) || (benchPhase == BenchPhase_READFILES) )
			{
				fd = dirModeOpenAndPrepFile(benchPhase, pathFDs, pathFDsIndex,
                    currentPath.data(), openFlags, fileOffset + fileSize, fileOffset);

				// try-block to ensure that fd is closed in case of exception
				try
				{
					((*this).*funcCuFileHandleReg)(fd, cuFileHandleData); // reg cuFile handle

					if(doStatInline)
					{ // inline stat (i.e. stat immediately after file open)
						struct stat statBuf;

					    OPLOG_PRE_OP("fstat", pathVec[pathFDsIndex] + "/" + currentPath.data(), 0,
					        0);

                        int statRes = fstat(fd, &statBuf);

					    OPLOG_POST_OP("fstat", pathVec[pathFDsIndex] + "/" + currentPath.data(), 0,
					        0, statRes == -1);

						IF_UNLIKELY(statRes == -1)
							throw WorkerException(std::string("Inline file stat failed. ") +
								"Path: " + pathVec[pathFDsIndex] + "/" + currentPath.data() + "; "
								"SysErr: " + strerror(errno) );
					}

					if(benchPhase == BenchPhase_CREATEFILES)
					{

						int64_t writeRes = ((*this).*funcRWBlockSized)();

						IF_UNLIKELY(writeRes == -1)
							throw WorkerException(std::string("File write failed. ") +
								( (progArgs->getUseDirectIO() && (errno == EINVAL) ) ?
									"Can be caused by directIO misalignment. " : "") +
								"Path: " + pathVec[pathFDsIndex] + "/" + currentPath.data() + "; "
								"SysErr: " + strerror(errno) );

						IF_UNLIKELY( (size_t)writeRes != fileSize)
							throw WorkerException(std::string("Unexpected short file write. ") +
								"Path: " + pathVec[pathFDsIndex] + "/" + currentPath.data() + "; "
								"Bytes written: " + std::to_string(writeRes) + "; "
								"Expected written: " + std::to_string(fileSize) + "; "
                                "Hint: Consider initial sequential write or adding "
                                    "\"--" ARG_TRUNCTOSIZE_LONG "\" to ensure full file size.");
					}

					if(benchPhase == BenchPhase_READFILES)
					{
						ssize_t readRes = ((*this).*funcRWBlockSized)();

						IF_UNLIKELY(readRes == -1)
							throw WorkerException(std::string("File read failed. ") +
								( (progArgs->getUseDirectIO() && (errno == EINVAL) ) ?
									"Can be caused by directIO misalignment. " : "") +
								"Path: " + pathVec[pathFDsIndex] + "/" + currentPath.data() + "; "
								"SysErr: " + strerror(errno) );

						IF_UNLIKELY( (size_t)readRes != fileSize)
							throw WorkerException(std::string("Unexpected short file read. ") +
								"Path: " + pathVec[pathFDsIndex] + "/" + currentPath.data() + "; "
								"Bytes read: " + std::to_string(readRes) + "; "
								"Expected read: " + std::to_string(fileSize) + "; "
                                "Hint: Consider initial sequential write or adding "
                                    "\"--" ARG_TRUNCTOSIZE_LONG "\" to ensure full file size.");
					}
				}
				catch(...)
				{
					// release memory mapping
					if(useMmap && (fileHandles.mmapVec[0] != MAP_FAILED) )
					{
                        munmap(fileHandles.mmapVec[0], fileOffset + fileSize);
						fileHandles.mmapVec[0] = (char*)MAP_FAILED;
					}

					((*this).*funcCuFileHandleDereg)(cuFileHandleData); // dereg cuFile handle

					OPLOG_PRE_OP("close", std::to_string(fd), 0, 0);

					int closeRes = close(fd);

					OPLOG_POST_OP("close", std::to_string(fd), 0, 0, closeRes == -1);

					throw;
				}

				// release memory mapping
				if(useMmap)
				{
                    int unmapRes = munmap(fileHandles.mmapVec[0], fileOffset + fileSize);

					IF_UNLIKELY(unmapRes == -1)
						ERRLOGGER(Log_NORMAL, "File memory unmap failed. " <<
							"Path: " + pathVec[pathFDsIndex] + "/" + currentPath.data() + "; "
							"SysErr: " << strerror(errno) << std::endl);

					fileHandles.mmapVec[0] = (char*)MAP_FAILED;
				}

				((*this).*funcCuFileHandleDereg)(cuFileHandleData); // deReg cuFile handle

                OPLOG_PRE_OP("close", std::to_string(fd), 0, 0);

				int closeRes = close(fd);

                OPLOG_POST_OP("close", std::to_string(fd), 0, 0, closeRes == -1);

				IF_UNLIKELY(closeRes == -1)
					throw WorkerException(std::string("File close failed. ") +
						"Path: " + pathVec[pathFDsIndex] + "/" + currentPath.data() + "; "
						"FD: " + std::to_string(fd) + "; "
						"SysErr: " + strerror(errno) );
			}

			if(benchPhase == BenchPhase_STATFILES)
			{
				struct stat statBuf;

				int statRes = fstatat(pathFDs[pathFDsIndex], currentPath.data(), &statBuf, 0);

				IF_UNLIKELY(statRes == -1)
					throw WorkerException(std::string("File stat failed. ") +
						"Path: " + pathVec[pathFDsIndex] + "/" + currentPath.data() + "; "
						"SysErr: " + strerror(errno) );
			}

			if(benchPhase == BenchPhase_DELETEFILES)
			{
                OPLOG_PRE_OP("unlinkat", pathVec[pathFDsIndex] + "/" + currentPath.data(), 0, 0);

                int unlinkRes = unlinkat(pathFDs[pathFDsIndex], currentPath.data(), 0);

	            OPLOG_POST_OP("unlinkat", pathVec[pathFDsIndex] + "/" + currentPath.data(), 0, 0,
	                unlinkRes == -1);

				if( (unlinkRes == -1) && (!progArgs->getIgnoreDelErrors() || (errno != ENOENT) ) )
					throw WorkerException(std::string("File delete failed. ") +
						"Path: " + pathVec[pathFDsIndex] + "/" + currentPath.data() + "; "
						"SysErr: " + strerror(errno) );
			}

			// calc entry operations latency. (for create, this includes open/rw/close.)
			std::chrono::steady_clock::time_point ioEndT = std::chrono::steady_clock::now();
			std::chrono::microseconds ioElapsedMicroSec =
				std::chrono::duration_cast<std::chrono::microseconds>
				(ioEndT - ioStartT);

			// inc special rwmix thread stats
			if(isRWMixedReader)
			{
				entriesLatHistoReadMix.addLatency(ioElapsedMicroSec.count() );
				atomicLiveOpsReadMix.numEntriesDone++;
			}
			else
			{
				entriesLatHisto.addLatency(ioElapsedMicroSec.count() );
				atomicLiveOps.numEntriesDone++;
			}

		} // end of files for loop
	} // end of dirs for loop

}

/**
 * This is for directory mode with custom files. Iterate over all files to create/read/remove them.
 * Each worker uses a subset of the files from the non-shared tree and parts of files from the
 * shared tree.
 *
 * Note: With a custom tree, multiple benchmark paths are not supported (because otherwise we
 * 	can't ensure in file creation phase that the matching parent dir has been created for the
 * 	current bench path).
 *
 * @throw WorkerException on error.
 */
void LocalWorker::dirModeIterateCustomFiles()
{
	const IntVec& benchPathFDs = progArgs->getBenchPathFDs();
	const unsigned benchPathFDIdx = 0; // multiple bench paths not supported with custom tree
	const int benchPathFD = progArgs->getBenchPathFDs()[0];
	const std::string benchPathStr = progArgs->getBenchPaths()[0];
	const int openFlags = getDirModeOpenFlags(benchPhase);
	const bool ignoreDelErrors = true; // shared files are unliked by all workers, so no errs
	const PathList& customTreePaths = customTreeFiles.getPaths();
	const BenchPhase globalBenchPhase = workersSharedData->currentBenchPhase;
	const size_t localWorkerRank = workerRank - progArgs->getRankOffset();
	const bool isRWMixedReader = ( (globalBenchPhase == BenchPhase_CREATEFILES) &&
		(localWorkerRank < progArgs->getNumRWMixReadThreads() ) );
	const bool useMmap = progArgs->getUseMmap();
	const bool doStatInline = progArgs->getDoStatInline();
    const uint64_t fileOffsetBase = progArgs->getFileOffset(); // user-defined min offset per file

	int& fd = fileHandles.fdVec[0];
	CuFileHandleData& cuFileHandleData = fileHandles.cuFileHandleDataVec[0];


	// check if this worker has anything to do in this round
    IF_UNLIKELY(customTreePaths.empty() )
    {
        LOGGER(Log_DEBUG, "got no work in this round. workerRank: " << workerRank << std::endl);
        workerGotPhaseWork = false;
        return;
    }


	unsigned short numFilesDone = 0; // just for occasional interruption check (so short is ok)

	// walk over custom tree part of this worker

	for(const PathStoreElem& currentPathElem : customTreePaths)
	{
		// occasional interruption check
		if( (numFilesDone % INTERRUPTION_CHECK_INTERVAL) == 0)
			checkInterruptionRequest();

		const char* currentPath = currentPathElem.path.c_str();

		std::chrono::steady_clock::time_point ioStartT = std::chrono::steady_clock::now();

		if( (benchPhase == BenchPhase_CREATEFILES) || (benchPhase == BenchPhase_READFILES) )
		{
			const uint64_t rangeLen = currentPathElem.rangeLen;
			const uint64_t fileOffset = currentPathElem.rangeStart;

			rwOffsetGen->reset(rangeLen, fileOffset);

			fd = dirModeOpenAndPrepFile(benchPhase, benchPathFDs, benchPathFDIdx,
                currentPathElem.path.c_str(), openFlags,
                currentPathElem.totalLen + fileOffsetBase, fileOffsetBase);

			// try-block to ensure that fd is closed in case of exception
			try
			{
				((*this).*funcCuFileHandleReg)(fd, cuFileHandleData); // reg cuFile handle

				if(doStatInline)
				{ // inline stat (i.e. stat immediately after file open)
					struct stat statBuf;

					int statRes = fstat(fd, &statBuf);

					IF_UNLIKELY(statRes == -1)
						throw WorkerException(std::string("File stat failed. ") +
							"Path: " + benchPathStr + "/" + currentPath + "; "
							"SysErr: " + strerror(errno) );
				}

				if(benchPhase == BenchPhase_CREATEFILES)
				{
					int64_t writeRes = ((*this).*funcRWBlockSized)();

					IF_UNLIKELY(writeRes == -1)
						throw WorkerException(std::string("File write failed. ") +
							( (progArgs->getUseDirectIO() && (errno == EINVAL) ) ?
								"Can be caused by directIO misalignment. " : "") +
							"Path: " + benchPathStr + "/" + currentPath + "; "
							"SysErr: " + strerror(errno) );

					IF_UNLIKELY( (size_t)writeRes != currentPathElem.rangeLen)
						throw WorkerException(std::string("Unexpected short file write. ") +
							"Path: " + benchPathStr + "/" + currentPath + "; "
							"Bytes written: " + std::to_string(writeRes) + "; "
							"Expected written: " + std::to_string(rangeLen) + "; "
                            "Hint: Consider initial sequential write or adding "
                                "\"--" ARG_TRUNCTOSIZE_LONG "\" to ensure full file size.");
				}

				if(benchPhase == BenchPhase_READFILES)
				{
					ssize_t readRes = ((*this).*funcRWBlockSized)();

					IF_UNLIKELY(readRes == -1)
						throw WorkerException(std::string("File read failed. ") +
							( (progArgs->getUseDirectIO() && (errno == EINVAL) ) ?
								"Can be caused by directIO misalignment. " : "") +
							"Path: " + benchPathStr + "/" + currentPath + "; "
							"SysErr: " + strerror(errno) );

					IF_UNLIKELY( (size_t)readRes != rangeLen)
						throw WorkerException(std::string("Unexpected short file read. ") +
							"Path: " + benchPathStr + "/" + currentPath + "; "
							"Bytes read: " + std::to_string(readRes) + "; "
							"Expected read: " + std::to_string(rangeLen) + "; "
                            "Hint: Consider initial sequential write or adding "
                                "\"--" ARG_TRUNCTOSIZE_LONG "\" to ensure full file size.");
				}
			}
			catch(...)
			{
				// release memory mapping
				if(useMmap && (fileHandles.mmapVec[0] != MAP_FAILED) )
				{
					munmap(fileHandles.mmapVec[0],
                        currentPathElem.totalLen + fileOffsetBase);
					fileHandles.mmapVec[0] = (char*)MAP_FAILED;
				}

				((*this).*funcCuFileHandleDereg)(cuFileHandleData); // dereg cuFile handle

                OPLOG_PRE_OP("close", std::to_string(fd), 0, 0);

                int closeRes = close(fd);

                OPLOG_POST_OP("close", std::to_string(fd), 0, 0, closeRes == -1);

                throw;
			}

			// release memory mapping
			if(useMmap)
			{
				int unmapRes = munmap(fileHandles.mmapVec[0],
                    currentPathElem.totalLen + fileOffsetBase);

				IF_UNLIKELY(unmapRes == -1)
					ERRLOGGER(Log_NORMAL, "File memory unmap failed. " <<
							"Path: " + benchPathStr + "/" + currentPath + "; "
						"SysErr: " << strerror(errno) << std::endl);

				fileHandles.mmapVec[0] = (char*)MAP_FAILED;
			}

			((*this).*funcCuFileHandleDereg)(cuFileHandleData); // deReg cuFile handle

            OPLOG_PRE_OP("close", std::to_string(fd), 0, 0);

            int closeRes = close(fd);

            OPLOG_POST_OP("close", std::to_string(fd), 0, 0, closeRes == -1);

			IF_UNLIKELY(closeRes == -1)
				throw WorkerException(std::string("File close failed. ") +
					"Path: " + benchPathStr + "/" + currentPath + "; "
					"FD: " + std::to_string(fd) + "; "
					"SysErr: " + strerror(errno) );
		}

		if(benchPhase == BenchPhase_STATFILES)
		{
			struct stat statBuf;

			int statRes = fstatat(benchPathFD, currentPath, &statBuf, 0);

			IF_UNLIKELY(statRes == -1)
				throw WorkerException(std::string("File stat failed. ") +
					"Path: " + benchPathStr + "/" + currentPath + "; "
					"SysErr: " + strerror(errno) );
		}

		if(benchPhase == BenchPhase_DELETEFILES)
		{
            OPLOG_PRE_OP("unlinkat", benchPathStr + "/" + currentPath, 0, 0);

            int unlinkRes = unlinkat(benchPathFD, currentPath, 0);

            OPLOG_POST_OP("unlinkat", benchPathStr + "/" + currentPath, 0, 0, unlinkRes == -1);

			if( (unlinkRes == -1) && (!ignoreDelErrors || (errno != ENOENT) ) )
				throw WorkerException(std::string("File delete failed. ") +
					"Path: " + benchPathStr + "/" + currentPath + "; "
					"SysErr: " + strerror(errno) );
		}

		// calc entry operations latency. (for create, this includes open/rw/close.)
		std::chrono::steady_clock::time_point ioEndT = std::chrono::steady_clock::now();
		std::chrono::microseconds ioElapsedMicroSec =
			std::chrono::duration_cast<std::chrono::microseconds>
			(ioEndT - ioStartT);

		// inc entry lat & num done count
		if(currentPathElem.totalLen == currentPathElem.rangeLen)
		{ // entry lat & done is only meaningful for fully processed entries
			if(isRWMixedReader)
			{
				entriesLatHistoReadMix.addLatency(ioElapsedMicroSec.count() );
				atomicLiveOpsReadMix.numEntriesDone++;
			}
			else
			{
				entriesLatHisto.addLatency(ioElapsedMicroSec.count() );
				atomicLiveOps.numEntriesDone++;
			}
		}

		numFilesDone++;

	} // end of tree elements for-loop

}

/**
 * This is for file/bdev mode. Send random I/Os round-robin to all given files across full file
 * range.
 *
 * @throw WorkerException on error.
 */
void LocalWorker::fileModeIterateFilesRand()
{
    // the total file range used for all workers

    const BenchPhase benchPhase = workersSharedData->currentBenchPhase;
    const bool isWritePhase = (benchPhase == BenchPhase_CREATEFILES);
    const size_t fileHandleVecSize = fileHandles.fdVecPtr->size();
    const size_t blockSize = progArgs->getBlockSize();
    const BlockSizeMix& blockSizeMix = progArgs->getBlockSizeMix();
    const uint64_t fileSize = progArgs->getFileSize();
    const uint64_t numBlocksPerFile = fileSize / blockSize;
    const uint64_t numBlocksTotal = numBlocksPerFile * fileHandleVecSize;
    const size_t numDataSetThreads = progArgs->getNumDataSetThreads();
    const uint64_t randomAmount = progArgs->getRandomAmount() / numDataSetThreads;

    // treat all files as virtual file-serial range of blocks and instantiate selected offset gen...

    const uint64_t rangeLen = blockSize * (numBlocksTotal / numDataSetThreads);
    const uint64_t rangeOffset = workerRank * blockSize * (numBlocksTotal / numDataSetThreads);

    if(progArgs->getUseStridedAccess() )
        rwOffsetGen = std::make_unique<OffsetGenStrided>(rangeLen, blockSize * workerRank,
            blockSize, numDataSetThreads);
    else
    if(progArgs->getUseRandomUnaligned() )
        rwOffsetGen = std::make_unique<OffsetGenRandom>(randomAmount, *randOffsetAlgo,
            rangeLen, rangeOffset, blockSizeMix);
    else
    if(!progArgs->getRandOffsetAlgo().empty() || !isWritePhase ||
        progArgs->isBlockSizeMixDefined() ) // (full coverage algo doesn't support block size mix)
        rwOffsetGen = std::make_unique<OffsetGenRandomAligned>(randomAmount, *randOffsetAlgo,
            rangeLen, rangeOffset, blockSizeMix);
    else
    { // random aligned writes without explicit algo selection => use full coverage algo
        rwOffsetGen = std::make_unique<OffsetGenRandomAlignedFullCoverageV2>(
            randomAmount, rangeLen, rangeOffset, blockSize);
    }

    // let funcRWBlockSized do the actual work of reading/writing all blocks...

    if(benchPhase == BenchPhase_CREATEFILES)
    {
        ssize_t writeRes = ((*this).*funcRWBlockSized)();

        IF_UNLIKELY(writeRes == -1)
            throw WorkerException(std::string("File write failed. ") +
                ( (progArgs->getUseDirectIO() && (errno == EINVAL) ) ?
                    "Can be caused by directIO misalignment. " : "") +
                fileModeLogPathFromFileHandlesErr() +
                "SysErr: " + strerror(errno) );

        IF_UNLIKELY( (size_t)writeRes != rwOffsetGen->getNumBytesTotal() )
            throw WorkerException(std::string("Unexpected short file write. ") +
                fileModeLogPathFromFileHandlesErr() +
                "Bytes written: " + std::to_string(writeRes) + "; "
                "Expected written: " + std::to_string(rwOffsetGen->getNumBytesTotal() ) + "; "
                "Hint: Consider initial sequential write or adding "
                    "\"--" ARG_TRUNCTOSIZE_LONG "\" to ensure full file size.");
    }

    if(benchPhase == BenchPhase_READFILES)
    {
        ssize_t readRes = ((*this).*funcRWBlockSized)();

        IF_UNLIKELY(readRes == -1)
            throw WorkerException(std::string("File read failed. ") +
                ( (progArgs->getUseDirectIO() && (errno == EINVAL) ) ?
                    "Can be caused by directIO misalignment. " : "") +
                fileModeLogPathFromFileHandlesErr() +
                "SysErr: " + strerror(errno) );

        IF_UNLIKELY( (size_t)readRes != rwOffsetGen->getNumBytesTotal() )
            throw WorkerException(std::string("Unexpected short file read. ") +
                fileModeLogPathFromFileHandlesErr() +
                "Bytes read: " + std::to_string(readRes) + "; "
                "Expected read: " + std::to_string(rwOffsetGen->getNumBytesTotal() ) + "; "
                "Hint: Consider initial sequential write or adding "
                    "\"--" ARG_TRUNCTOSIZE_LONG "\" to ensure full file size.");
    }

}

/**
 * This is for file/bdev mode. Iterate over all files to create/write or read them with sequential
 * I/O.
 *
 * @throw WorkerException on error.
 */
void LocalWorker::fileModeIterateFilesSeq()
{
    const BenchMode benchMode = progArgs->getBenchMode();
    CuFileHandleDataVec& cuFileHandleDataVec = fileHandles.threadCuFileHandleDataVec.empty() ?
        progArgs->getCuFileHandleDataVec() : fileHandles.threadCuFileHandleDataVec;
    const uint64_t fileSize = progArgs->getFileSize();
    const uint64_t fileOffset = progArgs->getFileOffset(); // user-defined min offset within file
    const size_t blockSize = progArgs->getBlockSize();
    const size_t numThreads = progArgs->getNumDataSetThreads();
    const bool useMmap = progArgs->getUseMmap();
    const bool useCuFile = progArgs->getUseCuFile();

    const IntVec& pathFDs = [&]() -> const IntVec&
    {
        if(benchMode == BenchMode_SPDK)
            return progArgs->getBenchPathSpdkNsIds();

        if(fileHandles.threadFDVec.empty() )
            return progArgs->getBenchPathFDs();

        return fileHandles.threadFDVec;
    }();

    const size_t numFiles = pathFDs.size();

	const uint64_t numBlocksPerFile = (fileSize / blockSize) +
		( (fileSize % blockSize) ? 1 : 0);

	const uint64_t numBlocksTotal = numBlocksPerFile * numFiles; // total for all files
	const uint64_t standardWorkerNumBlocks = numBlocksTotal / numThreads;

	// note: last worker might need to write up to "numThreads-1" more blocks than the others
	uint64_t thisWorkerNumBlocks = standardWorkerNumBlocks;
	if( (workerRank == (numThreads-1) ) && (numBlocksTotal % numThreads) )
		thisWorkerNumBlocks = numBlocksTotal - (standardWorkerNumBlocks * (numThreads-1) );

	// indices of start and end block. (end block is not inclusive.)
	uint64_t startBlock = workerRank * standardWorkerNumBlocks;
	uint64_t endBlock = startBlock + thisWorkerNumBlocks;

	LOGGER(Log_DEBUG, "workerRank: " << workerRank << "; "
		"numFiles: " << numFiles << "; "
		"dataSetThreads: " << numThreads << "; "
		"blocksTotal: " << numBlocksTotal << "; "
		"blocksPerFile: " << numBlocksPerFile << "; "
		"standardWorkerNumBlocks: " << standardWorkerNumBlocks << "; "
		"thisWorkerNumBlocks: " << thisWorkerNumBlocks << "; "
		"startBlock: " << startBlock << "; "
		"endBlock: " << endBlock << "; " << std::endl);


	// check if worker has anything to do in this round
	IF_UNLIKELY(startBlock >= endBlock)
	{
        LOGGER(Log_DEBUG, "got no work in this round. workerRank: " << workerRank << std::endl);

	    workerGotPhaseWork = false;
	    return;
	}


	uint64_t currentBlockIdx = startBlock;

	// iterate over global block range for this worker thread
	// (note: "global block range" means that different blocks can refer to different files)
	while(currentBlockIdx < endBlock)
	{
        // find the file index and inner file block index for current global block index
        const uint64_t currentFileIndex = currentBlockIdx / numBlocksPerFile;
        fileHandles.fdVec[0] = pathFDs[currentFileIndex];
        fileHandles.cuFileHandleDataPtrVec[0] = !useCuFile ? nullptr :
            &(cuFileHandleDataVec[currentFileIndex]);
        if(!fileHandles.journalPtrVec.empty() ) // alias the journal of the current file, like above
            fileHandles.journalPtrVec[0] = progArgs->getJournalForTarget(currentFileIndex);

        const uint64_t currentBlockInFile = currentBlockIdx % numBlocksPerFile;
        const uint64_t currentIOStart = currentBlockInFile * blockSize;

        // calc byte offset in file and range length
        const uint64_t remainingWorkerLen = (endBlock - currentBlockIdx) * blockSize;
        const uint64_t remainingFileLen = fileSize - (currentBlockInFile * blockSize);
        const uint64_t currentIOLen = std::min(remainingWorkerLen, remainingFileLen);

		// prep offset generator for current file range
		rwOffsetGen->reset(currentIOLen, currentIOStart);

		FileTk::fadvise<WorkerException>(fileHandles.fdVec[0], progArgs->getFadviseFlags(),
			progArgs->getBenchPaths()[currentFileIndex].c_str() );

		// prep memory mapping
		if(useMmap)
		{
			int protectionMode = (benchPhase == BenchPhase_READFILES) ?
				PROT_READ : (PROT_WRITE | PROT_READ);

			fileHandles.mmapVec[0] = (char*)FileTk::mmapAndMadvise<WorkerException>(
                fileOffset + fileSize, protectionMode, MAP_SHARED, fileHandles.fdVec[0],
				progArgs->getMadviseFlags(), progArgs->getBenchPaths()[currentFileIndex].c_str() );
		}

		// (try-block for munmap on error)
		try
		{
			// write/read our range of this file

			if(benchPhase == BenchPhase_CREATEFILES)
			{
				ssize_t writeRes = ((*this).*funcRWBlockSized)();

				IF_UNLIKELY(writeRes == -1)
					throw WorkerException(std::string("File write failed. ") +
						( (progArgs->getUseDirectIO() && (errno == EINVAL) ) ?
							"Can be caused by directIO misalignment. " : "") +
						"Path: " + progArgs->getBenchPaths()[currentFileIndex] + "; "
						"SysErr: " + strerror(errno) );

				IF_UNLIKELY( (size_t)writeRes != currentIOLen)
					throw WorkerException(std::string("Unexpected short file write. ") +
						"Path: " + progArgs->getBenchPaths()[currentFileIndex] + "; "
						"Bytes written: " + std::to_string(writeRes) + "; "
						"Expected written: " + std::to_string(currentIOLen) + "; "
                        "Hint: Consider initial sequential write or adding "
                            "\"--" ARG_TRUNCTOSIZE_LONG "\" to ensure full file size.");
			}

			if(benchPhase == BenchPhase_READFILES)
			{
				ssize_t readRes = ((*this).*funcRWBlockSized)();

				IF_UNLIKELY(readRes == -1)
					throw WorkerException(std::string("File read failed. ") +
						( (progArgs->getUseDirectIO() && (errno == EINVAL) ) ?
							"Can be caused by directIO misalignment. " : "") +
						"Path: " + progArgs->getBenchPaths()[currentFileIndex] + "; "
						"SysErr: " + strerror(errno) );

				IF_UNLIKELY( (size_t)readRes != currentIOLen)
					throw WorkerException(std::string("Unexpected short file read. ") +
						"Path: " + progArgs->getBenchPaths()[currentFileIndex] + "; "
						"Bytes read: " + std::to_string(readRes) + "; "
						"Expected read: " + std::to_string(currentIOLen) + "; "
                        "Hint: Consider initial sequential write or adding "
                            "\"--" ARG_TRUNCTOSIZE_LONG "\" to ensure full file size.");
			}

			// calc completed number of blocks to inc for next loop pass
			const uint64_t numBlocksDone = (currentIOLen / blockSize) +
				( (currentIOLen % blockSize) ? 1 : 0);

			LOGGER_DEBUG_BUILD("  w" << workerRank << " f" << currentFileIndex <<
				" b" << currentBlockInFile << " " <<
				currentIOStart << " - " << (currentIOStart+currentIOLen) << std::endl);

			currentBlockIdx += numBlocksDone;
		}
		catch(...)
		{
			// release memory mapping
			if(useMmap)
			{
                munmap(fileHandles.mmapVec[0], fileOffset + fileSize);
				fileHandles.mmapVec[0] = (char*)MAP_FAILED;
			}

			throw;
		}

		// release memory mapping
		if(useMmap)
		{
            int unmapRes = munmap(fileHandles.mmapVec[0], fileOffset + fileSize);

			IF_UNLIKELY(unmapRes == -1)
				ERRLOGGER(Log_NORMAL, "File memory unmap failed. " <<
					"Path: " + progArgs->getBenchPaths()[currentFileIndex] + "; "
					"SysErr: " << strerror(errno) << std::endl);

			fileHandles.mmapVec[0] = (char*)MAP_FAILED;
		}

	} // end of global blocks while-loop
}

/**
 * This is for file mode. Each thread tries to delete all given files.
 *
 * @throw WorkerException on error.
 */
void LocalWorker::fileModeDeleteFiles()
{
	const StringVec& benchPaths = progArgs->getBenchPaths();
	const size_t numFiles = benchPaths.size();

	// walk over all files and delete each of them
	// (note: each worker starts with a different file (based on workerRank) to spread the load)

	for(size_t fileIndex = 0; fileIndex < numFiles; fileIndex++)
	{
		// occasional interruption check
		if( (fileIndex % INTERRUPTION_CHECK_INTERVAL) == 0)
			checkInterruptionRequest();

		// delete current file

		const std::string& path =
			benchPaths[ (workerRank + fileIndex) % numFiles];

		int unlinkRes = unlink(path.c_str() );

		// (note: all threads try to delete all files, so ignore ENOENT)
		if( (unlinkRes == -1) && (errno != ENOENT) )
			throw WorkerException(std::string("File delete failed. ") +
				"Path: " + path + "; "
				"SysErr: " + strerror(errno) );

		atomicLiveOps.numEntriesDone++;

	} // end of files for loop

}

/**
 * This is for file/bdev mode. Retrieve path for log message based on fileHandles.errorFDVecIdx.
 *
 * The result will be "Path: /some/path; " based on progArgs benchPathsVec or "Path: unavailable; "
 * if errorFDVecIdx is not set ("==-1").
 */
std::string LocalWorker::fileModeLogPathFromFileHandlesErr()
{
	if(fileHandles.errorFDVecIdx == -1)
		return std::string("Path: unavailable; ");

	return "Path: " + progArgs->getBenchPaths()[fileHandles.errorFDVecIdx] + "; ";
}

/**
 * Return appropriate file open flags for the current benchmark phase in dir mode.
 *
 * @return flags for file open() in dir mode.
 */
int LocalWorker::getDirModeOpenFlags(BenchPhase benchPhase)
{
	int openFlags = 0;

	if(benchPhase == BenchPhase_CREATEFILES)
	{
		openFlags = O_CREAT | O_RDWR;

		if(progArgs->getDoTruncate() )
			openFlags |= O_TRUNC;
	}
	else
		openFlags = O_RDONLY;

#if !defined(__APPLE__)
    if(progArgs->getUseDirectIO() )
        openFlags |= O_DIRECT;
#endif // !apple

	return openFlags;
}

/**
 * Open file and prepare it for the actual IO, e.g. by truncating or preallocating it to
 * user-requested size.
 *
 * @benchPhase current benchmark phase.
 * @pathFDs progArgs->getBenchPathFDs.
 * @pathFDsIndex current index in pathFDs.
 * @relativePath path to open, relative to pathFD.
 * @openFlags as returned by getDirModeOpenFlags().
 * @fileLen total length of the file, i.e. min offset plus size in dir mode.
 * @preallocOffset offset at which to start preallocation of the range up to fileLen.
 * @return filedescriptor of open file.
 * @throw WorkerException on error, in which case file is guaranteed to be closed.
 */
int LocalWorker::dirModeOpenAndPrepFile(BenchPhase benchPhase, const IntVec& pathFDs,
        unsigned pathFDsIndex, const char* relativePath, int openFlags, uint64_t fileLen,
        uint64_t preallocOffset)
{
    const bool useMmap = progArgs->getUseMmap();
    const std::string currentPath = progArgs->getBenchPaths()[pathFDsIndex] + "/" + relativePath;

    OPLOG_PRE_OP("openat", currentPath, 0, 0);

    int fd = openat(pathFDs[pathFDsIndex], relativePath, openFlags, MKFILE_MODE);

    OPLOG_POST_OP("openat", currentPath, 0, 0, fd == -1);

    IF_UNLIKELY(fd == -1)
    {
        if( (errno == ENOENT) && (openFlags & O_CREAT) && !progArgs->getRunCreateDirsPhase() &&
            (progArgs->getNumDirs() || !progArgs->getCustomTreeDirs().getNumPaths() ) &&
            (progArgs->getBenchPathType() == BenchPathType_DIR) )
            throw WorkerException(std::string("File create/open failed. ") +
                "Did you forget to enable directory creation ('--" ARG_CREATEDIRS_LONG "')? "  +
                "Path: " + currentPath + "; "
                "SysErr: " + strerror(errno) );
        else
            throw WorkerException(std::string("File open failed. ") +
                "Path: " + currentPath + "; "
                "SysErr: " + strerror(errno) );
    }

    // try block to ensure file close on error
    try
    {
        if(benchPhase == BenchPhase_CREATEFILES)
        {
            if(progArgs->getDoTruncToSize() )
            {
                int truncRes = ftruncate(fd, fileLen);
                if(truncRes == -1)
                    throw WorkerException("Unable to set file size through ftruncate. "
                        "Path: " + currentPath + "; "
                        "Size: " + std::to_string(fileLen) + "; "
                        "SysErr: " + strerror(errno) );
            }

            if(progArgs->getDoPreallocFile() )
            {
                #if defined(__APPLE__)
                    throw WorkerException("posix_fallocate is not supported on macOS. "
                        "Path: " + currentPath + "; "
                        "Size: " + std::to_string(fileLen) );
                #else // linux
                    // (note: posix_fallocate does not set errno.)
                    int preallocRes = posix_fallocate(fd, preallocOffset,
                        fileLen - preallocOffset);
                    if(preallocRes != 0)
                        throw WorkerException(
                            "Unable to preallocate file size through posix_fallocate. "
                            "File: " + currentPath + "; "
                            "Offset: " + std::to_string(preallocOffset) + "; "
                            "Size: " + std::to_string(fileLen - preallocOffset) + "; "
                            "SysErr: " + strerror(preallocRes) );
                #endif // linux
            }
        }

        FileTk::fadvise<WorkerException>(fd, progArgs->getFadviseFlags(), currentPath.c_str() );

        // create memory mapping
        if(useMmap)
        {
            int protectionMode = (benchPhase == BenchPhase_READFILES) ?
                PROT_READ : (PROT_WRITE | PROT_READ);

            fileHandles.mmapVec[0] =  (char*)FileTk::mmapAndMadvise<WorkerException>(
                fileLen, protectionMode, MAP_SHARED, fd, progArgs->getMadviseFlags(),
                currentPath.c_str() );
        }

        return fd;
    }
    catch(WorkerException& e)
    {
        // release memory mapping
        if(useMmap)
        {
            munmap(fileHandles.mmapVec[0], fileLen);
            fileHandles.mmapVec[0] = (char*)MAP_FAILED;
        }

        OPLOG_PRE_OP("close", std::to_string(fd), 0, 0);

        int closeRes = close(fd);

        OPLOG_POST_OP("close", std::to_string(fd), 0, 0, closeRes == -1);

        if(closeRes == -1)
            ERRLOGGER(Log_NORMAL, "File close failed. " <<
                "Path: " << currentPath << "; " <<
                "FD: " << std::to_string(fd) << "; " <<
                "SysErr: " << strerror(errno) << std::endl);

        throw;
    }
}

/**
 * Iterate over all directories in HDFS dir mode to create or remove them.
 *
 * @throw WorkerException on error.
 */
void LocalWorker::hdfsDirModeIterateDirs()
{
#ifndef HDFS_SUPPORT
	throw WorkerException(std::string(__func__) + "called, but built without hdfs support");
#else

	if(progArgs->getNumDirs() == 0)
		return; // nothing to do

	std::array<char, PATH_BUF_LEN> currentPath;
	const size_t numDirs = progArgs->getNumDirs();
	const StringVec& pathVec = progArgs->getBenchPaths();
	const bool ignoreDelErrors = progArgs->getDoDirSharing() ?
		true : progArgs->getIgnoreDelErrors(); // in dir share mode, all workers mk/del all dirs
	const size_t workerDirRank = progArgs->getDoDirSharing() ? 0 : workerRank; /* for dir sharing,
		all workers use the dirs of worker rank 0 */

	// create rank dir inside each pathFD
	if(benchPhase == BenchPhase_CREATEDIRS)
	{
		for(unsigned pathFDsIndex = 0; pathFDsIndex < pathVec.size(); pathFDsIndex++)
		{
			// create rank dir for current pathFD...

			checkInterruptionRequest();

			// generate path
			int printRes = snprintf(currentPath.data(), PATH_BUF_LEN, "r%zu", workerDirRank);
			IF_UNLIKELY(printRes >= PATH_BUF_LEN)
				throw WorkerException("mkdir path too long for static buffer. "
					"Buffer size: " + std::to_string(PATH_BUF_LEN) + "; "
					"workerRank: " + std::to_string(workerRank) );

			std::string fullPath = pathVec[pathFDsIndex] + "/" + currentPath.data();

			int mkdirRes = hdfsCreateDirectory(hdfsFSHandle, fullPath.c_str() );

			if( (mkdirRes == -1) && (errno != EEXIST) )
				throw WorkerException(std::string("Rank directory creation failed. ") +
					"Path: " + pathVec[pathFDsIndex] + "/" + currentPath.data() );
		}
	}

	// create user-specified number of directories round-robin across all given bench paths
	for(size_t dirIndex = 0; dirIndex < numDirs; dirIndex++)
	{
		checkInterruptionRequest();

		// generate current dir path
		int printRes = snprintf(currentPath.data(), PATH_BUF_LEN, "r%zu/d%zu",
			workerDirRank, dirIndex);
		IF_UNLIKELY(printRes >= PATH_BUF_LEN)
			throw WorkerException("mkdir path too long for static buffer. "
				"Buffer size: " + std::to_string(PATH_BUF_LEN) + "; "
				"dirIndex: " + std::to_string(dirIndex) + "; "
				"workerRank: " + std::to_string(workerRank) );

		unsigned pathFDsIndex = (workerRank + dirIndex) % pathVec.size();

		std::string fullPath = pathVec[pathFDsIndex] + "/" + currentPath.data();

		std::chrono::steady_clock::time_point ioStartT = std::chrono::steady_clock::now();

		if(benchPhase == BenchPhase_CREATEDIRS)
		{ // create dir
			int mkdirRes = hdfsCreateDirectory(hdfsFSHandle, fullPath.c_str() );

			if( (mkdirRes == -1) && (errno != EEXIST) )
				throw WorkerException(std::string("Directory creation failed. ") +
					"Path: " + pathVec[pathFDsIndex] + "/" + currentPath.data() );
		}

		if(benchPhase == BenchPhase_DELETEDIRS)
		{ // remove dir
			int rmdirRes = hdfsDelete(hdfsFSHandle, fullPath.c_str(), 0 /* recursive */ );

			if( (rmdirRes == -1) && !ignoreDelErrors) // hdfs doesn't have a meaningful error code
				throw WorkerException(std::string("Directory deletion failed. ") +
					"Path: " + pathVec[pathFDsIndex] + "/" + currentPath.data() );
		}

		// calc entry operations latency
		std::chrono::steady_clock::time_point ioEndT = std::chrono::steady_clock::now();
		std::chrono::microseconds ioElapsedMicroSec =
			std::chrono::duration_cast<std::chrono::microseconds>
			(ioEndT - ioStartT);

		entriesLatHisto.addLatency(ioElapsedMicroSec.count() );

		atomicLiveOps.numEntriesDone++;
	} // end of for loop


	// delete rank dir inside each pathFD
	if(benchPhase == BenchPhase_DELETEDIRS)
	{
		for(unsigned pathFDsIndex = 0; pathFDsIndex < pathVec.size(); pathFDsIndex++)
		{
			// delete rank dir for current pathFD...

			checkInterruptionRequest();

			// generate path
			int printRes = snprintf(currentPath.data(), PATH_BUF_LEN, "r%zu", workerDirRank);
			IF_UNLIKELY(printRes >= PATH_BUF_LEN)
				throw WorkerException("mkdir path too long for static buffer. "
					"Buffer size: " + std::to_string(PATH_BUF_LEN) + "; "
					"workerRank: " + std::to_string(workerRank) );

			std::string fullPath = pathVec[pathFDsIndex] + "/" + currentPath.data();

			int rmdirRes = hdfsDelete(hdfsFSHandle, fullPath.c_str(), 0 /* recursive */ );

			if( (rmdirRes == -1) && !ignoreDelErrors) // hdfs doesn't have a meaningful error code
				throw WorkerException(std::string("Directory deletion failed. ") +
					"Path: " + pathVec[pathFDsIndex] + "/" + currentPath.data() );
		}
	}

#endif // HDFS_SUPPORT
}

/**
 * This is for HDFS directory mode. Iterate over all files to create/read/remove them.
 * By default, this uses a unique dir per worker and fills up each dir before moving on to the next.
 * If dir sharing is enabled, all workers will use dirs of rank 0.
 *
 * @throw WorkerException on error.
 */
void LocalWorker::hdfsDirModeIterateFiles()
{
#ifndef HDFS_SUPPORT
	throw WorkerException(std::string(__func__) + "called, but built without hdfs support");
#else

	const bool haveSubdirs = (progArgs->getNumDirs() > 0);
	const size_t numDirs = haveSubdirs ? progArgs->getNumDirs() : 1; // set 1 to run dir loop once
	const size_t numFiles = progArgs->getNumFiles();
	const uint64_t fileSize = progArgs->getFileSize();
	const StringVec& pathVec = progArgs->getBenchPaths();
	const int openFlags = (benchPhase == BenchPhase_CREATEFILES) ? O_WRONLY : O_RDONLY;
	std::array<char, PATH_BUF_LEN> currentPath;
	const size_t workerDirRank = progArgs->getDoDirSharing() ? 0 : workerRank; /* for dir sharing,
		all workers use the dirs of worker rank 0 */
	const BenchPhase globalBenchPhase = workersSharedData->currentBenchPhase;
	const size_t localWorkerRank = workerRank - progArgs->getRankOffset();
	const bool isRWMixedReader = ( (globalBenchPhase == BenchPhase_CREATEFILES) &&
		(localWorkerRank < progArgs->getNumRWMixReadThreads() ) );

	// walk over each unique dir per worker

	for(size_t dirIndex = 0; dirIndex < numDirs; dirIndex++)
	{
		// occasional interruption check
		IF_UNLIKELY( (dirIndex % INTERRUPTION_CHECK_INTERVAL) == 0)
			checkInterruptionRequest();

		// fill up this dir with all files before moving on to the next dir

		for(size_t fileIndex = 0; fileIndex < numFiles; fileIndex++)
		{
			// occasional interruption check
			IF_UNLIKELY( (fileIndex % INTERRUPTION_CHECK_INTERVAL) == 0)
				checkInterruptionRequest();

			// generate current dir path
			int printRes;

			if(haveSubdirs)
				printRes = snprintf(currentPath.data(), PATH_BUF_LEN, "r%zu/d%zu/r%zu-f%zu",
					workerDirRank, dirIndex, workerRank, fileIndex);
			else
				printRes = snprintf(currentPath.data(), PATH_BUF_LEN, "r%zu-f%zu",
					workerRank, fileIndex);

			IF_UNLIKELY(printRes >= PATH_BUF_LEN)
				throw WorkerException("file path too long for static buffer. "
					"Buffer size: " + std::to_string(PATH_BUF_LEN) + "; "
					"workerRank: " + std::to_string(workerRank) + "; "
					"dirIndex: " + std::to_string(dirIndex) + "; "
					"fileIndex: " + std::to_string(fileIndex) );

			unsigned pathFDsIndex = (workerRank + dirIndex) % pathVec.size();

			std::string fullPath = pathVec[pathFDsIndex] + "/" + currentPath.data();

			rwOffsetGen->reset(); // reset for next file

			std::chrono::steady_clock::time_point ioStartT = std::chrono::steady_clock::now();

			if( (benchPhase == BenchPhase_CREATEFILES) || (benchPhase == BenchPhase_READFILES) )
			{
				hdfsFileHandle = hdfsOpenFile(hdfsFSHandle, fullPath.c_str(), openFlags, 0, 0, 0);

				IF_UNLIKELY(hdfsFileHandle == NULL) // hdfs doesn't provide a meaningful error code
					throw WorkerException(std::string("File open failed. ") +
						"Path: " + fullPath);

				if(progArgs->getUseDirectIO() )
					hdfsUnbufferFile(hdfsFileHandle);

				// try-block to ensure that fd is closed in case of exception
				try
				{
					if(benchPhase == BenchPhase_CREATEFILES)
					{
						int64_t writeRes = ((*this).*funcRWBlockSized)();

						IF_UNLIKELY(writeRes == -1)
							throw WorkerException(std::string("File write failed. ") +
								( (progArgs->getUseDirectIO() && (errno == EINVAL) ) ?
									"Can be caused by directIO misalignment. " : "") +
								"Path: " + pathVec[pathFDsIndex] + "/" + currentPath.data() + "; "
								"SysErr: " + strerror(errno) );

						IF_UNLIKELY( (size_t)writeRes != fileSize)
							throw WorkerException(std::string("Unexpected short file write. ") +
								"Path: " + pathVec[pathFDsIndex] + "/" + currentPath.data() + "; "
								"Bytes written: " + std::to_string(writeRes) + "; "
								"Expected written: " + std::to_string(fileSize) );
					}

					if(benchPhase == BenchPhase_READFILES)
					{
						ssize_t readRes = ((*this).*funcRWBlockSized)();

						IF_UNLIKELY(readRes == -1)
							throw WorkerException(std::string("File read failed. ") +
								( (progArgs->getUseDirectIO() && (errno == EINVAL) ) ?
									"Can be caused by directIO misalignment. " : "") +
								"Path: " + pathVec[pathFDsIndex] + "/" + currentPath.data() + "; "
								"SysErr: " + strerror(errno) );

						IF_UNLIKELY( (size_t)readRes != fileSize)
							throw WorkerException(std::string("Unexpected short file read. ") +
								"Path: " + pathVec[pathFDsIndex] + "/" + currentPath.data() + "; "
								"Bytes read: " + std::to_string(readRes) + "; "
								"Expected read: " + std::to_string(fileSize) );
					}
				}
				catch(...)
				{ // ensure that we don't leak an open file fd
					hdfsCloseFile(hdfsFSHandle, hdfsFileHandle);
					throw;
				}

				int closeRes = hdfsCloseFile(hdfsFSHandle, hdfsFileHandle);

				IF_UNLIKELY(closeRes == -1) // hdfs doesn't provide a meaningful error code
					throw WorkerException(std::string("File close failed. ") +
						"Path: " + fullPath);
			}

			if(benchPhase == BenchPhase_STATFILES)
			{
				hdfsFileInfo* fileInfo = hdfsGetPathInfo(hdfsFSHandle, fullPath.c_str() );

				if(fileInfo == NULL) // hdfs doesn't provide a meaningful error code
					throw WorkerException(std::string("File stat failed. ") +
						"Path: " + pathVec[pathFDsIndex] + "/" + currentPath.data() );
			}

			if(benchPhase == BenchPhase_DELETEFILES)
			{
				int unlinkRes =  hdfsDelete(hdfsFSHandle, fullPath.c_str(), 0 /* recursive */ );

				if( (unlinkRes == -1) && !progArgs->getIgnoreDelErrors() )
					throw WorkerException(std::string("File delete failed. ") +
						"Path: " + pathVec[pathFDsIndex] + "/" + currentPath.data() );
			}

			// calc entry operations latency. (for create, this includes open/rw/close.)
			std::chrono::steady_clock::time_point ioEndT = std::chrono::steady_clock::now();
			std::chrono::microseconds ioElapsedMicroSec =
				std::chrono::duration_cast<std::chrono::microseconds>
				(ioEndT - ioStartT);

			// inc special rwmix thread stats
			if(isRWMixedReader)
			{
				entriesLatHistoReadMix.addLatency(ioElapsedMicroSec.count() );
				atomicLiveOpsReadMix.numEntriesDone++;
			}
			else
			{
				entriesLatHisto.addLatency(ioElapsedMicroSec.count() );
				atomicLiveOps.numEntriesDone++;
			}

		} // end of files for loop
	} // end of dirs for loop


#endif // HDFS_SUPPORT
}

/**
 * In netbench mode, this is the wrapper to start either server or client mode (or none) for this
 * worker thread. "None" would be the case if this was a server, but we don't have enough client
 * connections to feed all of the server threads (e.g. 1 client with single thread and 2 servers).
 */
void LocalWorker::netbenchDoTransfer()
{
	if(serverSocketVec.size() )
		netbenchDoTransferServer();
	else
	if(clientSocket)
		netbenchDoTransferClient();
	else // neither server nor client: clients don't have enough threads for all servers
	{
		LOGGER(Log_DEBUG, "This worker is neither initialized as server nor as client. "
			"Rank: " + std::to_string(workerRank) );

		workerGotPhaseWork = false;
	}
}

/**
 * In netbench mode, this worker owns its fair share of incoming client connections and polls all
 * of them until we have received a complete blocksized package from one, in which case a
 * respsized reply is sent back.
 */
void LocalWorker::netbenchDoTransferServer()
{
	const int pollShortTimeoutSecs = NETBENCH_SHORT_POLL_TIMEOUT_SEC; // to re-check for interrupt
	const uint64_t transferBytesPerConn = progArgs->getFileSize();
	const size_t blockSize = progArgs->getBlockSize();
	const size_t respSize = progArgs->getNetBenchRespSize();
	size_t transferBufSize = std::max(blockSize, respSize);
	std::unique_ptr<char[]> transferBuf(new char [transferBufSize] );
	const size_t localWorkerRank = workerRank - progArgs->getRankOffset();

	// get our own subset of sockets (each n-th socket, where n is number of local threads)

	SocketVec workerSocketVec;

	for(size_t i = localWorkerRank; i < serverSocketVec.size(); i += progArgs->getNumThreads() )
		workerSocketVec.push_back(serverSocketVec[i] );

	if(workerSocketVec.empty() )
	{ // this worker didn't get any work

		LOGGER(Log_DEBUG, "This server worker didn't get any sockets. "
			"Rank: " + std::to_string(workerRank) );

		workerGotPhaseWork = false;
		return;
	}

	UInt64Vec transferredBytesVec(workerSocketVec.size(), 0); // to check when we're done

	// build pollFDVec

	std::vector<struct pollfd> pollFDVec; // for poll()

	pollFDVec.reserve(workerSocketVec.size() );

	for(BasicSocket* sock : workerSocketVec)
		pollFDVec.push_back( { sock->getFD(), POLLIN, 0 } );


	// at the end of a tranfer, all serverSocketVec sockets will have been erased
	while(workerSocketVec.size() )
	{
		// wait for incoming data on any of the client connections

		int pollRes = 0;

		for(int elapsedSecs=0;
			!pollRes && (elapsedSecs < NETBENCH_RECEIVE_TIMEOUT_SEC);
			elapsedSecs += pollShortTimeoutSecs)
		{
			// (this short loop exists to more quickly detect user interrupt requests)
			checkInterruptionRequest();

			pollRes = poll(pollFDVec.data(), pollFDVec.size(), pollShortTimeoutSecs * 1000);
		}

		if(!pollRes)
			throw WorkerException("Server: Waiting for incoming data timed out. "
				"Rank: " + std::to_string(workerRank) + "; "
				"1st peer: " + workerSocketVec.at(0)->getPeername() + "; "
				"Num peers: " + std::to_string(workerSocketVec.size() ) );
		else
		if(pollRes == -1)
			throw WorkerException(std::string("Server: poll() failed. ") +
				"SysErr: " + std::strerror(errno) );

		// process the events that poll() returned

		int numEventsProcessed=0;

		for(unsigned i=0; (numEventsProcessed < pollRes) && (i < pollFDVec.size() ); i++)
		{
			if(!pollFDVec[i].revents)
				continue;

			// we have an event for this socket

			numEventsProcessed++;

			try
			{
				const uint64_t bytesLeft = transferBytesPerConn - transferredBytesVec[i];
				const size_t currentBlockSize = (bytesLeft < blockSize) ?
					bytesLeft : blockSize;

				// allow partial block read to not stall other sockets with available data

				ssize_t recvRes = workerSocketVec[i]->recvT(
					transferBuf.get(), currentBlockSize, 0, NETBENCH_RECEIVE_TIMEOUT_SEC);

				transferredBytesVec[i] += recvRes;

				atomicLiveOpsReadMix.numBytesDone += recvRes;

				// send single response byte after a complete block transfer
				if( ( (transferredBytesVec[i] % blockSize) == 0) ||
					(transferredBytesVec[i] == transferBytesPerConn) )
				{
					((*this).*funcPreWriteBlockModifier)(
						transferBuf.get(), gpuIOBufVec[0], respSize, transferredBytesVec[i] );

					workerSocketVec[i]->send(transferBuf.get(), respSize, 0);

					atomicLiveOps.numBytesDone += respSize;
					atomicLiveOpsReadMix.numIOPSDone++;
				}
			}
			catch(SocketDisconnectException& e)
			{
				if(transferredBytesVec[i] != transferBytesPerConn)
				{ // unexpected premature disconnect => probably ctrl+c
					LOGGER(Log_VERBOSE, "Server: Unexpected disconnect: " <<
							workerSocketVec[i]->getPeername() << "; " <<
						"Transferred bytes: " << transferredBytesVec[i] << "; " <<
						"Expected bytes: " << transferBytesPerConn << "; "
						"Message: " << e.what() << std::endl);

					transferredBytesVec.erase(transferredBytesVec.begin() + i);
					pollFDVec.erase(pollFDVec.begin() + i);
					workerSocketVec.erase(
						workerSocketVec.begin() + i); // destructor contains close()

					continue; // skip check below because elem [i] has been erased
				}
			}
			catch(SocketException& e)
			{ // avoid making noise if we have e.g. incomplete send() because of ctrl+c
				checkInterruptionRequest();
				throw;
			}

			if(transferredBytesVec[i] == transferBytesPerConn)
			{ // everything done with this connection
				LOGGER(Log_DEBUG,"Server: Transfer finished: " <<
					workerSocketVec[i]->getPeername() << std::endl);

				// (note: no sock shutdown() here because of possible infloop)

				transferredBytesVec.erase(transferredBytesVec.begin() + i);
				pollFDVec.erase(pollFDVec.begin() + i);
				workerSocketVec.erase(
					workerSocketVec.begin() + i); // destructor contains close()
			}

		} // end of poll() returned events loop
	} // end of while(serverSocketVec.size() )
}

/**
 * In netbench mode, this worker owns a single connection exclusively and keeps sending
 * "blocksize" to the server and waits "respsize" response after each block.
 */
void LocalWorker::netbenchDoTransferClient()
{
	const int pollShortTimeoutSecs = NETBENCH_SHORT_POLL_TIMEOUT_SEC; // to re-check for interrupt
	const uint64_t transferBytesPerConn = progArgs->getFileSize();
	const size_t blockSize = progArgs->getBlockSize();
	const size_t respSize = progArgs->getNetBenchRespSize();
	size_t transferBufSize = std::max(blockSize, respSize);
	std::unique_ptr<char[]> transferBuf(new char [transferBufSize] );

	// each loop is one blocksize transfer
	for(uint64_t transferredBytes = 0; transferredBytes != transferBytesPerConn; )
	{
		checkInterruptionRequest();

		try
		{
			const uint64_t bytesLeft = transferBytesPerConn - transferredBytes;
			const size_t currentBlockSize = (bytesLeft < blockSize) ?
				bytesLeft : blockSize;

			((*this).*funcRWRateLimiter)(currentBlockSize, isInterruptionRequested);

			std::chrono::steady_clock::time_point ioStartT = std::chrono::steady_clock::now();

			((*this).*funcPreWriteBlockModifier)(
				transferBuf.get(), gpuIOBufVec[0], currentBlockSize, transferredBytes);

			clientSocket->send(transferBuf.get(), currentBlockSize, 0);

			transferredBytes += currentBlockSize;

			// receive single response byte after a complete block transfer

			int recvRes = 0;

			for(int elapsedSecs=0;
				!recvRes && (elapsedSecs < NETBENCH_RECEIVE_TIMEOUT_SEC);
				elapsedSecs += pollShortTimeoutSecs)
			{
				// (this short loop exists to more quickly detect user interrupt requests)
				checkInterruptionRequest();

				try
				{
					recvRes = clientSocket->recvExactT(
						transferBuf.get(), respSize, 0, pollShortTimeoutSecs * 1000);

					atomicLiveOpsReadMix.numBytesDone += recvRes;
				}
				catch(SocketTimeoutException& e)
				{ /* ignore for pollShortTimeoutSecs. (NETBENCH_RECEIVE_TIMEOUT_SEC handled
						below.) */
				}
			}

			if(!recvRes)
				throw WorkerException("Client: Waiting for incoming data timed out. "
					"Peer: " + clientSocket->getPeername() + "; "
					"Transferred: " + std::to_string(transferredBytes) + " / " +
						std::to_string(transferBytesPerConn) + "; "
					"Timeout: " + std::to_string(NETBENCH_RECEIVE_TIMEOUT_SEC) + "s");

			// calc io operation latency
			std::chrono::steady_clock::time_point ioEndT = std::chrono::steady_clock::now();
			std::chrono::microseconds ioElapsedMicroSec =
				std::chrono::duration_cast<std::chrono::microseconds>
				(ioEndT - ioStartT);

			// iops lat & num done
			iopsLatHisto.addLatency(ioElapsedMicroSec.count() );
			atomicLiveOps.numBytesDone += currentBlockSize;
			atomicLiveOps.numIOPSDone++;
		}
		catch(SocketDisconnectException& e)
		{
			if(transferredBytes != transferBytesPerConn)
			{ // unexpected premature disconnect => probably ctrl+c
				LOGGER(Log_VERBOSE,"Client: Unexpected disconnect: " <<
					clientSocket->getPeername() << "; " <<
					"Transferred bytes: " << transferredBytes << "; " <<
					"Expected bytes: " << transferBytesPerConn << "; "
					"Message: " << e.what() << std::endl);

				break;
			}
		}
		catch(SocketException& e)
		{ // avoid making noise if we have e.g. incomplete send() because of ctrl+c
			checkInterruptionRequest();
			throw;
		}

		if(transferredBytes == transferBytesPerConn)
		{ // everything done with this connection
			LOGGER(Log_DEBUG,"Client: Transfer finished: " <<
				clientSocket->getPeername() << std::endl);

			// (note: no sock shutdown() here because of possible infloop)
		}

	} // end of for-loop for each block

}

/**
 * Calls the general sync() command to commit dirty pages from the linux page cache to stable
 * storage.
 *
 * Only the first worker of this instance does this, otherwise the kernel-level spinlocks of the
 * page cache make the sync extremely slow.
 *
 * @throw WorkerException on error.
 */
void LocalWorker::anyModeSync()
{
	// don't do anything if this is not the first worker thread of this instance
	if(workerRank != progArgs->getRankOffset() )
	{
		workerGotPhaseWork = false;
		return;
	}

#ifndef SYNCFS_SUPPORT

		sync();

#else // SYNCFS_SUPPORT

	const IntVec& pathFDs = progArgs->getBenchPathFDs();
	const StringVec& pathVec = progArgs->getBenchPaths();

	for(size_t i=0; i < pathFDs.size(); i++)
	{
		// (workerRank offset is to let different workers sync different file systems in parallel)
		size_t currentIdx = (i + workerRank) % pathFDs.size();
		int currentFD = pathFDs[currentIdx];

		int syncRes = syncfs(currentFD);

		if(syncRes == -1)
			throw WorkerException(std::string("Cache sync failed. ") +
				"Path: " + pathVec[currentIdx] + "; "
				"SysErr: " + strerror(errno) );
	}

#endif // SYNCFS_SUPPORT
}

/**
 * Prints 3 to /proc/sys/vm/drop_caches to drop cached data from the Linux page cache.
 *
 * Only the first worker of this instance does this, otherwise the kernel-level spinlocks of the
 * page cache make the flush extremely slow.
 *
 * @throw WorkerException on error.
 */
void LocalWorker::anyModeDropCaches()
{
	// don't do anything if this is not the first worker thread of this instance
	if(workerRank != progArgs->getRankOffset() )
	{
		workerGotPhaseWork = false;
		return;
	}

	std::string dropCachesPath = "/proc/sys/vm/drop_caches";
	std::string dropCachesValStr = "3"; // "3" to drop page cache, dentries and inodes

	int fd = open(dropCachesPath.c_str(), O_WRONLY);

	if(fd == -1)
		throw WorkerException(std::string("Opening virtual drop_caches file failed. ") +
			"Path: " + dropCachesPath + "; "
			"SysErr: " + strerror(errno) );

	ssize_t writeRes = write(fd, dropCachesValStr.c_str(), dropCachesValStr.size() );

	if(writeRes == -1)
	{
		close(fd);

		throw WorkerException(std::string("Writing to cache drop command file failed. ") +
			"Path: " + dropCachesPath + "; "
			"SysErr: " + strerror(errno) );
	}

	close(fd);
}
