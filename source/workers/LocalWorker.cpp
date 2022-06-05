#include "LocalWorker.h"
#include "toolkits/random/RandAlgoSelectorTk.h"
#include "WorkerException.h"
#include "WorkersSharedData.h"

#ifdef CUDA_SUPPORT
	#include <cuda_runtime.h>
#endif

#ifdef LIBAIO_SUPPORT
	#include <libaio.h>
#endif

#ifdef S3_SUPPORT
	#include <aws/core/auth/AWSCredentialsProvider.h>
	#include <aws/core/utils/HashingUtils.h>
	#include <aws/core/utils/memory/stl/AWSString.h>
	#include <aws/core/utils/memory/AWSMemory.h>
	#include <aws/core/utils/memory/stl/AWSStreamFwd.h>
	#include <aws/core/utils/stream/PreallocatedStreamBuf.h>
	#include <aws/core/utils/StringUtils.h>
	#include <aws/core/utils/threading/Executor.h>
	#include <aws/core/utils/UUID.h>
	#include <aws/s3/model/AbortMultipartUploadRequest.h>
	#include <aws/s3/model/BucketLocationConstraint.h>
	#include <aws/s3/model/CompleteMultipartUploadRequest.h>
	#include <aws/s3/model/CreateBucketRequest.h>
	#include <aws/s3/model/CreateMultipartUploadRequest.h>
	#include <aws/s3/model/DeleteBucketRequest.h>
	#include <aws/s3/model/DeleteObjectRequest.h>
	#include <aws/s3/model/GetObjectRequest.h>
	#include <aws/s3/model/HeadObjectRequest.h>
	#include <aws/s3/model/ListObjectsV2Request.h>
	#include <aws/s3/model/Object.h>
	#include <aws/s3/model/PutObjectRequest.h>
	#include <aws/s3/model/UploadPartRequest.h>
	#include <aws/transfer/TransferManager.h>
#endif

#define PATH_BUF_LEN					64
#define MKDIR_MODE						0777
#define INTERRUPTION_CHECK_INTERVAL		128
#define AIO_MAX_WAIT_SEC				5
#define AIO_MAX_EVENTS					4 // max number of events to retrieve in io_getevents()


#ifdef S3_SUPPORT
	S3UploadStore LocalWorker::s3SharedUploadStore; // singleton for shared uploads
#endif

#ifdef S3_SUPPORT
	namespace S3 = Aws::S3::Model;
#endif

#ifdef S3_SUPPORT
	/**
	 * Aws::IOStream derived in-memory stream implementation for S3 object upload/download. The
	 * actual in-memory part comes from the streambuf that gets provided to the constructor.
	 */
	class S3MemoryStream : public Aws::IOStream
	{
		public:
			using Base = Aws::IOStream;

			S3MemoryStream(std::streambuf *buf) : Base(buf) {}

			virtual ~S3MemoryStream() = default;
	};
#endif // S3_SUPPORT

LocalWorker::LocalWorker(WorkersSharedData* workersSharedData, size_t workerRank) :
	Worker(workersSharedData, workerRank)
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
		applyNumaAndCoreBinding();
		initThreadFDVec();
		initThreadCuFileHandleDataVec();
		allocIOBuffer();
		allocGPUIOBuffer();
		prepareCustomTreePathStores();
		initS3Client();

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
					{
						if(progArgs->getBenchPathType() != BenchPathType_DIR)
							throw WorkerException("Directory creation and deletion are not "
								"available in file and block device mode.");

						if(progArgs->getS3EndpointsVec().empty() )
						{
							progArgs->getTreeFilePath().empty() ?
								dirModeIterateDirs() : dirModeIterateCustomDirs();
						}
						else
							s3ModeIterateBuckets();

					} break;

					case BenchPhase_CREATEFILES:
					case BenchPhase_READFILES:
					{
						if(progArgs->getBenchPathType() == BenchPathType_DIR)
						{
							if(progArgs->getS3EndpointsVec().empty() )
								progArgs->getTreeFilePath().empty() ?
									dirModeIterateFiles() : dirModeIterateCustomFiles();
							else
								progArgs->getTreeFilePath().empty() ?
									s3ModeIterateObjects() : s3ModeIterateCustomObjects();
						}
						else
						{
							if(!progArgs->getUseRandomOffsets() )
								fileModeIterateFilesSeq();
							else
								fileModeIterateFilesRand();
						}
					} break;

					case BenchPhase_STATFILES:
					{
						if(progArgs->getBenchPathType() != BenchPathType_DIR)
							throw WorkerException("File stat operation not available in file and "
								"block device mode.");

						if(progArgs->getS3EndpointsVec().empty() )
							progArgs->getTreeFilePath().empty() ?
								dirModeIterateFiles() : dirModeIterateCustomFiles();
						else
							progArgs->getTreeFilePath().empty() ?
								s3ModeIterateObjects() : s3ModeIterateCustomObjects();
					} break;

					case BenchPhase_LISTOBJECTS:
					{
						s3ModeListObjects();
					} break;

					case BenchPhase_LISTOBJPARALLEL:
					{
						if(!progArgs->getTreeFilePath().empty() )
							throw WorkerException("Parallel object listing is not available in "
								"custom tree mode.");

						s3ModeListObjParallel();
					} break;

					case BenchPhase_DELETEFILES:
					{
						if(progArgs->getBenchPathType() == BenchPathType_DIR)
						{
							if(progArgs->getS3EndpointsVec().empty() )
								progArgs->getTreeFilePath().empty() ?
									dirModeIterateFiles() : dirModeIterateCustomFiles();
							else
								progArgs->getTreeFilePath().empty() ?
									s3ModeIterateObjects() : s3ModeIterateCustomObjects();
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

			} while(doInfiniteIOLoop); // end of infinite loop

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
			s3ModeAbortUnfinishedSharedUploads(); // abort unfinished S3 uploads
			finishPhase(); // let coordinator know that we are done
		}

		return;
	}
	catch(std::exception& e)
	{
		ErrLogger(Log_NORMAL, progArgs->getRunAsService() ) << e.what() << std::endl;

		s3ModeAbortUnfinishedSharedUploads(); // abort unfinished S3 uploads
	}

	incNumWorkersDoneWithError();
}

/**
 * Update finish time values, then signal coordinator that we're done.
 *
 * Nothing should run after this, because coordinator will assume that it can reset things after
 * the done counter has been increased inside this function.
 */
void LocalWorker::finishPhase()
{
	std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

	std::chrono::microseconds elapsedDurationUSec =
		std::chrono::duration_cast<std::chrono::microseconds>
		(now - workersSharedData->phaseStartT);
	uint64_t finishElapsedUSec = elapsedDurationUSec.count();

	elapsedUSecVec.resize(1);
	elapsedUSecVec[0] = finishElapsedUSec;

	phaseFinished = true; // before incNumWorkersDone() because Coordinator can reset after inc

	incNumWorkersDone();
}

/**
 * Initialize AWS S3 SDK & S3 client object. Intended to be called at the start of each benchmark
 * phase. Will do nothing if not built with S3 support or no S3 endpoints defined.
 *
 * S3 endpoints get assigned round-robin to workers based on workerRank.
 */
void LocalWorker::initS3Client()
{
#ifdef S3_SUPPORT

	if(progArgs->getS3EndpointsVec().empty() )
		return; // nothing to do

	Aws::Client::ClientConfiguration config;

	config.verifySSL = false; // to avoid self-signed certificate errors
	config.enableEndpointDiscovery = false; // to avoid delays for discovery
	config.maxConnections = progArgs->getIODepth();
	config.connectTimeoutMs = 5000;
	config.requestTimeoutMs = 300000;
	config.executor = std::make_shared<Aws::Utils::Threading::PooledThreadExecutor>(1);

	if(!progArgs->getS3Region().empty() )
		config.region = progArgs->getS3Region();

	Aws::Auth::AWSCredentials credentials;

	if(!progArgs->getS3AccessKey().empty() )
		credentials.SetAWSAccessKeyId(progArgs->getS3AccessKey() );

	if(!progArgs->getS3AccessSecret().empty() )
		credentials.SetAWSSecretKey(progArgs->getS3AccessSecret() );

	s3Client = std::make_shared<Aws::S3::S3Client>(credentials, config,
		(Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy)progArgs->getS3SignPolicy(), false);

	const StringVec& endpointsVec = progArgs->getS3EndpointsVec();
	size_t numEndpoints = endpointsVec.size();
	std::string endpoint = endpointsVec[workerRank % numEndpoints];

	s3Client->OverrideEndpoint(endpoint);

	s3EndpointStr = endpoint;

#endif // S3_SUPPORT
}

/**
 * Free S3 client object. Intended to be called at the end of each benchmark phase.
 */
void LocalWorker::uninitS3Client()
{
#ifdef S3_SUPPORT

	if(progArgs->getS3EndpointsVec().empty() )
		return; // nothing to do

	s3Client.reset();

#endif // S3_SUPPORT
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

		if(progArgs->getUseDirectIO() )
			openFlags |= O_DIRECT;

		if(progArgs->getRunCreateFilesPhase() )
			openFlags |= O_CREAT;

		fd = open(path.c_str(), openFlags, MKFILE_MODE);

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
		int closeRes = close(fd);

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

/*
 * Deregsiter threadCuFileHandleVec entries.
 */
void LocalWorker::uninitThreadCuFileHandleDataVec()
{
	for(CuFileHandleData& cuFileHandleData : fileHandles.threadCuFileHandleDataVec)
		cuFileHandleData.deregisterHandle();

	fileHandles.threadCuFileHandleDataVec.resize(0); // reset vec before reuse in service mode
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

	fileHandles.errorFDVecIdx = -1; // clear ("-1" means "not set")

	// reset/clear all vecs
	fileHandles.fdVec.resize(0);
	fileHandles.fdVecPtr = NULL;
	fileHandles.cuFileHandleDataVec.resize(0);
	fileHandles.cuFileHandleDataPtrVec.resize(0);


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
	}
	else
	if(!progArgs->getUseRandomOffsets() )
	{
		/* in sequential file/bdev mode, there is only one currently active file per worker.
			original file FDs will be taken from progArgs or fileHandles.threadFDVec, but FD will be
			copied to fileHandles.fdVec[0] for the single current file. */

		fileHandles.fdVec.resize(1);
		fileHandles.fdVec[0] = -1; // clear/reset, will be set dynamically to current file

		fileHandles.fdVecPtr = &fileHandles.fdVec;

		fileHandles.cuFileHandleDataPtrVec.resize(1); // set dynamically to current file
	}
	else
	{
		// in random file/bdev mode, rwBlockSized/aioBlockSized randomly select FDs from given set

		fileHandles.fdVecPtr = fileHandles.threadFDVec.empty() ?
			&progArgs->getBenchPathFDs() : &fileHandles.threadFDVec;

		CuFileHandleDataVec& cuFileHandleDataVec = fileHandles.threadCuFileHandleDataVec.empty() ?
			progArgs->getCuFileHandleDataVec() : fileHandles.threadCuFileHandleDataVec;

		for(size_t i=0; i < cuFileHandleDataVec.size(); i++)
			fileHandles.cuFileHandleDataPtrVec.push_back(&(cuFileHandleDataVec[i]) );
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
	const uint64_t fileSize = progArgs->getFileSize();

	/* in dir mode randAmount is file size, for file/bdev it's the total amount for this thread
		across all given files/bdevs */
	const uint64_t randomAmount = (progArgs->getBenchPathType() == BenchPathType_DIR) ?
		fileSize : progArgs->getRandomAmount() / progArgs->getNumDataSetThreads();

	// init random algos
	randOffsetAlgo = RandAlgoSelectorTk::stringToAlgo(progArgs->getRandOffsetAlgo() );
	randBlockVarAlgo = RandAlgoSelectorTk::stringToAlgo(progArgs->getBlockVarianceAlgo() );
	randBlockVarReseed = std::make_unique<RandAlgoXoshiro256ss>();

	// note: in some cases these defs get overridden per-file later (e.g. for custom tree)

	if(progArgs->getDoReverseSeqOffsets() || getS3ModeDoReverseSeqFallback() ) // seq backward
		rwOffsetGen = std::make_unique<OffsetGenReverseSeq>(
			fileSize, 0, blockSize);
	else
	if(!progArgs->getUseRandomOffsets() ) // sequential forward
		rwOffsetGen = std::make_unique<OffsetGenSequential>(
			fileSize, 0, blockSize);
	else
	if(progArgs->getUseRandomAligned() ) // random aligned
	{
		rwOffsetGen = std::make_unique<OffsetGenRandomAligned>(randomAmount, *randOffsetAlgo,
			fileSize, 0, blockSize);
	}
	else // random unaligned
		rwOffsetGen = std::make_unique<OffsetGenRandom>(randomAmount, *randOffsetAlgo,
			fileSize, 0, blockSize);
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
	const bool useCuFileAPI = progArgs->getUseCuFile();
	const BenchPathType benchPathType = progArgs->getBenchPathType();
	const bool integrityCheckEnabled = (progArgs->getIntegrityCheckSalt() != 0);
	const bool areGPUsGiven = !progArgs->getGPUIDsVec().empty();
	const bool doDirectVerify = progArgs->getDoDirectVerify();
	const unsigned blockVariancePercent = progArgs->getBlockVariancePercent();
	const unsigned rwMixPercent = progArgs->getRWMixPercent();
	const RandAlgoType blockVarAlgo = RandAlgoSelectorTk::stringToEnum(
		progArgs->getBlockVarianceAlgo() );
	const BenchPhase globalBenchPhase = workersSharedData->currentBenchPhase;
	const size_t localWorkerRank = workerRank - progArgs->getRankOffset();
	const bool isRWMixedReader = ( (globalBenchPhase == BenchPhase_CREATEFILES) &&
		(localWorkerRank < progArgs->getNumRWMixReadThreads() ) );

	nullifyPhaseFunctionPointers(); // set all function pointers to NULL


	// independent of whether current phase is read or write...
	// (these need to be set above the phase-dependent settings because those can override

	funcPositionalWrite = useCuFileAPI ?
		&LocalWorker::cuFileWriteWrapper : &LocalWorker::pwriteWrapper;

	funcPositionalRead = useCuFileAPI ?
		&LocalWorker::cuFileReadWrapper : &LocalWorker::preadWrapper;


	// phase-dependent settings...

	if(benchPhase == BenchPhase_CREATEFILES)
	{
		funcRWBlockSized = (ioDepth == 1) ?
			&LocalWorker::rwBlockSized : &LocalWorker::aioBlockSized;

		funcAioRwPrepper = (ioDepth == 1) ? NULL : &LocalWorker::aioWritePrepper;

		if(rwMixPercent && funcAioRwPrepper)
			funcAioRwPrepper = &LocalWorker::aioRWMixPrepper;

		funcPreWriteCudaMemcpy = (areGPUsGiven && !useCuFileAPI) ?
			&LocalWorker::cudaMemcpyGPUToHost : &LocalWorker::noOpCudaMemcpy;
		funcPostReadCudaMemcpy = &LocalWorker::noOpCudaMemcpy;

		if(useCuFileAPI && integrityCheckEnabled)
			funcPreWriteCudaMemcpy = &LocalWorker::cudaMemcpyHostToGPU;

		if(integrityCheckEnabled)
			funcPreWriteBlockModifier = &LocalWorker::preWriteIntegrityCheckFillBuf;
		else
		if(blockVariancePercent && (blockVarAlgo == RandAlgo_GOLDENRATIOPRIME) )
			funcPreWriteBlockModifier = &LocalWorker::preWriteBufRandRefillFast;
		else
		if(blockVariancePercent)
			funcPreWriteBlockModifier = &LocalWorker::preWriteBufRandRefill;
		else
			funcPreWriteBlockModifier = &LocalWorker::noOpIntegrityCheck;

		funcPostReadBlockChecker = &LocalWorker::noOpIntegrityCheck;

		if(doDirectVerify)
		{
			if(!useCuFileAPI)
				funcPositionalWrite = &LocalWorker::pwriteAndReadWrapper;
			else
			{
				funcPositionalWrite = &LocalWorker::cuFileWriteAndReadWrapper;
				funcPostReadCudaMemcpy = &LocalWorker::cudaMemcpyGPUToHost;
			}

			funcPostReadBlockChecker = &LocalWorker::postReadIntegrityCheckVerifyBuf;
		}

		uint64_t rateLimitMiBps = isRWMixedReader ?
			progArgs->getLimitReadBps() : progArgs->getLimitWriteBps();

		if(!rateLimitMiBps)
			funcRWRateLimiter = &LocalWorker::noOpRateLimiter;
		else
		{
			funcRWRateLimiter = &LocalWorker::preRWRateLimiter;
			rateLimiter.initStart(rateLimitMiBps);
		}
	}
	else // BenchPhase_READFILES (and others which don't use these function pointers)
	{
		funcRWBlockSized = (ioDepth == 1) ?
			&LocalWorker::rwBlockSized : &LocalWorker::aioBlockSized;

		funcAioRwPrepper = (ioDepth == 1) ? NULL : &LocalWorker::aioReadPrepper;

		funcPreWriteCudaMemcpy = &LocalWorker::noOpCudaMemcpy;
		funcPostReadCudaMemcpy = (areGPUsGiven && !useCuFileAPI) ?
			&LocalWorker::cudaMemcpyHostToGPU : &LocalWorker::noOpCudaMemcpy;

		if(useCuFileAPI && integrityCheckEnabled)
			funcPostReadCudaMemcpy = &LocalWorker::cudaMemcpyGPUToHost;

		funcPreWriteBlockModifier = &LocalWorker::noOpIntegrityCheck;
		funcPostReadBlockChecker = integrityCheckEnabled ?
			&LocalWorker::postReadIntegrityCheckVerifyBuf : &LocalWorker::noOpIntegrityCheck;

		if(!progArgs->getLimitReadBps() )
			funcRWRateLimiter = &LocalWorker::noOpRateLimiter;
		else
		{
			funcRWRateLimiter = &LocalWorker::preRWRateLimiter;
			rateLimiter.initStart(progArgs->getLimitReadBps() );
		}
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

	if(!progArgs->getS3EndpointsStr().empty() && !progArgs->getRunCreateFilesPhase() &&
		(progArgs->getUseS3FastRead() || progArgs->getUseS3TransferManager() ) )
		return; // nothing to do if read to /dev/null is set and no writes to be done

	// alloc number of IO buffers matching iodepth
	for(size_t i=0; i < progArgs->getIODepth(); i++)
	{
		char* ioBuf;

		// alloc I/O buffer appropriately aligned for O_DIRECT
		int allocAlignedRes = posix_memalign( (void**)&ioBuf, sysconf(_SC_PAGESIZE),
			progArgs->getBlockSize() );

		if(allocAlignedRes)
			throw WorkerException("Aligned memory allocation failed. "
				"Buffer size: " + std::to_string(progArgs->getBlockSize() ) + "; "
				"Page size: " + std::to_string(sysconf(_SC_PAGESIZE) ) + "; "
				"SysErr: " + strerror(allocAlignedRes) ); // yes, not errno here

		ioBufVec.push_back(ioBuf);

		// fill buffer with random data to ensure it's really alloc'ed (and not "sparse")
		unsigned seed = 0;
		int* intIOBuf = (int*)ioBuf;
		for(size_t i=0; i < (progArgs->getBlockSize() / sizeof(unsigned) ); i++)
			intIOBuf[i] = rand_r(&seed);
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
	{
		// gpu bufs won't be accessed, but vec elems might be passed e.g. to noOpCudaMemcpy
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

#endif // CUDA_SUPPORT

#ifndef CUFILE_SUPPORT

	if(progArgs->getUseCuFile() )
		throw WorkerException("cuFile API requested, but this executable was built without cuFile "
			"support.");

#else // CUFILE_SUPPORT

	// register GPU buffer for DMA
	for(char* gpuIOBuf : gpuIOBufVec)
	{
		if(!progArgs->getUseCuFile() || !gpuIOBuf || !progArgs->getUseGPUBufReg() )
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
 * Prepare paths for custom tree mode for this worker.
 *
 * @throw WorkerException on error
 */
void LocalWorker::prepareCustomTreePathStores()
{
	if(progArgs->getTreeFilePath().empty() )
		return; // nothing to do here

	bool throwOnSmallerThanBlockSize = progArgs->getUseRandomOffsets();

	progArgs->getCustomTreeFilesNonShared().getWorkerSublistNonShared(workerRank,
		progArgs->getNumDataSetThreads(), throwOnSmallerThanBlockSize, customTreeFiles);

	size_t numThreads = progArgs->getNumThreads();
	size_t numDataSetThreads = progArgs->getNumDataSetThreads();

	if(progArgs->getRunAsService() && !progArgs->getS3EndpointsStr().empty() &&
		progArgs->getRunCreateFilesPhase() && (numDataSetThreads != numThreads) )
	{
		/* shared s3 uploads of an object possible, but only among threads within the same
			service instance (due to the uploadID not beeing shared among service instances).
			in this case, ProgArgs prepared a special shared file tree that only contains files for
			this service instance. thus we use only the local ranks, not the global ranks. */

		size_t workerRankLocal = workerRank - progArgs->getRankOffset();

		progArgs->getCustomTreeFilesShared().getWorkerSublistShared(workerRankLocal,
			numThreads, throwOnSmallerThanBlockSize, customTreeFiles);
	}
	else
	{
		// this is the normal case: fair sharing across all services/workers based on blocksize

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

	uninitS3Client();

	// reset custom tree mode path store
	customTreeFiles.clear();

#ifdef CUFILE_SUPPORT
	// deregister GPU buffers for DMA
	for(char* gpuIOBuf : gpuIOBufVec)
	{
		if(!progArgs->getUseCuFile() || !gpuIOBuf || !progArgs->getUseGPUBufReg() )
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
#endif

	// free host memory buffers
	for(char* ioBuf : ioBufVec)
		SAFE_FREE(ioBuf);

	uninitThreadCuFileHandleDataVec();
	uninitThreadFDVec();
}

/**
 * Loop around pread/pwrite to use user-defined block size instead of full given count in one call.
 * Reads/writes the pre-allocated ioBuf. Uses rwOffsetGen for next offset and block size.
 *
 * @fdVec if more multiple fds are given then they will be used round-robin; there is no guarantee
 * 		for which fd from the vec will be used first, so this is only suitable for random IO.
 * @return similar to pread/pwrite.
 */
int64_t LocalWorker::rwBlockSized()
{
	const BenchPhase globalBenchPhase = workersSharedData->currentBenchPhase;
	const unsigned rwMixPercent = progArgs->getRWMixPercent();
	const size_t fileHandleVecSize = fileHandles.fdVecPtr->size();

	OffsetGenRandom randFileIndexGen(~(uint64_t)0, *randOffsetAlgo, fileHandleVecSize, 0, 1);

	while(rwOffsetGen->getNumBytesLeftToSubmit() )
	{
		const uint64_t currentOffset = rwOffsetGen->getNextOffset();
		const size_t blockSize = rwOffsetGen->getNextBlockSizeToSubmit();
		const size_t fileHandleIdx = (fileHandleVecSize > 1) ? randFileIndexGen.getNextOffset() : 0;
		bool isRWMixRead = false;
		ssize_t rwRes;

		std::chrono::steady_clock::time_point ioStartT = std::chrono::steady_clock::now();

		((*this).*funcRWRateLimiter)(blockSize);
		((*this).*funcPreWriteBlockModifier)(ioBufVec[0], blockSize, currentOffset); // fill buffer
		((*this).*funcPreWriteCudaMemcpy)(ioBufVec[0], gpuIOBufVec[0], blockSize);

		if(benchPhase == BenchPhase_READFILES)
		{ // this is a read, but could be a rwmix read thread
			isRWMixRead = (benchPhase != globalBenchPhase);

			rwRes = ((*this).*funcPositionalRead)(
				fileHandleIdx, ioBufVec[0], blockSize, currentOffset);
		}
		else // this is a write or rwmixpct read
		if(rwMixPercent &&
			( ( (workerRank + numIOPSSubmitted) % 100) < rwMixPercent) )
		{ // this is a rwmix read
			isRWMixRead = true;

			rwRes = ((*this).*funcPositionalRead)(
				fileHandleIdx, ioBufVec[0], blockSize, currentOffset);
		}
		else
		{ // this is a plain write
			rwRes = ((*this).*funcPositionalWrite)(
				fileHandleIdx, ioBufVec[0], blockSize, currentOffset);
		}

		IF_UNLIKELY(rwRes <= 0)
		{ // unexpected result
			ERRLOGGER(Log_NORMAL, "IO failed: " << "blockSize: " << blockSize << "; " <<
				"currentOffset:" << currentOffset << "; " <<
				"leftToSubmit:" << rwOffsetGen->getNumBytesLeftToSubmit() << "; " <<
				"rank:" << workerRank << "; " <<
				"return code: " << rwRes << "; " <<
				"errno: " << errno << std::endl);

			fileHandles.errorFDVecIdx = fileHandleIdx;

			return (rwRes < 0) ?
				rwRes :
				(rwOffsetGen->getNumBytesTotal() - rwOffsetGen->getNumBytesLeftToSubmit() );
		}

		((*this).*funcPostReadCudaMemcpy)(ioBufVec[0], gpuIOBufVec[0], blockSize);
		((*this).*funcPostReadBlockChecker)(ioBufVec[0], blockSize, currentOffset); // verify buf

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
 * Reads/writes the pre-allocated ioBuf. Uses iodepth from progArgs. Uses rwOffsetGen for next
 * offset and block size.
 *
 * @fdVec if more multiple fds are given then they will be used round-robin; there is no guarantee
 * 		for which fd from the vec will be used first, so this is only suitable for random IO.
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
	const size_t maxIODepth = progArgs->getIODepth();
	const size_t fileHandlesVecSize = fileHandles.fdVecPtr->size();

	size_t numPending = 0; // num requests submitted and pending for completion
	size_t numBytesDone = 0; // after successfully completed requests

	io_context_t ioContext = {}; // zeroing required by io_queue_init
	std::vector<struct iocb> iocbVec(maxIODepth);
	std::vector<struct iocb*> iocbPointerVec(maxIODepth);
	std::vector<std::chrono::steady_clock::time_point> ioStartTimeVec(maxIODepth);
	struct io_event ioEvents[AIO_MAX_EVENTS];
	struct timespec ioTimeout;

	OffsetGenRandom randFileIndexGen(~(uint64_t)0, *randOffsetAlgo, fileHandlesVecSize, 0, 1);

	int initRes = io_queue_init(maxIODepth, &ioContext);
	IF_UNLIKELY(initRes)
		throw WorkerException(std::string("Initializing async IO (io_queue_init) failed. ") +
			"SysErr: " + strerror(-initRes) ); // (io_queue_init returns negative errno)


	// P H A S E 1: initial seed of io submissions up to full ioDepth

	while(rwOffsetGen->getNumBytesLeftToSubmit() && (numPending < maxIODepth) )
	{
		const size_t blockSize = rwOffsetGen->getNextBlockSizeToSubmit();
		const uint64_t currentOffset = rwOffsetGen->getNextOffset();
		const size_t fileHandlesIdx = (fileHandlesVecSize > 1) ?
			randFileIndexGen.getNextOffset() : 0;
		const int fd = (*fileHandles.fdVecPtr)[fileHandlesIdx];
		const size_t ioVecIdx = numPending; // iocbVec index

		iocbPointerVec[ioVecIdx] = &iocbVec[ioVecIdx];

		((*this).*funcAioRwPrepper)(&iocbVec[ioVecIdx], fd, ioBufVec[ioVecIdx], blockSize,
			currentOffset);
		iocbVec[ioVecIdx].data = (void*)ioVecIdx; /* the vec index of this request; ioctl.data
						is caller's private data returned after io_getevents as ioEvents[].data */

		ioStartTimeVec[ioVecIdx] = std::chrono::steady_clock::now();

		((*this).*funcRWRateLimiter)(blockSize);
		((*this).*funcPreWriteBlockModifier)(ioBufVec[ioVecIdx], blockSize, currentOffset); // fill
		((*this).*funcPreWriteCudaMemcpy)(ioBufVec[ioVecIdx], gpuIOBufVec[ioVecIdx], blockSize);

		int submitRes = io_submit(ioContext, 1, &iocbPointerVec[ioVecIdx] );
		IF_UNLIKELY(submitRes != 1)
		{
			io_queue_release(ioContext);

			throw WorkerException(std::string("Async IO submission (io_submit) failed. ") +
				"NumRequests: " + std::to_string(numPending) + "; "
				"ReturnCode: " + std::to_string(submitRes) + "; "
				"SysErr: " + strerror(-initRes) ); // (io_queue_init returns negative errno)
		}

		numPending++;
		numIOPSSubmitted++;
		rwOffsetGen->addBytesSubmitted(blockSize);
	}


	// P H A S E 2: wait for submissions to complete and submit new requests if bytes left

	while(numPending)
	{
		ioTimeout.tv_sec = AIO_MAX_WAIT_SEC;
		ioTimeout.tv_nsec = 0;

		int eventsRes = io_getevents(ioContext, 1, AIO_MAX_EVENTS, ioEvents, &ioTimeout);
		IF_UNLIKELY(!eventsRes)
		{ // timeout expired; that's ok, as we set a short timeout to check interruptions
			checkInterruptionRequest( [&]() {io_queue_release(ioContext); } );
			continue;
		}
		else
		IF_UNLIKELY(eventsRes < 0)
		{
			io_queue_release(ioContext);

			throw WorkerException(std::string("Getting async IO events (io_getevents) failed. ") +
				"NumPending: " + std::to_string(numPending) + "; "
				"ReturnCode: " + std::to_string(eventsRes) + "; "
				"Wait time: " + std::to_string(AIO_MAX_WAIT_SEC) + "; "
				"Wait time left: " + std::to_string(ioTimeout.tv_sec) + "; "
				"SysErr: " + strerror(-initRes) ); // (io_queue_init returns negative errno)
		}

		// check result of completed iocbs and reuse them if any blocks left to submit

		for(int eventIdx = 0; eventIdx < eventsRes; eventIdx++)
		{
			// ioEvents[].res2 is negative errno for aio framework errors, 0 means no error
			// ioEvents[].res is actually read/written bytes when res2==0 or negative errno
			/* note: all messy with res/res2, because defined as ulong, but examples need them
				interpreted as int for errors, which can overlap valid partial writes on 64bit */

			IF_UNLIKELY(ioEvents[eventIdx].res2 ||
				(ioEvents[eventIdx].res != ioEvents[eventIdx].obj->u.c.nbytes) )
			{ // unexpected result
				io_queue_release(ioContext);

				if(ioEvents[eventIdx].res2)
					throw WorkerException(std::string("Async IO framework error. ") +
						"NumPending: " + std::to_string(numPending) + "; "
						"res: " + std::to_string(ioEvents[eventIdx].res2) + "; "
						"res2: " + std::to_string(ioEvents[eventIdx].res) + "; "
						"IO size: " + std::to_string(ioEvents[eventIdx].obj->u.c.nbytes) + "; "
						"SysErr: " + strerror(-(int)ioEvents[eventIdx].res2) );

				if( (int)ioEvents[eventIdx].res < 0)
				{
					errno = -(int)ioEvents[eventIdx].res; // res is negative errno
					return -1;
				}

				// partial read/write, so return what we got so far
				return (numBytesDone + ioEvents[eventIdx].res);
			}

			const size_t ioVecIdx = (size_t)ioEvents[eventIdx].data; // caller priv data is vec idx

			((*this).*funcPostReadCudaMemcpy)(ioBufVec[ioVecIdx], gpuIOBufVec[ioVecIdx],
				ioEvents[eventIdx].obj->u.c.nbytes);
			((*this).*funcPostReadBlockChecker)( (char*)ioEvents[eventIdx].obj->u.c.buf,
				ioEvents[eventIdx].obj->u.c.nbytes, ioEvents[eventIdx].obj->u.c.offset); // verify

			// calc io operation latency
			std::chrono::steady_clock::time_point ioEndT = std::chrono::steady_clock::now();
			std::chrono::microseconds ioElapsedMicroSec =
				std::chrono::duration_cast<std::chrono::microseconds>
				(ioEndT - ioStartTimeVec[ioVecIdx] );

			numBytesDone += ioEvents[eventIdx].res;

			// inc special rwmix read stats
			if(	(ioEvents[eventIdx].obj->aio_lio_opcode == IO_CMD_PREAD) &&
				(globalBenchPhase == BenchPhase_CREATEFILES) )
			{ // this is a read in a write phase => inc rwmix read stats
				iopsLatHistoReadMix.addLatency(ioElapsedMicroSec.count() );
				atomicLiveOpsReadMix.numBytesDone += ioEvents[eventIdx].res;
				atomicLiveOpsReadMix.numIOPSDone++;
			}
			else
			{
				iopsLatHisto.addLatency(ioElapsedMicroSec.count() );
				atomicLiveOps.numBytesDone += ioEvents[eventIdx].res;
				atomicLiveOps.numIOPSDone++;
			}

			checkInterruptionRequest( [&]() {io_queue_release(ioContext); } );

			if(!rwOffsetGen->getNumBytesLeftToSubmit() )
			{
				numPending--;
				continue;
			}

			// request complete, so reuse iocb for the next request...

			const size_t blockSize = rwOffsetGen->getNextBlockSizeToSubmit();
			const uint64_t currentOffset = rwOffsetGen->getNextOffset();
			const size_t fileHandlesIdx = (fileHandlesVecSize > 1) ?
				randFileIndexGen.getNextOffset() : 0;
			const int fd = (*fileHandles.fdVecPtr)[fileHandlesIdx];

			((*this).*funcAioRwPrepper)(ioEvents[eventIdx].obj, fd, ioBufVec[ioVecIdx], blockSize,
				currentOffset);
			ioEvents[eventIdx].obj->data = (void*)ioVecIdx; // caller's private data

			ioStartTimeVec[ioVecIdx] = std::chrono::steady_clock::now();

			((*this).*funcRWRateLimiter)(blockSize);
			((*this).*funcPreWriteBlockModifier)(ioBufVec[ioVecIdx], blockSize, currentOffset);
			((*this).*funcPreWriteCudaMemcpy)(ioBufVec[ioVecIdx], gpuIOBufVec[ioVecIdx], blockSize);

			int submitRes = io_submit(
				ioContext, 1, &iocbPointerVec[ioVecIdx] );
			IF_UNLIKELY(submitRes != 1)
			{
				io_queue_release(ioContext);

				throw WorkerException(std::string("Async IO resubmission (io_submit) failed. ") +
					"NumRequests: " + std::to_string(numPending) + "; "
					"ReturnCode: " + std::to_string(submitRes) + "; "
					"SysErr: " + strerror(-initRes) ); // (io_queue_init returns negative errno)
			}

			numIOPSSubmitted++;
			rwOffsetGen->addBytesSubmitted(blockSize);

		} // end of for loop to resubmit completed iocbs

	} // end of while loop until all blocks completed

	io_queue_release(ioContext);

	return rwOffsetGen->getNumBytesTotal();

#endif // LIBAIO_SUPPORT
}

/**
 * Noop for the case when no integrity check selected by user.
 */
void LocalWorker::noOpIntegrityCheck(char* buf, size_t bufLen, off_t fileOffset)
{
	return; // noop
}

/**
 * Fill buf with unsigned 64bit values made of offset plus integrity check salt.
 *
 * @bufLen buf len to fill with checksums
 * @fileOffset file offset for buf
 */
void LocalWorker::preWriteIntegrityCheckFillBuf(char* buf, size_t bufLen, off_t fileOffset)
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
		size_t checkSumCopyLen = std::min(numBytesLeft, checkSumLen - checkSumArrayStartIdx);

		memcpy(&buf[numBytesDone], &checkSumArray[checkSumArrayStartIdx], checkSumCopyLen);

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
void LocalWorker::postReadIntegrityCheckVerifyBuf(char* buf, size_t bufLen, off_t fileOffset)
{
	IF_UNLIKELY(!bufLen)
		return;

	char* verifyBuf = (char*)malloc(bufLen);

	IF_UNLIKELY(!verifyBuf)
		throw WorkerException("Buffer alloc for verification buffer failed. "
			"Size: " + std::to_string(bufLen) );

	// fill verifyBuf with the correct data
	preWriteIntegrityCheckFillBuf(verifyBuf, bufLen, fileOffset);

	// compare correct data to actual data
	int compareRes = memcmp(buf, verifyBuf, bufLen);

	if(!compareRes)
	{ // buffers are equal, so all good
		free(verifyBuf);
		return;
	}

	// verification failed, find exact mismatch offset
	for(size_t i=0; i < bufLen; i++)
	{
		if(verifyBuf[i] == buf[i])
			continue;

		// we found the exact offset for mismatch

		unsigned expectedVal = (unsigned char)verifyBuf[i];
		unsigned actualVal = (unsigned char)buf[i];

		free(verifyBuf);

		throw WorkerException("Data verification failed. "
			"Offset: " + std::to_string(fileOffset + i) + "; "
			"Expected value: " + std::to_string(expectedVal) + "; "
			"Actual value: " + std::to_string(actualVal) );
	}
}

/**
 * Fill buffer with given value. In contrast to memset() this can full 64bit values to at least
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
void LocalWorker::preWriteBufRandRefill(char* buf, size_t bufLen, off_t fileOffset)
{
	// note: this same logic is used in aioRWMixPrepper/pwriteRWMixWrapper
	if( ( (workerRank + numIOPSSubmitted) % 100) < progArgs->getRWMixPercent() )
		return; // this is a read in rwmix mode, so no need for refill in this round

	// refill buffer with random data

	const unsigned blockVariancePercent = progArgs->getBlockVariancePercent();
	const uint64_t varFillLen = (bufLen * blockVariancePercent) / 100;
	const size_t constFillRemainderLen = bufLen - varFillLen;

	randBlockVarAlgo->fillBuf(buf, varFillLen);

	if(!constFillRemainderLen)
		return;

	// fill remainder of buffer with same 64bit value
	// note: rand algo is used to defeat simple dedupe across remainders of different blocks
	bufFill(&buf[varFillLen], randBlockVarAlgo->next(), constFillRemainderLen);
}

/**
 * Refill some percentage of the buffer with random data. The percentage to refill is defined via
 * progArgs::blockVariancePercent.
 *
 * Note: The only reason why this function exists separate from preWriteBufRandRefill() which does
 * the same with RandAlgoGoldenPrime::fillBuf() is that tests have shown 30% lower perf for
 * "-w -t 1 -b 128k --iodepth 128 --blockvarpct 100 --rand --direct" when the function in the
 * RandAlgo object is called (which is quite mysterious).
 */
void LocalWorker::preWriteBufRandRefillFast(char* buf, size_t bufLen, off_t fileOffset)
{
	// note: this same logic is used in aioRWMixPrepper/pwriteRWMixWrapper
	if( ( (workerRank + numIOPSSubmitted) % 100) < progArgs->getRWMixPercent() )
		return; // this is a read in rwmix mode, so no need for refill in this round

	// note: workerRank is used to have skew between different worker threads
	if( ( (workerRank + numIOPSSubmitted) % 100) >= progArgs->getBlockVariancePercent() )
		return;

	// refill buffer with random data

	const unsigned blockVariancePercent = progArgs->getBlockVariancePercent();
	const uint64_t varFillLen = (bufLen * blockVariancePercent) / 100;
	const size_t constFillRemainderLen = bufLen - varFillLen;

	uint64_t state = randBlockVarReseed->next();

	size_t numBytesDone = 0;

	for(uint64_t i=0; i < (varFillLen / sizeof(uint64_t) ); i++)
	{
		uint64_t* uint64Buf = (uint64_t*)buf;
		state *= RANDALGO_GOLDEN_RATIO_PRIME;
		state >>= 3;
		*uint64Buf = state;

		buf += sizeof(uint64_t);
		numBytesDone += sizeof(uint64_t);
	}

	if(numBytesDone < varFillLen)
	{ // we have a remainder to fill, which can only be smaller than sizeof(uint64_t)
		state *= RANDALGO_GOLDEN_RATIO_PRIME;
		state >>= 3;
		uint64_t randUint64 = state;

		const size_t memcpySize = varFillLen - numBytesDone;

		memcpy(buf, &randUint64, memcpySize);
		buf += memcpySize;
	}

	if(!constFillRemainderLen)
		return;

	// fill remainder of buffer with same 64bit value
	// note: rand algo is used to defeat simple dedupe across remainders of different blocks
	bufFill(buf, randBlockVarAlgo->next(), constFillRemainderLen);
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
	if( ( (workerRank + numIOPSSubmitted) % 100) >= progArgs->getRWMixPercent() )
		io_prep_pwrite(iocb, fd, buf, count, offset);
	else
		io_prep_pread(iocb, fd, buf, count, offset);

#endif // LIBAIO_SUPPORT
}

/**
 * Noop for cases where no rate limit selected by user.
 */
void LocalWorker::noOpRateLimiter(size_t rwSize)
{
	return; // noop
}

/**
 * Rate limiter before reads in case rate limit was selected by user.
 */
void LocalWorker::preRWRateLimiter(size_t rwSize)
{
	rateLimiter.wait(rwSize);
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

	return pread(fd, buf, nbytes, offset);
}

/**
 * Wrapper for positional sync write.
 */
ssize_t LocalWorker::pwriteWrapper(size_t fileHandleIdx, void* buf, size_t nbytes, off_t offset)
{
	const int fd = (*fileHandles.fdVecPtr)[fileHandleIdx];

	return pwrite(fd, buf, nbytes, offset);
}

/**
 * Wrapper for positional sync write followed by an immediate read of the same block.
 */
ssize_t LocalWorker::pwriteAndReadWrapper(size_t fileHandleIdx, void* buf, size_t nbytes, off_t offset)
{
	const int fd = (*fileHandles.fdVecPtr)[fileHandleIdx];

	ssize_t pwriteRes = pwrite(fd, buf, nbytes, offset);

	IF_UNLIKELY(pwriteRes <= 0)
		return pwriteRes;

	return pread(fd, buf, pwriteRes, offset);
}

/**
 * Within a write phase, send user-defined pecentage of block reads for mixed r/w.
 *
 * Parameters and return value are similar to p{write,read}.
 */
ssize_t LocalWorker::pwriteRWMixWrapper(size_t fileHandleIdx, void* buf, size_t nbytes, off_t offset)
{
	// example: 40% means 40 out of 100 submitted blocks will be reads, the remaining 60 are writes

	/* note: keep in mind that this also needs to work with lots of small files, so percentage needs
		to work between different files. (numIOPSSubmitted ensures that below; numIOPSDone would not
		work for this because aio would not inc counter directly on submission.) */

	const int fd = (*fileHandles.fdVecPtr)[fileHandleIdx];

	// note: workerRank is used to have skew between different worker threads
	if( ( (workerRank + numIOPSSubmitted) % 100) >= progArgs->getRWMixPercent() )
		return pwrite(fd, buf, nbytes, offset);
	else
		return pread(fd, buf, nbytes, offset);
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
	return cuFileRead(fileHandles.cuFileHandleDataPtrVec[fileHandleIdx]->cfr_handle,
		gpuIOBufVec[0], nbytes, offset, 0);
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
	return cuFileWrite(fileHandles.cuFileHandleDataPtrVec[fileHandleIdx]->cfr_handle,
		gpuIOBufVec[0], nbytes, offset, 0);
#endif
}

/**
 * Wrapper for positional sync cuFile write followed by an immediate cuFile read of the same block.
 */
ssize_t LocalWorker::cuFileWriteAndReadWrapper(size_t fileHandleIdx, void* buf, size_t nbytes, off_t offset)
{
#ifndef CUFILE_SUPPORT
	throw WorkerException("cuFileWriteAndReadWrapper called, but this executable was built without "
		"cuFile API support");
#else
	ssize_t writeRes =
		cuFileWrite(fileHandles.cuFileHandleDataPtrVec[fileHandleIdx]->cfr_handle,
			gpuIOBufVec[0], nbytes, offset, 0);

	IF_UNLIKELY(writeRes <= 0)
		return writeRes;

	return cuFileRead(fileHandles.cuFileHandleDataPtrVec[fileHandleIdx]->cfr_handle,
		gpuIOBufVec[0], writeRes, offset, 0);
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

	// note: workerRank is used to have skew between different worker threads
	if( ( (workerRank + numIOPSSubmitted) % 100) >= progArgs->getRWMixPercent() )
		return cuFileWrite(fileHandles.cuFileHandleDataPtrVec[fileHandleIdx]->cfr_handle,
			gpuIOBufVec[0], nbytes, offset, 0);
	else
		return cuFileRead(fileHandles.cuFileHandleDataPtrVec[fileHandleIdx]->cfr_handle,
			gpuIOBufVec[0], nbytes, offset, 0);
#endif
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

			int mkdirRes = mkdirat(pathFDs[pathFDsIndex], currentPath.data(), MKDIR_MODE);

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
			int mkdirRes = mkdirat(pathFDs[pathFDsIndex], currentPath.data(), MKDIR_MODE);

			if( (mkdirRes == -1) && (errno != EEXIST) )
				throw WorkerException(std::string("Directory creation failed. ") +
					"Path: " + pathVec[pathFDsIndex] + "/" + currentPath.data() + "; "
					"SysErr: " + strerror(errno) );
		}

		if(benchPhase == BenchPhase_DELETEDIRS)
		{ // remove dir
			int rmdirRes = unlinkat(pathFDs[pathFDsIndex], currentPath.data(), AT_REMOVEDIR);

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

			int rmdirRes = unlinkat(pathFDs[pathFDsIndex], currentPath.data(), AT_REMOVEDIR);

			if( (rmdirRes == -1) && ( (errno != ENOENT) || !ignoreDelErrors) )
				throw WorkerException(std::string("Directory deletion failed. ") +
					"Path: " + pathVec[pathFDsIndex] + "/" + currentPath.data() + "; "
					"SysErr: " + strerror(errno) );
		}
	}

}

/**
 * In directory mode with custom tree, iterate over all directories to create or remove them.
 * All workers create/remove all dirs.
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
	const PathList& customTreePaths = progArgs->getCustomTreeDirs().getPaths();
	const bool reverseOrder = (benchPhase == BenchPhase_DELETEDIRS);

	IF_UNLIKELY(customTreePaths.empty() )
		return; // nothing to do here

	/* note on reverse: dirs are ordered by path length, so that parents dirs come before their
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
			int mkdirRes = mkdirat(benchPathFD, currentPathElem.path.c_str(), MKDIR_MODE);

			if( (mkdirRes == -1) && (errno != EEXIST) )
				throw WorkerException(std::string("Directory creation failed. ") +
					"Path: " + benchPathStr + "/" + currentPathElem.path + "; "
					"SysErr: " + strerror(errno) );
		}

		if(benchPhase == BenchPhase_DELETEDIRS)
		{ // remove dir
			int rmdirRes = unlinkat(benchPathFD, currentPathElem.path.c_str(), AT_REMOVEDIR);

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
 * If dir sharing is enabled, all workers will use dirs or rank 0.
 *
 * @throw WorkerException on error.
 */
void LocalWorker::dirModeIterateFiles()
{
	const bool haveSubdirs = (progArgs->getNumDirs() > 0);
	const size_t numDirs = haveSubdirs ? progArgs->getNumDirs() : 1; // set 1 to run dir loop once
	const size_t numFiles = progArgs->getNumFiles();
	const uint64_t fileSize = progArgs->getFileSize();
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
					currentPath.data(), openFlags, fileSize);

				// try-block to ensure that fd is closed in case of exception
				try
				{
					((*this).*funcCuFileHandleReg)(fd, cuFileHandleData); // reg cuFile handle

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
					((*this).*funcCuFileHandleDereg)(cuFileHandleData); // dereg cuFile handle

					close(fd);
					throw;
				}

				((*this).*funcCuFileHandleDereg)(cuFileHandleData); // deReg cuFile handle

				int closeRes = close(fd);

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

				if(statRes == -1)
					throw WorkerException(std::string("File stat failed. ") +
						"Path: " + pathVec[pathFDsIndex] + "/" + currentPath.data() + "; "
						"SysErr: " + strerror(errno) );
			}

			if(benchPhase == BenchPhase_DELETEFILES)
			{
				int unlinkRes = unlinkat(pathFDs[pathFDsIndex], currentPath.data(), 0);

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

	int& fd = fileHandles.fdVec[0];
	CuFileHandleData& cuFileHandleData = fileHandles.cuFileHandleDataVec[0];

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
				currentPathElem.path.c_str(), openFlags, currentPathElem.totalLen);

			// try-block to ensure that fd is closed in case of exception
			try
			{
				((*this).*funcCuFileHandleReg)(fd, cuFileHandleData); // reg cuFile handle

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
							"Expected written: " + std::to_string(rangeLen) );
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
							"Expected read: " + std::to_string(rangeLen) );
				}
			}
			catch(...)
			{ // ensure that we don't leak an open file fd
				((*this).*funcCuFileHandleDereg)(cuFileHandleData); // dereg cuFile handle

				close(fd);
				throw;
			}

			((*this).*funcCuFileHandleDereg)(cuFileHandleData); // deReg cuFile handle

			int closeRes = close(fd);

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

			if(statRes == -1)
				throw WorkerException(std::string("File stat failed. ") +
					"Path: " + benchPathStr + "/" + currentPath + "; "
					"SysErr: " + strerror(errno) );
		}

		if(benchPhase == BenchPhase_DELETEFILES)
		{
			int unlinkRes = unlinkat(benchPathFD, currentPath, 0);

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
	// funcRWBlockSized() will send IOs round-robin to all user-given files.

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
				"Expected written: " + std::to_string(rwOffsetGen->getNumBytesTotal() ) );
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
				"Expected read: " + std::to_string(rwOffsetGen->getNumBytesTotal() ) );
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
	const IntVec& pathFDs = fileHandles.threadFDVec.empty() ?
		progArgs->getBenchPathFDs() : fileHandles.threadFDVec;
	CuFileHandleDataVec& cuFileHandleDataVec = fileHandles.threadCuFileHandleDataVec.empty() ?
		progArgs->getCuFileHandleDataVec() : fileHandles.threadCuFileHandleDataVec;
	const size_t numFiles = pathFDs.size();
	const uint64_t fileSize = progArgs->getFileSize();
	const size_t blockSize = progArgs->getBlockSize();
	const size_t numThreads = progArgs->getNumDataSetThreads();

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

	uint64_t currentBlockIdx = startBlock;

	// iterate over global block range for this worker thread
	// (note: "global block range" means that different blocks can refer to different files)
	while(currentBlockIdx < endBlock)
	{
		// find the file index and inner file block index for current global block index
		const uint64_t currentFileIndex = currentBlockIdx / numBlocksPerFile;
		fileHandles.fdVec[0] = pathFDs[currentFileIndex];
		fileHandles.cuFileHandleDataPtrVec[0] = &(cuFileHandleDataVec[currentFileIndex]);

		const uint64_t currentBlockInFile = currentBlockIdx % numBlocksPerFile;
		const uint64_t currentIOStart = currentBlockInFile * blockSize;

		// calc byte offset in file and range length
		const uint64_t remainingWorkerLen = (endBlock - currentBlockIdx) * blockSize;
		const uint64_t remainingFileLen = fileSize - (currentBlockInFile * blockSize);
		const uint64_t currentIOLen = std::min(remainingWorkerLen, remainingFileLen);

		// prep offset generator for current file range
		rwOffsetGen->reset(currentIOLen, currentIOStart);

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
					"Expected written: " + std::to_string(currentIOLen) );
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
					"Expected read: " + std::to_string(currentIOLen) );
		}

		// calc completed number of blocks to inc for next loop pass
		const uint64_t numBlocksDone = (currentIOLen / blockSize) +
			( (currentIOLen % blockSize) ? 1 : 0);

		LOGGER_DEBUG_BUILD("  w" << workerRank << " f" << currentFileIndex <<
			" b" << currentBlockInFile << " " <<
			currentIOStart << " - " << (currentIOStart+currentIOLen) << std::endl);

		currentBlockIdx += numBlocksDone;

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
 * Iterate over all buckets to create or remove them. Each worker processes its own subset of
 * buckets.
 *
 * @throw WorkerException on error.
 */
void LocalWorker::s3ModeIterateBuckets()
{
#ifndef S3_SUPPORT
	throw WorkerException(std::string(__func__) + "called, but this was built without S3 support");
#else

	const StringVec& bucketVec = progArgs->getBenchPaths();
	const size_t numBuckets = bucketVec.size();
	const bool ignoreDelErrors = progArgs->getIgnoreDelErrors();
	const size_t numDataSetThreads = progArgs->getNumDataSetThreads();


	for(unsigned bucketIndex = workerRank;
		bucketIndex < numBuckets;
		bucketIndex += numDataSetThreads)
	{
		checkInterruptionRequest();

		std::chrono::steady_clock::time_point ioStartT = std::chrono::steady_clock::now();

		// create buckets
		if(benchPhase == BenchPhase_CREATEDIRS)
		{
			S3::CreateBucketRequest request;
			request.SetBucket(bucketVec[bucketIndex] );

			S3::CreateBucketOutcome createOutcome = s3Client->CreateBucket(request);

			if(!createOutcome.IsSuccess() )
			{
				auto s3Error = createOutcome.GetError();

				// bucket already existing is not an error
				if( (s3Error.GetErrorType() != Aws::S3::S3Errors::BUCKET_ALREADY_OWNED_BY_YOU) &&
					(s3Error.GetErrorType() != Aws::S3::S3Errors::BUCKET_ALREADY_EXISTS) )
				{
					throw WorkerException(std::string("Bucket creation failed. ") +
						"Endpoint: " + s3EndpointStr + "; "
						"Bucket: " + bucketVec[bucketIndex] + "; "
						"Exception: " + s3Error.GetExceptionName() + "; " +
						"Message: " + s3Error.GetMessage() );
				}
			}
		}

		// delete buckets
		if(benchPhase == BenchPhase_DELETEDIRS)
		{
			S3::DeleteBucketRequest request;
			request.SetBucket(bucketVec[bucketIndex] );

			S3::DeleteBucketOutcome deleteOutcome = s3Client->DeleteBucket(request);

			if(!deleteOutcome.IsSuccess() )
			{
				auto s3Error = deleteOutcome.GetError();

				if( (s3Error.GetErrorType() != Aws::S3::S3Errors::NO_SUCH_BUCKET) ||
					!ignoreDelErrors)
				{
					throw WorkerException(std::string("Bucket deletion failed. ") +
						"Bucket: " + bucketVec[bucketIndex] + "; "
						"Exception: " + s3Error.GetExceptionName() + "; " +
						"Message: " + s3Error.GetMessage() );
				}
			}
		}

		// calc entry operations latency
		std::chrono::steady_clock::time_point ioEndT = std::chrono::steady_clock::now();
		std::chrono::microseconds ioElapsedMicroSec =
			std::chrono::duration_cast<std::chrono::microseconds>
			(ioEndT - ioStartT);

		entriesLatHisto.addLatency(ioElapsedMicroSec.count() );

		atomicLiveOps.numEntriesDone++;
	}

#endif // S3_SUPPORT
}

/**
 * This is for s3 mode. Iterate over all objects to create/read/remove them.
 * By default, this uses a unique "dir" (i.e. prefix with slashes inside a bucket) per worker and
 * fills up each dir before moving on to the next. If dir sharing is enabled, all workers will use
 * dirs or rank 0.
 *
 * @throw WorkerException on error.
 */
void LocalWorker::s3ModeIterateObjects()
{
#ifndef S3_SUPPORT
	throw WorkerException(std::string(__func__) + "called, but this was built without S3 support");
#else

	if( (benchPhase == BenchPhase_READFILES) && progArgs->getUseS3RandObjSelect() )
	{
		s3ModeIterateObjectsRand();
		return;
	}

	const bool haveSubdirs = (progArgs->getNumDirs() > 0);
	const size_t numDirs = haveSubdirs ? progArgs->getNumDirs() : 1; // set 1 to run dir loop once
	const size_t numFiles = progArgs->getNumFiles();
	const uint64_t fileSize = progArgs->getFileSize();
	const size_t blockSize = progArgs->getBlockSize();
	const StringVec& bucketVec = progArgs->getBenchPaths();
	std::array<char, PATH_BUF_LEN> currentPath;
	const size_t workerDirRank = progArgs->getDoDirSharing() ? 0 : workerRank; /* for dir sharing,
		all workers use the dirs of worker rank 0 */
	const bool useTransMan = progArgs->getUseS3TransferManager();
	std::string objectPrefix = progArgs->getS3ObjectPrefix();
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
				throw WorkerException("object path too long for static buffer. "
					"Buffer size: " + std::to_string(PATH_BUF_LEN) + "; "
					"workerRank: " + std::to_string(workerRank) + "; "
					"dirIndex: " + std::to_string(dirIndex) + "; "
					"fileIndex: " + std::to_string(fileIndex) );

			unsigned bucketIndex = (workerRank + dirIndex) % bucketVec.size();
			std::string currentObjectPath = objectPrefix + currentPath.data();

			rwOffsetGen->reset(); // reset for next file

			std::chrono::steady_clock::time_point ioStartT = std::chrono::steady_clock::now();

			if( (benchPhase == BenchPhase_CREATEFILES) && !isRWMixedReader)
			{
				if(blockSize < fileSize)
					s3ModeUploadObjectMultiPart(bucketVec[bucketIndex], currentObjectPath);
				else
					s3ModeUploadObjectSinglePart(bucketVec[bucketIndex], currentObjectPath);
			}

			if( (benchPhase == BenchPhase_READFILES) || isRWMixedReader)
			{
				if(useTransMan)
					s3ModeDownloadObjectTransMan(bucketVec[bucketIndex], currentObjectPath,
						isRWMixedReader);
				else
					s3ModeDownloadObject(bucketVec[bucketIndex], currentObjectPath,
						isRWMixedReader);
			}

			if(benchPhase == BenchPhase_STATFILES)
				s3ModeStatObject(bucketVec[bucketIndex], currentObjectPath);

			if(benchPhase == BenchPhase_DELETEFILES)
				s3ModeDeleteObject(bucketVec[bucketIndex], currentObjectPath);

			// calc entry operations latency. (for create, this includes open/rw/close.)
			std::chrono::steady_clock::time_point ioEndT = std::chrono::steady_clock::now();
			std::chrono::microseconds ioElapsedMicroSec =
				std::chrono::duration_cast<std::chrono::microseconds>
				(ioEndT - ioStartT);

			// entry lat & num done count
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

#endif // S3_SUPPORT
}

/**
 * This is for s3 mode and only valid for reads. Randomly selects the next object and does one
 * random offset read within each object. Number of ops is defined by ProgArgs::randomAmount.
 *
 * This inits all of the used random generators (for offset, dir index, file index) internally.
 *
 * @throw WorkerException on error.
 */
void LocalWorker::s3ModeIterateObjectsRand()
{
#ifndef S3_SUPPORT
	throw WorkerException(std::string(__func__) + "called, but this was built without S3 support");
#else

	const bool haveSubdirs = (progArgs->getNumDirs() > 0);
	const size_t numDirs = haveSubdirs ? progArgs->getNumDirs() : 1; // set 1 to run dir loop once
	const size_t numFiles = progArgs->getNumFiles();
	const uint64_t fileSize = progArgs->getFileSize();
	const size_t blockSize = progArgs->getBlockSize();
	const StringVec& bucketVec = progArgs->getBenchPaths();
	std::array<char, PATH_BUF_LEN> currentPath;
	const size_t workerDirRank = progArgs->getDoDirSharing() ? 0 : workerRank; /* for dir sharing,
		all workers use the dirs of worker rank 0 */
	std::string objectPrefix = progArgs->getS3ObjectPrefix();

	// init random generators for dir & file index selection

	OffsetGenRandom randDirIndexGen(~(uint64_t)0, *randOffsetAlgo, numDirs, 0, 1);
	OffsetGenRandom randFileIndexGen(~(uint64_t)0, *randOffsetAlgo, numFiles, 0, 1);

	// init random offset gen for one read per file

	const uint64_t randomAmount = progArgs->getRandomAmount() / progArgs->getNumDataSetThreads();
	if(progArgs->getUseRandomAligned() ) // random aligned
	{
		rwOffsetGen = std::make_unique<OffsetGenRandomAligned>(blockSize, *randOffsetAlgo,
			fileSize, 0, blockSize);
	}
	else // random unaligned
		rwOffsetGen = std::make_unique<OffsetGenRandom>(blockSize, *randOffsetAlgo,
			fileSize, 0, blockSize);

	// randomly select objects and do one random offset read from each

	uint64_t numBytesDone = 0;
	uint64_t interruptCheckBytes = blockSize * INTERRUPTION_CHECK_INTERVAL;

	while(numBytesDone < randomAmount)
	{
		// occasional interruption check
		IF_UNLIKELY( (numBytesDone % interruptCheckBytes) == 0)
			checkInterruptionRequest();

		const size_t dirIndex = randDirIndexGen.getNextOffset();
		const size_t fileIndex = randFileIndexGen.getNextOffset();
		const uint64_t currentBlockSize = rwOffsetGen->getNextBlockSizeToSubmit();

		// generate current dir path
		int printRes;

		if(haveSubdirs)
			printRes = snprintf(currentPath.data(), PATH_BUF_LEN, "r%zu/d%zu/r%zu-f%zu",
				workerDirRank, dirIndex, workerRank, fileIndex);
		else
			printRes = snprintf(currentPath.data(), PATH_BUF_LEN, "r%zu-f%zu",
				workerRank, fileIndex);

		IF_UNLIKELY(printRes >= PATH_BUF_LEN)
			throw WorkerException("object path too long for static buffer. "
				"Buffer size: " + std::to_string(PATH_BUF_LEN) + "; "
				"workerRank: " + std::to_string(workerRank) + "; "
				"dirIndex: " + std::to_string(dirIndex) + "; "
				"fileIndex: " + std::to_string(fileIndex) );

		const unsigned bucketIndex = (workerRank + dirIndex) % bucketVec.size();
		std::string currentObjectPath = objectPrefix + currentPath.data();

		s3ModeDownloadObject(bucketVec[bucketIndex], currentObjectPath, false);

		rwOffsetGen->reset(); // reset for next rand read op
		numBytesDone += currentBlockSize;
	}


#endif // S3_SUPPORT
}


/**
 * This is for s3 mode with custom tree. Iterate over all objects to create/read/remove them. Each
 * worker uses a subset of the files from the non-shared tree and parts of files from the shared
 * tree.
 *
 * Note: With a custom tree, multiple benchmark paths are not supported. This is a limitation of
 * dirModeIterateCustomFiles() and we keep the same limitation here for compatibility.
 *
 * @throw WorkerException on error.
 */
void LocalWorker::s3ModeIterateCustomObjects()
{
#ifndef S3_SUPPORT
	throw WorkerException(std::string(__func__) + "called, but this was built without S3 support");
#else

	const std::string bucketName = progArgs->getBenchPaths()[0];
	const size_t blockSize = progArgs->getBlockSize();
	const PathList& customTreePaths = customTreeFiles.getPaths();
	const bool useTransMan = progArgs->getUseS3TransferManager();
	const BenchPhase globalBenchPhase = workersSharedData->currentBenchPhase;
	const size_t localWorkerRank = workerRank - progArgs->getRankOffset();
	const bool isRWMixedReader = ( (globalBenchPhase == BenchPhase_CREATEFILES) &&
		(localWorkerRank < progArgs->getNumRWMixReadThreads() ) );

	unsigned short numFilesDone = 0; // just for occasional interruption check (so "short" is ok)

	// walk over custom tree part of this worker

	for(const PathStoreElem& currentPathElem : customTreePaths)
	{
		// occasional interruption check
		if( (numFilesDone % INTERRUPTION_CHECK_INTERVAL) == 0)
			checkInterruptionRequest();

		std::chrono::steady_clock::time_point ioStartT = std::chrono::steady_clock::now();

		if( (benchPhase == BenchPhase_CREATEFILES) || (benchPhase == BenchPhase_READFILES) )
		{
			const uint64_t fileSize = currentPathElem.totalLen;
			const uint64_t fileOffset = currentPathElem.rangeStart;
			const uint64_t rangeLen = currentPathElem.rangeLen;

			rwOffsetGen->reset(rangeLen, fileOffset);

			if(benchPhase == BenchPhase_CREATEFILES)
			{
				if(rangeLen < fileSize)
					s3ModeUploadObjectMultiPartShared(bucketName, currentPathElem.path, fileSize);
				else
				{ // this worker uploads the whole object
					if(blockSize < fileSize)
						s3ModeUploadObjectMultiPart(bucketName, currentPathElem.path);
					else
						s3ModeUploadObjectSinglePart(bucketName, currentPathElem.path);
				}
			}

			if(benchPhase == BenchPhase_READFILES)
			{
				if(useTransMan)
					s3ModeDownloadObjectTransMan(bucketName, currentPathElem.path, false);
				else
					s3ModeDownloadObject(bucketName, currentPathElem.path, false);
			}
		}

		if(benchPhase == BenchPhase_STATFILES)
			s3ModeStatObject(bucketName, currentPathElem.path);

		if(benchPhase == BenchPhase_DELETEFILES)
			s3ModeDeleteObject(bucketName, currentPathElem.path);

		// calc entry operations latency. (for create, this includes open/rw/close.)
		std::chrono::steady_clock::time_point ioEndT = std::chrono::steady_clock::now();
		std::chrono::microseconds ioElapsedMicroSec =
			std::chrono::duration_cast<std::chrono::microseconds>
			(ioEndT - ioStartT);

		// entry lat & num done count
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

#endif // S3_SUPPORT
}

/**
 * Singlepart upload of an S3 object to an existing bucket. This assumes that progArgs fileSize
 * is not larger than blockSize. Or in other words: This can only upload objects consisting of a
 * single block.
 *
 * @throw WorkerException on error.
 */
void LocalWorker::s3ModeUploadObjectSinglePart(std::string bucketName, std::string objectName)
{
#ifndef S3_SUPPORT
	throw WorkerException(std::string(__func__) + "called, but this was built without S3 support");
#else

	const uint64_t currentOffset = rwOffsetGen->getNextOffset();
	const size_t blockSize = rwOffsetGen->getNextBlockSizeToSubmit();

	std::shared_ptr<Aws::IOStream> s3MemStream;
	Aws::Utils::Stream::PreallocatedStreamBuf streamBuf(
		(unsigned char*) (blockSize ? ioBufVec[0] : NULL), blockSize);

	if(blockSize)
		s3MemStream = std::make_shared<S3MemoryStream>(&streamBuf);

	std::chrono::steady_clock::time_point ioStartT = std::chrono::steady_clock::now();

	if(blockSize)
	{
		((*this).*funcRWRateLimiter)(blockSize);
		((*this).*funcPreWriteBlockModifier)(ioBufVec[0], blockSize, currentOffset); // fill buffer
		((*this).*funcPreWriteCudaMemcpy)(ioBufVec[0], gpuIOBufVec[0], blockSize);
	}

	S3::PutObjectRequest request;
	request.WithBucket(bucketName)
		.WithKey(objectName)
		.WithContentLength(blockSize);

	if(blockSize)
		request.SetBody(s3MemStream);

	request.SetDataSentEventHandler(
		[&](const Aws::Http::HttpRequest* request, long long numBytes)
		{ atomicLiveOps.numBytesDone += numBytes; } );

	request.SetContinueRequestHandler( [&](const Aws::Http::HttpRequest* request)
		{ return !isInterruptionRequested.load(); } );

	S3::PutObjectOutcome outcome = s3Client->PutObject(request);

	checkInterruptionRequest(); // (placed here to avoid outcome check on interruption)

	IF_UNLIKELY(!outcome.IsSuccess() )
	{
		auto s3Error = outcome.GetError();

		throw WorkerException(std::string("Object upload failed. ") +
			"Endpoint: " + s3EndpointStr + "; "
			"Bucket: " + bucketName + "; "
			"Object: " + objectName + "; "
			"Exception: " + s3Error.GetExceptionName() + "; " +
			"Message: " + s3Error.GetMessage() );
	}

	if(blockSize)
	{
		((*this).*funcPostReadCudaMemcpy)(ioBufVec[0], gpuIOBufVec[0], blockSize);
		((*this).*funcPostReadBlockChecker)(ioBufVec[0], blockSize, currentOffset); // verify buf
	}

	// calc io operation latency
	std::chrono::steady_clock::time_point ioEndT = std::chrono::steady_clock::now();
	std::chrono::microseconds ioElapsedMicroSec =
		std::chrono::duration_cast<std::chrono::microseconds>
		(ioEndT - ioStartT);

	iopsLatHisto.addLatency(ioElapsedMicroSec.count() );

	numIOPSSubmitted++;
	rwOffsetGen->addBytesSubmitted(blockSize);
	atomicLiveOps.numIOPSDone++;

#endif // S3_SUPPORT
}

/**
 * Block-sized multipart upload of an S3 object to an existing bucket.
 *
 * @throw WorkerException on error.
 */
void LocalWorker::s3ModeUploadObjectMultiPart(std::string bucketName, std::string objectName)
{
#ifndef S3_SUPPORT
	throw WorkerException(std::string(__func__) + "called, but this was built without S3 support");
#else

	// S T E P 1: retrieve multipart upload ID from server

	S3::CreateMultipartUploadRequest createMultipartUploadRequest;
	createMultipartUploadRequest.SetBucket(bucketName);
	createMultipartUploadRequest.SetKey(objectName);

	auto createMultipartUploadOutcome = s3Client->CreateMultipartUpload(
		createMultipartUploadRequest);

	IF_UNLIKELY(!createMultipartUploadOutcome.IsSuccess() )
	{
		auto s3Error = createMultipartUploadOutcome.GetError();

		throw WorkerException(std::string("Multipart upload creation failed. ") +
			"Endpoint: " + s3EndpointStr + "; "
			"Bucket: " + bucketName + "; "
			"Exception: " + s3Error.GetExceptionName() + "; " +
			"Message: " + s3Error.GetMessage() );
	}

	Aws::String uploadID = createMultipartUploadOutcome.GetResult().GetUploadId();

	S3::CompletedMultipartUpload completedMultipartUpload;

	// S T E P 2: upload one block-sized part in each loop pass

	while(rwOffsetGen->getNumBytesLeftToSubmit() )
	{
		const size_t blockSize = rwOffsetGen->getNextBlockSizeToSubmit();
		const uint64_t currentOffset = rwOffsetGen->getNextOffset();
		const uint64_t currentPartNum =
			1 + (currentOffset / rwOffsetGen->getBlockSize() ); // +1 because valid range is 1..10K

		/* note: streamBuf needs to be initialized in loop to have the exact remaining blockSize.
		 	 otherwise the AWS SDK will sent full streamBuf len despite smaller contentLength */
		Aws::Utils::Stream::PreallocatedStreamBuf streamBuf(
			(unsigned char*) ioBufVec[0], blockSize);
		std::shared_ptr<Aws::IOStream> s3MemStream = std::make_shared<S3MemoryStream>(&streamBuf);

		std::chrono::steady_clock::time_point ioStartT = std::chrono::steady_clock::now();

		((*this).*funcRWRateLimiter)(blockSize);
		((*this).*funcPreWriteBlockModifier)(ioBufVec[0], blockSize, currentOffset); // fill buffer
		((*this).*funcPreWriteCudaMemcpy)(ioBufVec[0], gpuIOBufVec[0], blockSize);

		// prepare part upload

		S3::UploadPartRequest uploadPartRequest;
		uploadPartRequest.WithBucket(bucketName)
			.WithKey(objectName)
			.WithUploadId(uploadID)
			.WithPartNumber(currentPartNum)
			.WithContentLength(blockSize);
		uploadPartRequest.SetBody(s3MemStream);

		uploadPartRequest.SetDataSentEventHandler(
			[&](const Aws::Http::HttpRequest* request, long long numBytes)
			{ atomicLiveOps.numBytesDone += numBytes; } );

		uploadPartRequest.SetContinueRequestHandler( [&](const Aws::Http::HttpRequest* request)
			{ return !isInterruptionRequested.load(); } );

		/* start part upload (note: "callable" returns a future to the op so that it can be executed
			in parallel to other requests) */
		auto uploadPartOutcomeCallable = s3Client->UploadPartCallable(uploadPartRequest);

		/* note: there is no way to tell the server about the offset of a part within an object, so
			concurrent part uploads might add overhead on completion for the S3 server to assemble
			the full object in correct parts order. */

		// wait for part upload to finish
		S3::UploadPartOutcome uploadPartOutcome = uploadPartOutcomeCallable.get();

		checkInterruptionRequest( // (placed here to avoid outcome check on interruption)
			[&] { s3AbortMultipartUpload(bucketName, objectName, uploadID); } );

		IF_UNLIKELY(!uploadPartOutcome.IsSuccess() )
		{
			s3AbortMultipartUpload(bucketName, objectName, uploadID);

			auto s3Error = uploadPartOutcome.GetError();

			throw WorkerException(std::string("Multipart part upload failed. ") +
				"Endpoint: " + s3EndpointStr + "; "
				"Bucket: " + bucketName + "; "
				"Object: " + objectName + "; "
				"Part: " + std::to_string(currentPartNum) + "; "
				"Exception: " + s3Error.GetExceptionName() + "; " +
				"Message: " + s3Error.GetMessage() );
		}

		// mark part as completed

		S3::CompletedPart completedPart;
		completedPart.SetPartNumber(currentPartNum);
		auto partETag = uploadPartOutcome.GetResult().GetETag();
		completedPart.SetETag(partETag);

		completedMultipartUpload.AddParts(completedPart);

		((*this).*funcPostReadCudaMemcpy)(ioBufVec[0], gpuIOBufVec[0], blockSize);
		((*this).*funcPostReadBlockChecker)(ioBufVec[0], blockSize, currentOffset); // verify buf

		// calc io operation latency
		std::chrono::steady_clock::time_point ioEndT = std::chrono::steady_clock::now();
		std::chrono::microseconds ioElapsedMicroSec =
			std::chrono::duration_cast<std::chrono::microseconds>
			(ioEndT - ioStartT);

		iopsLatHisto.addLatency(ioElapsedMicroSec.count() );

		numIOPSSubmitted++;
		rwOffsetGen->addBytesSubmitted(blockSize);
		atomicLiveOps.numIOPSDone++;
	}

	// S T E P 3: submit upload completion

	if(progArgs->getDoReverseSeqOffsets() || getS3ModeDoReverseSeqFallback() )
	{ // we need to reverse the parts vector for ascending order
		const Aws::Vector<S3::CompletedPart>& reversePartsVec = completedMultipartUpload.GetParts();
		Aws::Vector<S3::CompletedPart> forwardPartsVec(reversePartsVec.size() );

		std::reverse_copy(std::begin(reversePartsVec), std::end(reversePartsVec),
			std::begin(forwardPartsVec) );

		completedMultipartUpload.SetParts(forwardPartsVec);
	}

	S3::CompleteMultipartUploadRequest completionRequest;
	completionRequest.WithBucket(bucketName)
		.WithKey(objectName)
		.WithUploadId(uploadID)
		.WithMultipartUpload(completedMultipartUpload);

	auto completionOutcome = s3Client->CompleteMultipartUpload(
			completionRequest);

	IF_UNLIKELY(!completionOutcome.IsSuccess() )
	{
		s3AbortMultipartUpload(bucketName, objectName, uploadID);

		auto s3Error = completionOutcome.GetError();

		throw WorkerException(std::string("Multipart upload completion failed. ") +
			"Endpoint: " + s3EndpointStr + "; "
			"Bucket: " + bucketName + "; "
			"Object: " + objectName + "; "
			"NumParts: " + std::to_string(completedMultipartUpload.GetParts().size() ) + "; "
			"Exception: " + s3Error.GetExceptionName() + "; " +
			"Message: " + s3Error.GetMessage() );
	}

#endif // S3_SUPPORT
}

/**
 * Block-sized multipart upload of an S3 object to an existing bucket, shared by multiple workers.
 *
 * @throw WorkerException on error.
 */
void LocalWorker::s3ModeUploadObjectMultiPartShared(std::string bucketName, std::string objectName,
	uint64_t objectTotalSize)
{
#ifndef S3_SUPPORT
	throw WorkerException(std::string(__func__) + "called, but this was built without S3 support");
#else

	// S T E P 1: retrieve multipart upload ID from server

	Aws::String uploadID = s3SharedUploadStore.getMultipartUploadID(
		bucketName, objectName, s3Client);

	std::unique_ptr<Aws::Vector<S3::CompletedPart>> allCompletedParts; // need sort before send

	// S T E P 2: upload one block-sized part in each loop pass

	while(rwOffsetGen->getNumBytesLeftToSubmit() )
	{
		const size_t blockSize = rwOffsetGen->getNextBlockSizeToSubmit();
		const uint64_t currentOffset = rwOffsetGen->getNextOffset();
		const uint64_t currentPartNum =
			1 + (currentOffset / rwOffsetGen->getBlockSize() ); // +1 because valid range is 1..10K

		/* note: streamBuf needs to be initialized in loop to have the exact remaining blockSize.
		 	 otherwise the AWS SDK will sent full streamBuf len despite smaller contentLength */
		Aws::Utils::Stream::PreallocatedStreamBuf streamBuf(
			(unsigned char*) ioBufVec[0], blockSize);
		std::shared_ptr<Aws::IOStream> s3MemStream = std::make_shared<S3MemoryStream>(&streamBuf);

		std::chrono::steady_clock::time_point ioStartT = std::chrono::steady_clock::now();

		((*this).*funcRWRateLimiter)(blockSize);
		((*this).*funcPreWriteBlockModifier)(ioBufVec[0], blockSize, currentOffset); // fill buffer
		((*this).*funcPreWriteCudaMemcpy)(ioBufVec[0], gpuIOBufVec[0], blockSize);

		// prepare part upload

		S3::UploadPartRequest uploadPartRequest;
		uploadPartRequest.WithBucket(bucketName)
			.WithKey(objectName)
			.WithUploadId(uploadID)
			.WithPartNumber(currentPartNum)
			.WithContentLength(blockSize);
		uploadPartRequest.SetBody(s3MemStream);

		uploadPartRequest.SetDataSentEventHandler(
			[&](const Aws::Http::HttpRequest* request, long long numBytes)
			{ atomicLiveOps.numBytesDone += numBytes; } );

		uploadPartRequest.SetContinueRequestHandler( [&](const Aws::Http::HttpRequest* request)
			{ return !isInterruptionRequested.load(); } );

		/* start part upload (note: "callable" returns a future to the op so that it can be executed
			in parallel to other requests) */
		auto uploadPartOutcomeCallable = s3Client->UploadPartCallable(uploadPartRequest);

		/* note: there is no way to tell the server about the offset of a part within an object, so
			concurrent part uploads might add overhead on completion for the S3 server to assemble
			the full object in correct parts order. */

		// wait for part upload to finish
		S3::UploadPartOutcome uploadPartOutcome = uploadPartOutcomeCallable.get();

		checkInterruptionRequest(); /* (placed here to avoid outcome check on interruption; abort
			message will be sent during s3SharedUploadStore cleanup) */

		IF_UNLIKELY(!uploadPartOutcome.IsSuccess() )
		{
			// (note: abort message will be sent during s3SharedUploadStore cleanup)

			auto s3Error = uploadPartOutcome.GetError();

			throw WorkerException(std::string("Shared multipart part upload failed. ") +
				"Endpoint: " + s3EndpointStr + "; "
				"Bucket: " + bucketName + "; "
				"Object: " + objectName + "; "
				"Part: " + std::to_string(currentPartNum) + "; "
				"UploadID: " + uploadID + "; "
				"Rank: " + std::to_string(workerRank) + "; "
				"Exception: " + s3Error.GetExceptionName() + "; " +
				"Message: " + s3Error.GetMessage() );
		}

		// mark part as completed

		S3::CompletedPart completedPart;
		completedPart.SetPartNumber(currentPartNum);
		auto partETag = uploadPartOutcome.GetResult().GetETag();
		completedPart.SetETag(partETag);

		allCompletedParts = s3SharedUploadStore.addCompletedPart(
			bucketName, objectName, blockSize, objectTotalSize, completedPart);

		((*this).*funcPostReadCudaMemcpy)(ioBufVec[0], gpuIOBufVec[0], blockSize);
		((*this).*funcPostReadBlockChecker)(ioBufVec[0], blockSize, currentOffset); // verify buf

		// calc io operation latency
		std::chrono::steady_clock::time_point ioEndT = std::chrono::steady_clock::now();
		std::chrono::microseconds ioElapsedMicroSec =
			std::chrono::duration_cast<std::chrono::microseconds>
			(ioEndT - ioStartT);

		iopsLatHisto.addLatency(ioElapsedMicroSec.count() );

		numIOPSSubmitted++;
		rwOffsetGen->addBytesSubmitted(blockSize);
		atomicLiveOps.numIOPSDone++;

		// sanity check afer rwOffsetGen update
		IF_UNLIKELY(allCompletedParts && rwOffsetGen->getNumBytesLeftToSubmit() )
			throw WorkerException(std::string("Shared multipart upload logic error. ") +
				"Completion returned by upload store, but bytes left to submit. "
				"Endpoint: " + s3EndpointStr + "; "
				"Bucket: " + bucketName + "; "
				"Object: " + objectName + "; "
				"Part: " + std::to_string(currentPartNum) + "; "
				"BytesLeft: " + std::to_string(rwOffsetGen->getNumBytesLeftToSubmit() ) + "; " +
				"ObjectSize: " + std::to_string(objectTotalSize) );
	}

	// S T E P 3: submit upload completion

	if(!allCompletedParts)
		return; // another worker still needs to upload parts, so no completion yet

	// (note: part vec must be in ascending order)
	std::sort(allCompletedParts->begin(), allCompletedParts->end(),
		[](const S3::CompletedPart& a, const S3::CompletedPart& b) -> bool
		{ return a.GetPartNumber() < b.GetPartNumber(); } );

	S3::CompletedMultipartUpload completedMultipartUpload;
	completedMultipartUpload.SetParts(*allCompletedParts);

	S3::CompleteMultipartUploadRequest completionRequest;
	completionRequest.WithBucket(bucketName)
		.WithKey(objectName)
		.WithUploadId(uploadID)
		.WithMultipartUpload(completedMultipartUpload);

	auto completionOutcome = s3Client->CompleteMultipartUpload(
			completionRequest);

	IF_UNLIKELY(!completionOutcome.IsSuccess() )
	{
		s3AbortMultipartUpload(bucketName, objectName, uploadID);

		auto s3Error = completionOutcome.GetError();

		throw WorkerException(std::string("Shared multipart upload completion failed. ") +
			"Endpoint: " + s3EndpointStr + "; "
			"Bucket: " + bucketName + "; "
			"Object: " + objectName + "; "
			"NumParts: " + std::to_string(completedMultipartUpload.GetParts().size() ) + "; "
			"Exception: " + s3Error.GetExceptionName() + "; " +
			"Message: " + s3Error.GetMessage() );
	}

#endif // S3_SUPPORT
}

/**
 * Abort an incomplete S3 multipart upload.
 *
 * @return true if abort request succeeded, false otherwise.
 */
bool LocalWorker::s3AbortMultipartUpload(std::string bucketName, std::string objectName,
	std::string uploadID)
{
#ifndef S3_SUPPORT
	throw WorkerException(std::string(__func__) + "called, but this was built without S3 support");
#else

	S3::AbortMultipartUploadRequest abortMultipartUploadRequest;

	abortMultipartUploadRequest.SetBucket(bucketName);
	abortMultipartUploadRequest.SetKey(objectName);
	abortMultipartUploadRequest.SetUploadId(uploadID);

	auto abortOutcome = s3Client->AbortMultipartUpload(abortMultipartUploadRequest);

	return abortOutcome.IsSuccess();

#endif // S3_SUPPORT
}

/**
 * Abort unfinished shared multipart uploads e.g. after interruption or error. This does not throw
 * an exception on error, because it's intended for the cleanup after a previous error or
 * interruption.
 */
void LocalWorker::s3ModeAbortUnfinishedSharedUploads()
{
#ifdef S3_SUPPORT

	// loop until no more unfinished objects are returned
	for( ; ; )
	{
		std::string bucketName;
		std::string objectName;
		std::string uploadID;

		s3SharedUploadStore.getNextUnfinishedUpload(bucketName, objectName, uploadID);

		if(uploadID.empty() )
			return; // no unfinished uploads left to abort

		LOGGER(Log_DEBUG, "Aborting unfinished shared multipart upload. "
			"Rank: " << workerRank << "; "
			"Endpoint: " << s3EndpointStr << "; "
			"Bucket: " << bucketName << "; "
			"Object: " << objectName << "; " << std::endl);

		bool abortSuccess = s3AbortMultipartUpload(bucketName, objectName, uploadID);

		if(abortSuccess)
			continue;

		// aborting unfinished upload failed
		ERRLOGGER(Log_NORMAL, "Aborting unfinished shared multipart upload failed. "
			"Rank: " << workerRank << "; "
			"Endpoint: " << s3EndpointStr << "; "
			"Bucket: " << bucketName << "; "
			"Object: " << objectName << "; "
			"UploadID: " << uploadID << "; " << std::endl);
	}

#endif // S3_SUPPORT
}

/**
 * Block-sized download of an S3 object.
 *
 * @isRWMixedReader true if this is a reader of a mixed read/write phase, so that the corresponding
 * 		statistics get increased.
 *
 * @throw WorkerException on error.
 */
void LocalWorker::s3ModeDownloadObject(std::string bucketName, std::string objectName,
	const bool isRWMixedReader)
{
#ifndef S3_SUPPORT
	throw WorkerException(std::string(__func__) + "called, but this was built without S3 support");
#else

	const bool useS3FastRead = progArgs->getUseS3FastRead();

	// download one block-sized chunk in each loop pass
	while(rwOffsetGen->getNumBytesLeftToSubmit() )
	{
		const uint64_t currentOffset = rwOffsetGen->getNextOffset();
		const size_t blockSize = rwOffsetGen->getNextBlockSizeToSubmit();

		std::string objectRange = "bytes=" + std::to_string(currentOffset) + "-" +
			std::to_string(currentOffset+blockSize-1);

		char* ioBuf = useS3FastRead ? NULL : ioBufVec[0];
		char* gpuIOBuf = useS3FastRead ? NULL : gpuIOBufVec[0];
		Aws::Utils::Stream::PreallocatedStreamBuf streamBuf( (unsigned char*)ioBuf, blockSize);

		std::chrono::steady_clock::time_point ioStartT = std::chrono::steady_clock::now();

		((*this).*funcRWRateLimiter)(blockSize);
		((*this).*funcPreWriteBlockModifier)(ioBuf, blockSize, currentOffset); // fill buffer
		((*this).*funcPreWriteCudaMemcpy)(ioBuf, gpuIOBuf, blockSize);

		S3::GetObjectRequest request;
		request.WithBucket(bucketName)
			.WithKey(objectName)
			.WithRange(objectRange);

		if(!useS3FastRead)
			request.SetResponseStreamFactory( [&]() { return new S3MemoryStream(&streamBuf); } );
		else
			request.SetResponseStreamFactory( [&]()
			{
				return new Aws::FStream("/dev/null", std::ios_base::out | std::ios_base::binary);
			} );

		request.SetDataReceivedEventHandler(
			[&](const Aws::Http::HttpRequest* request, Aws::Http::HttpResponse* response,
			long long numBytes)
			{
				if(isRWMixedReader)
					atomicLiveOpsReadMix.numBytesDone += numBytes;
				else
					atomicLiveOps.numBytesDone += numBytes;
			} );

		request.SetContinueRequestHandler( [&](const Aws::Http::HttpRequest* request)
			{ return !isInterruptionRequested.load(); } );

		S3::GetObjectOutcome outcome = s3Client->GetObject(request);

		checkInterruptionRequest(); // (placed here to avoid outcome check on interruption)

		IF_UNLIKELY(!outcome.IsSuccess() )
		{
			auto s3Error = outcome.GetError();

			throw WorkerException(std::string("Object download failed. ") +
				"Endpoint: " + s3EndpointStr + "; "
				"Bucket: " + bucketName + "; "
				"Object: " + objectName + "; "
				"Range: " + objectRange + "; "
				"Exception: " + s3Error.GetExceptionName() + "; " +
				"Message: " + s3Error.GetMessage() );
		}

		IF_UNLIKELY( (size_t)outcome.GetResult().GetContentLength() < blockSize)
		{
			throw WorkerException(std::string("Object too small. ") +
				"Endpoint: " + s3EndpointStr + "; "
				"Bucket: " + bucketName + "; "
				"Object: " + objectName + "; "
				"Offset: " + std::to_string(currentOffset) + "; "
				"Requested blocksize: " + std::to_string(blockSize) + "; "
				"Received length: " + std::to_string(outcome.GetResult().GetContentLength() ) );
		}

		((*this).*funcPostReadCudaMemcpy)(ioBuf, gpuIOBuf, blockSize);
		((*this).*funcPostReadBlockChecker)(ioBuf, blockSize, currentOffset); // verify buf

		// calc io operation latency
		std::chrono::steady_clock::time_point ioEndT = std::chrono::steady_clock::now();
		std::chrono::microseconds ioElapsedMicroSec =
			std::chrono::duration_cast<std::chrono::microseconds>
			(ioEndT - ioStartT);

		if(isRWMixedReader)
		{
			iopsLatHistoReadMix.addLatency(ioElapsedMicroSec.count() );
			atomicLiveOpsReadMix.numIOPSDone++;
		}
		else
		{
			iopsLatHisto.addLatency(ioElapsedMicroSec.count() );
			atomicLiveOps.numIOPSDone++;
		}

		numIOPSSubmitted++;
		rwOffsetGen->addBytesSubmitted(blockSize);
	}

#endif // S3_SUPPORT
}

/**
 * Chunked download of an S3 object via AWS SDK Transfer Manager.
 *
 * Downloads to "/dev/null" instead of memory buffer, so no post-processing like data verification
 * or GPU transfer possible. But iodepth greater 1 is supported, translating to multiple
 * TransferManager threads.
 *
 * @isRWMixedReader true if this is a reader of a mixed read/write phase, so that the corresponding
 *		statistics get increased.
 *
 * @throw WorkerException on error.
 */
void LocalWorker::s3ModeDownloadObjectTransMan(std::string bucketName, std::string objectName,
	const bool isRWMixedReader)
{
#ifndef S3_SUPPORT
	throw WorkerException(std::string(__func__) + "called, but this was built without S3 support");
#else

	const uint64_t fileOffset = rwOffsetGen->getNextOffset(); // offset within object to download
	const uint64_t downloadBytes = rwOffsetGen->getNumBytesLeftToSubmit();
	const size_t ioDepth = progArgs->getIODepth();
	auto threadExecutor = std::make_unique<Aws::Utils::Threading::PooledThreadExecutor>(ioDepth);
	uint64_t numBytesDownloaded = 0;
	std::shared_ptr<Aws::Transfer::TransferHandle> transferHandle;

	Aws::Transfer::TransferManagerConfiguration transferConfig(threadExecutor.get() );
	transferConfig.s3Client = s3Client;

	transferConfig.downloadProgressCallback = [&](const Aws::Transfer::TransferManager*,
		const std::shared_ptr<const Aws::Transfer::TransferHandle> &handle)
		{
			if(isInterruptionRequested)
				transferHandle->Cancel();

			const uint64_t numBytesDone = handle->GetBytesTransferred() - numBytesDownloaded;

			if(isRWMixedReader)
				atomicLiveOpsReadMix.numBytesDone += numBytesDone;
			else
				atomicLiveOps.numBytesDone += numBytesDone;

			numBytesDownloaded = handle->GetBytesTransferred();
		};

	auto transferManager = Aws::Transfer::TransferManager::Create(transferConfig);

	std::chrono::steady_clock::time_point ioStartT = std::chrono::steady_clock::now();

	transferHandle = transferManager->DownloadFile(
		bucketName, objectName, fileOffset, downloadBytes, [&]()
		{ return new Aws::FStream("/dev/null", std::ios_base::out | std::ios_base::binary); },
		Aws::Transfer::DownloadConfiguration(), "/dev/null");

	transferHandle->WaitUntilFinished();

	checkInterruptionRequest(); // (placed here to avoid outcome check on interruption)

	IF_UNLIKELY(transferHandle->GetStatus() != Aws::Transfer::TransferStatus::COMPLETED)
	{
		throw WorkerException(std::string("Object download failed. ") +
			"Endpoint: " + s3EndpointStr + "; "
			"Bucket: " + bucketName + "; "
			"Object: " + objectName + "; "
			"Requested size: " + std::to_string(downloadBytes) + "; "
			"Received length: " + std::to_string(transferHandle->GetBytesTransferred() ) );
	}

	IF_UNLIKELY(transferHandle->GetBytesTransferred() != downloadBytes)
	{
		throw WorkerException(std::string("Object too small. ") +
			"Endpoint: " + s3EndpointStr + "; "
			"Bucket: " + bucketName + "; "
			"Object: " + objectName + "; "
			"Requested size: " + std::to_string(downloadBytes) + "; "
			"Received length: " + std::to_string(transferHandle->GetBytesTransferred() ) );
	}

	// calc io operation latency
	std::chrono::steady_clock::time_point ioEndT = std::chrono::steady_clock::now();
	std::chrono::microseconds ioElapsedMicroSec =
		std::chrono::duration_cast<std::chrono::microseconds>
		(ioEndT - ioStartT);

	if(isRWMixedReader)
	{
		iopsLatHistoReadMix.addLatency(ioElapsedMicroSec.count() );
		atomicLiveOpsReadMix.numIOPSDone++;
	}
	else
	{
		iopsLatHisto.addLatency(ioElapsedMicroSec.count() );
		atomicLiveOps.numIOPSDone++;
	}

	numIOPSSubmitted++;
	rwOffsetGen->addBytesSubmitted(downloadBytes);

#endif // S3_SUPPORT
}

/**
 * Retrieve object metadata by sending a HeadObject request (the equivalent of a stat() call in the
 * file world).
 *
 * @throw WorkerException on error.
 */
void LocalWorker::s3ModeStatObject(std::string bucketName, std::string objectName)
{
#ifndef S3_SUPPORT
	throw WorkerException(std::string(__func__) + "called, but this was built without S3 support");
#else

	S3::HeadObjectRequest request;
	request.WithBucket(bucketName)
		.WithKey(objectName);

	S3::HeadObjectOutcome outcome = s3Client->HeadObject(request);

	IF_UNLIKELY(!outcome.IsSuccess() )
	{
		auto s3Error = outcome.GetError();

		throw WorkerException(std::string("Object metadata retrieval via HeadObject failed. ") +
			"Endpoint: " + s3EndpointStr + "; "
			"Bucket: " + bucketName + "; "
			"Object: " + objectName + "; "
			"Exception: " + s3Error.GetExceptionName() + "; " +
			"Message: " + s3Error.GetMessage() );
	}

#endif // S3_SUPPORT
}

/**
 * Delete given S3 object.
 *
 * @throw WorkerException on error.
 */
void LocalWorker::s3ModeDeleteObject(std::string bucketName, std::string objectName)
{
#ifndef S3_SUPPORT
	throw WorkerException(std::string(__func__) + "called, but this was built without S3 support");
#else

	S3::DeleteObjectRequest request;
	request.WithBucket(bucketName)
		.WithKey(objectName);

	S3::DeleteObjectOutcome outcome = s3Client->DeleteObject(request);

	IF_UNLIKELY(!outcome.IsSuccess() )
	{
		auto s3Error = outcome.GetError();

		throw WorkerException(std::string("Object deletion failed. ") +
			"Endpoint: " + s3EndpointStr + "; "
			"Bucket: " + bucketName + "; "
			"Object: " + objectName + "; "
			"Exception: " + s3Error.GetExceptionName() + "; " +
			"Message: " + s3Error.GetMessage() );
	}

#endif // S3_SUPPORT
}

/**
 * List objects in given buckets with user-defined limit for number of entries.
 *
 * @throw WorkerException on error.
 */
void LocalWorker::s3ModeListObjects()
{
#ifndef S3_SUPPORT
	throw WorkerException(std::string(__func__) + "called, but this was built without S3 support");
#else

	const StringVec& bucketVec = progArgs->getBenchPaths();
	const size_t numBuckets = bucketVec.size();
	const size_t numDataSetThreads = progArgs->getNumDataSetThreads();
	std::string objectPrefix = progArgs->getS3ObjectPrefix();

	for(unsigned bucketIndex = workerRank;
		bucketIndex < numBuckets;
		bucketIndex += numDataSetThreads)
	{
		uint64_t numObjectsLeft = progArgs->getS3ListObjNum();
		std::string nextContinuationToken;
		bool isTruncated; // true if S3 server reports more objects left to retrieve

		do
		{
			checkInterruptionRequest();

			std::chrono::steady_clock::time_point ioStartT = std::chrono::steady_clock::now();

			S3::ListObjectsV2Request request;
			request.SetBucket(bucketVec[bucketIndex] );
			request.SetPrefix(objectPrefix);
			request.SetMaxKeys( (numObjectsLeft > 1000) ? 1000 : numObjectsLeft); // can't be >1000

			if(!nextContinuationToken.empty() )
				request.SetContinuationToken(nextContinuationToken);

			S3::ListObjectsV2Outcome outcome = s3Client->ListObjectsV2(request);

			IF_UNLIKELY(!outcome.IsSuccess() )
			{
				auto s3Error = outcome.GetError();

				throw WorkerException(std::string("Object listing v2 failed. ") +
					"Endpoint: " + s3EndpointStr + "; "
					"Bucket: " + bucketVec[bucketIndex] + "; "
					"Prefix: " + objectPrefix + "; "
					"ContinuationToken: " + nextContinuationToken + "; "
					"NumObjectsLeft: " + std::to_string(numObjectsLeft) + "; "
					"Exception: " + s3Error.GetExceptionName() + "; " +
					"Message: " + s3Error.GetMessage() );
			}

			// calc entry operations latency
			std::chrono::steady_clock::time_point ioEndT = std::chrono::steady_clock::now();
			std::chrono::microseconds ioElapsedMicroSec =
				std::chrono::duration_cast<std::chrono::microseconds>
				(ioEndT - ioStartT);

			entriesLatHisto.addLatency(ioElapsedMicroSec.count() );

			unsigned keyCount = outcome.GetResult().GetKeyCount();

			atomicLiveOps.numEntriesDone += keyCount;
			numObjectsLeft -= keyCount;

			nextContinuationToken = outcome.GetResult().GetNextContinuationToken();
			isTruncated = outcome.GetResult().GetIsTruncated();

		} while(isTruncated && numObjectsLeft); // end of while numObjectsLeft loop
	}

#endif // S3_SUPPORT
}

/**
 * This is for s3 mode parallel listing of objects. Expects a dataset created via
 * s3ModeIterateObjects() and uses different prefixes per worker thread to parallelize, so that each
 * worker requests the listing of the dirs/objs that it created in s3ModeIterateObjects().
 *
 * @throw WorkerException on error.
 */
void LocalWorker::s3ModeListObjParallel()
{
#ifndef S3_SUPPORT
	throw WorkerException(std::string(__func__) + "called, but this was built without S3 support");
#else

	const bool haveSubdirs = (progArgs->getNumDirs() > 0);
	const size_t numDirs = haveSubdirs ? progArgs->getNumDirs() : 1; // set 1 to run dir loop once
	const size_t numFiles = progArgs->getNumFiles();
	const StringVec& bucketVec = progArgs->getBenchPaths();
	std::array<char, PATH_BUF_LEN> currentPath;
	const size_t workerDirRank = progArgs->getDoDirSharing() ? 0 : workerRank; /* for dir sharing,
		all workers use the dirs of worker rank 0 */
	std::string objectPrefix = progArgs->getS3ObjectPrefix();
	const bool doListObjVerify = progArgs->getDoListObjVerify();


	// walk over each unique dir per worker

	for(size_t dirIndex = 0; dirIndex < numDirs; dirIndex++)
	{
		uint64_t numObjectsLeft = numFiles;
		std::string nextContinuationToken;
		bool isTruncated; // true if S3 server reports more objects left to retrieve
		StringList receivedObjs; // for verification (if requested by user)
		StringSet expectedObjs; // for verification (if requested by user)

		// generate list prefix for current dir
		int printRes;

		if(haveSubdirs)
			printRes = snprintf(currentPath.data(), PATH_BUF_LEN, "r%zu/d%zu/r%zu-",
						workerDirRank, dirIndex, workerRank);
		else
			printRes = snprintf(currentPath.data(), PATH_BUF_LEN, "r%zu-",
						workerRank);

		IF_UNLIKELY(printRes >= PATH_BUF_LEN)
			throw WorkerException("object path too long for static buffer. "
				"Buffer size: " + std::to_string(PATH_BUF_LEN) + "; "
				"workerRank: " + std::to_string(workerRank) + "; "
				"dirIndex: " + std::to_string(dirIndex) );

		unsigned bucketIndex = (workerRank + dirIndex) % bucketVec.size();
		std::string currentListPrefix = objectPrefix + currentPath.data();

		// build list of expected objs in dir for verification. (std::set for alphabetic order)
		for(size_t fileIndex = 0; doListObjVerify && (fileIndex < numFiles); fileIndex++)
		{
			int printRes;

			if(haveSubdirs)
				printRes = snprintf(currentPath.data(), PATH_BUF_LEN, "r%zu/d%zu/r%zu-f%zu",
					workerDirRank, dirIndex, workerRank, fileIndex);
			else
				printRes = snprintf(currentPath.data(), PATH_BUF_LEN, "r%zu-f%zu",
					workerRank, fileIndex);

			IF_UNLIKELY(printRes >= PATH_BUF_LEN)
				throw WorkerException("Verification object path too long for static buffer. "
					"Buffer size: " + std::to_string(PATH_BUF_LEN) + "; "
					"workerRank: " + std::to_string(workerRank) + "; "
					"dirIndex: " + std::to_string(dirIndex) + "; "
					"fileIndex: " + std::to_string(fileIndex) );

			std::string currentObjectPath = objectPrefix + currentPath.data();

			expectedObjs.insert(currentObjectPath);
		}

		// receive listing of current dir
		do
		{
			checkInterruptionRequest();

			std::chrono::steady_clock::time_point ioStartT = std::chrono::steady_clock::now();

			S3::ListObjectsV2Request request;
			request.SetBucket(bucketVec[bucketIndex] );
			request.SetPrefix(currentListPrefix);
			request.SetMaxKeys( (numObjectsLeft > 1000) ? 1000 : numObjectsLeft); // can't be >1000

			if(!nextContinuationToken.empty() )
				request.SetContinuationToken(nextContinuationToken);

			S3::ListObjectsV2Outcome outcome = s3Client->ListObjectsV2(request);

			IF_UNLIKELY(!outcome.IsSuccess() )
			{
				auto s3Error = outcome.GetError();

				throw WorkerException(std::string("Object listing v2 failed. ") +
					"Endpoint: " + s3EndpointStr + "; "
					"Bucket: " + bucketVec[bucketIndex] + "; "
					"Prefix: " + objectPrefix + "; "
					"ContinuationToken: " + nextContinuationToken + "; "
					"NumObjectsLeft: " + std::to_string(numObjectsLeft) + "; "
					"Exception: " + s3Error.GetExceptionName() + "; " +
					"Message: " + s3Error.GetMessage() );
			}

			// calc entry operations latency
			std::chrono::steady_clock::time_point ioEndT = std::chrono::steady_clock::now();
			std::chrono::microseconds ioElapsedMicroSec =
				std::chrono::duration_cast<std::chrono::microseconds>
				(ioEndT - ioStartT);

			entriesLatHisto.addLatency(ioElapsedMicroSec.count() );

			unsigned keyCount = outcome.GetResult().GetKeyCount();

			atomicLiveOps.numEntriesDone += keyCount;
			numObjectsLeft -= keyCount;

			nextContinuationToken = outcome.GetResult().GetNextContinuationToken();
			isTruncated = outcome.GetResult().GetIsTruncated();

			// build list of received keys. (std::list to preserve order; must be alphabetic)
			IF_UNLIKELY(doListObjVerify)
				for(const Aws::S3::Model::Object& obj : outcome.GetResult().GetContents() )
					receivedObjs.push_back(obj.GetKey() );

		} while(isTruncated && numObjectsLeft); // end of while numObjectsLeft in dir loop

		IF_UNLIKELY(doListObjVerify)
			s3ModeVerifyListing(expectedObjs, receivedObjs, bucketVec[bucketIndex],
				currentListPrefix);

	} // end of dirs for-loop

#endif // S3_SUPPORT
}

/**
 * Verify expected and received dir listing. This includes a verification of the entry order
 * inside the listing.
 *
 * @listPrefix the prefix that was used in the object listing request.
 *
 * @throw WorkerException on error (e.g. mismatch between expected and received).
 */
void LocalWorker::s3ModeVerifyListing(StringSet& expectedSet, StringList& receivedList,
	std::string bucketName, std::string listPrefix)
{
#ifndef S3_SUPPORT
	throw WorkerException(std::string(__func__) + "called, but this was built without S3 support");
#else

	if(expectedSet.size() != receivedList.size() )
		throw WorkerException(std::string("Object listing v2 verification failed. ") +
			"Number of expected and number of received entries differ. "
			"Endpoint: " + s3EndpointStr + "; "
			"Bucket: " + bucketName + "; "
			"Prefix: " + listPrefix + "; "
			"NumObjectsExpected: " + std::to_string(expectedSet.size() ) + "; " +
			"NumObjectsReceived: " + std::to_string(receivedList.size() ) );

	uint64_t currentOffset = 0; // offset inside listing

	while(!expectedSet.empty() && !receivedList.empty() )
	{
		if(*expectedSet.begin() == *receivedList.begin() )
		{ // all good with this entry, so delete and move on to the next one
			expectedSet.erase(expectedSet.begin() );
			receivedList.erase(receivedList.begin() );

			currentOffset++;

			continue;
		}

		// entries differ, so verification failed

		throw WorkerException(std::string("Object listing v2 verification failed. ") +
			"Found object differs from expected object at offset. "
			"FoundObject: " + *receivedList.begin() + "; "
			"ExpectedObject: " + *expectedSet.begin() + "; "
			"ListingOffset: " + std::to_string(currentOffset) + "; "
			"Endpoint: " + s3EndpointStr + "; "
			"Bucket: " + bucketName + "; "
			"Prefix: " + listPrefix + "; "
			"NumObjectsExpected: " + std::to_string(expectedSet.size() ) + "; " +
			"NumObjectsReceived: " + std::to_string(receivedList.size() ) );
	}

#endif // S3_SUPPORT
}

/**
 * In S3 mode, decide if we do fallback to reverse upload. This would be the case if this is a write
 * phase and user selected random offsets.
 *
 * @return true if we do fallback to reverse sequential, false otherwise.
 */
bool LocalWorker::getS3ModeDoReverseSeqFallback()
{
	if(progArgs->getUseRandomOffsets() &&
		!progArgs->getS3EndpointsVec().empty() &&
		(benchPhase == BenchPhase_CREATEFILES) )
		return true;

	return false;
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

	if(progArgs->getUseDirectIO() )
		openFlags |= O_DIRECT;

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
 * @fileSize progArgs->getFileSize().
 * @return filedescriptor of open file.
 * @throw WorkerException on error, in which case file is guaranteed to be closed.
 */
int LocalWorker::dirModeOpenAndPrepFile(BenchPhase benchPhase, const IntVec& pathFDs,
		unsigned pathFDsIndex, const char* relativePath, int openFlags, uint64_t fileSize)
{
	int fd = openat(pathFDs[pathFDsIndex], relativePath, openFlags, MKFILE_MODE);

	IF_UNLIKELY(fd == -1)
		throw WorkerException(std::string("File open failed. ") +
			"Path: " + progArgs->getBenchPaths()[pathFDsIndex] + "/" + relativePath + "; "
			"SysErr: " + strerror(errno) );

	// try block to ensure file close on error
	try
	{
		if(benchPhase == BenchPhase_CREATEFILES)
		{
			if(progArgs->getDoTruncToSize() )
			{
				int truncRes = ftruncate(fd, fileSize);
				if(truncRes == -1)
					throw WorkerException("Unable to set file size through ftruncate. "
						"Path: " + progArgs->getBenchPaths()[pathFDsIndex] + "/" + relativePath +
							"; "
						"Size: " + std::to_string(fileSize) + "; "
						"SysErr: " + strerror(errno) );
			}

			if(progArgs->getDoPreallocFile() )
			{
				// (note: posix_fallocate does not set errno.)
				int preallocRes = posix_fallocate(fd, 0, fileSize);
				if(preallocRes != 0)
					throw WorkerException(
						"Unable to preallocate file size through posix_fallocate. "
						"File: " + progArgs->getBenchPaths()[pathFDsIndex] + "/" + relativePath +
							"; "
						"Size: " + std::to_string(fileSize) + "; "
						"SysErr: " + strerror(preallocRes) );
			}
		}

		return fd;
	}
	catch(WorkerException& e)
	{
		int closeRes = close(fd);

		if(closeRes == -1)
			ERRLOGGER(Log_NORMAL, "File close failed. " <<
				"Path: " << progArgs->getBenchPaths()[pathFDsIndex] << "/" << relativePath <<
					"; " <<
				"FD: " << std::to_string(fd) << "; " <<
				"SysErr: " << strerror(errno) << std::endl);

		throw;
	}
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
		return;

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
		return;

	std::string dropCachesPath = "/proc/sys/vm/drop_caches";
	std::string dropCachesValStr = "3"; // "3" to drop page cache, dentries and inodes

	int fd = open(dropCachesPath.c_str(), O_WRONLY);

	if(fd == -1)
		throw WorkerException(std::string("Opening cache drop command file failed. ") +
			"Path: " + dropCachesPath + "; "
			"SysErr: " + strerror(errno) );

	ssize_t writeRes = write(fd, dropCachesValStr.c_str(), dropCachesValStr.size() );

	if(writeRes == -1)
		throw WorkerException(std::string("Writing to cache drop command file failed. ") +
			"Path: " + dropCachesPath + "; "
			"SysErr: " + strerror(errno) );
}
