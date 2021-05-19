#include <libaio.h>
#include "LocalWorker.h"
#include "toolkits/random/RandAlgoSelectorTk.h"
#include "WorkerException.h"
#include "WorkersSharedData.h"

#ifdef CUDA_SUPPORT
	#include <cuda_runtime.h>
#endif


#define PATH_BUF_LEN					64
#define MKDIR_MODE						0777
#define MKFILE_MODE						(S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH)
#define INTERRUPTION_CHECK_INTERVAL		128
#define AIO_MAX_WAIT_SEC				5
#define AIO_MAX_EVENTS					4 // max number of events to retrieve in io_getevents()


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
		applyNumaBinding();
		allocIOBuffer();
		allocGPUIOBuffer();
		prepareCustomTreePathStores();

		// signal coordinator that our preparations phase is done
		phaseFinished = true; // before incNumWorkersDone(), as Coordinator can reset after done inc
		incNumWorkersDone();

		for( ; ; )
		{
			// wait for coordinator to set new bench ID to signal us that we are good to start
			waitForNextPhase(currentBenchID);

			currentBenchID = workersSharedData->currentBenchID;

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
						throw WorkerException("Directory creation and deletion are not available "
							"in file and block device mode.");

					progArgs->getTreeFilePath().empty() ?
						dirModeIterateDirs() : dirModeIterateCustomDirs();
				} break;
				case BenchPhase_CREATEFILES:
				case BenchPhase_READFILES:
				{
					if(progArgs->getBenchPathType() == BenchPathType_DIR)
						progArgs->getTreeFilePath().empty() ?
							dirModeIterateFiles() : dirModeIterateCustomFiles();
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
						throw WorkerException("File stat operation not available in file and block "
							"device mode.");

					progArgs->getTreeFilePath().empty() ?
						dirModeIterateFiles() : dirModeIterateCustomFiles();
				} break;
				case BenchPhase_DELETEFILES:
				{
					if(progArgs->getBenchPathType() == BenchPathType_DIR)
						progArgs->getTreeFilePath().empty() ?
							dirModeIterateFiles() : dirModeIterateCustomFiles();
					else
						fileModeDeleteFiles();
				} break;
				case BenchPhase_SYNC:
				{
					anyModeSync();
				} break;
				case BenchPhase_DROPCACHES:
				{
					anyModeDropCaches();
				} break;
				default:
				{ // should never happen
					throw WorkerException("Unknown/invalid next phase type: " +
						std::to_string(workersSharedData->currentBenchPhase) );
				} break;

			} // end of switch

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
			finishPhase(); // let coordinator know that we are done

		return;
	}
	catch(std::exception& e)
	{
		ErrLogger(Log_NORMAL, progArgs->getRunAsService() ) << e.what() << std::endl;
	}

	incNumWorkersDoneWithError();
}

/**
 * Update finish time values, then signal coordinator that we're done.
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
 * Init fileHandle vectors for current phase.
 */
void LocalWorker::initPhaseFileHandleVecs()
{
	const BenchPathType benchPathType = progArgs->getBenchPathType();

	fileHandles.errorFDVecIdx = -1; // clear ("-1" means "not set")

	if(benchPathType == BenchPathType_DIR)
	{
		// there is only one current file per worker in dir mode

		fileHandles.fdVec.resize(1);
		fileHandles.fdVec[0] = -1; // clear/reset

		fileHandles.fdVecPtr = &fileHandles.fdVec;

		// fileHandles.cuFileHandleDataVec will be used for current file

		fileHandles.cuFileHandleDataVec.resize(0); // clear/reset
		fileHandles.cuFileHandleDataVec.resize(1);

		fileHandles.cuFileHandleDataPtrVec.resize(0); // clear/reset
		fileHandles.cuFileHandleDataPtrVec.push_back(&fileHandles.cuFileHandleDataVec[0] );
	}
	else
	if(!progArgs->getUseRandomOffsets() )
	{
		// there is only one current file in sequential file/bdev mode

		// files are opened by progArgs, but FD will be copied to only use the single current file

		fileHandles.fdVec.resize(1);
		fileHandles.fdVec[0] = -1; // clear/reset, will be set dynamically to current file

		fileHandles.fdVecPtr = &fileHandles.fdVec;

		fileHandles.cuFileHandleDataVec.resize(0); // not needed, using cuFileHandle from progArgs

		// fileHandles.cuFileHandleDataPtrVec will be set to progArgs cuFileHandle for current file

		fileHandles.cuFileHandleDataPtrVec.resize(0); // clear/reset
		fileHandles.cuFileHandleDataPtrVec.resize(1); // set dynamically to current file
	}
	else
	{
		// in random file/bdev mode, all FDs from progArgs will be used round-robin

		fileHandles.fdVec.resize(0);
		fileHandles.cuFileHandleDataVec.resize(0);
		fileHandles.cuFileHandleDataPtrVec.resize(0);

		// FDs will be provided by progArgs

		fileHandles.fdVecPtr = &progArgs->getBenchPathFDs();

		// cuFileHandles will be provided by progArgs

		for(size_t i=0; i < progArgs->getCuFileHandleDataVec().size(); i++)
			fileHandles.cuFileHandleDataPtrVec.push_back(&progArgs->getCuFileHandleDataVec()[i] );
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

	// init random algos
	randOffsetAlgo = RandAlgoSelectorTk::stringToAlgo(progArgs->getRandOffsetAlgo() );
	randBlockVarAlgo = RandAlgoSelectorTk::stringToAlgo(progArgs->getBlockVarianceAlgo() );
	randBlockVarReseed = std::make_unique<RandAlgoXoshiro256ss>();

	// note: file/bdev mode sequential is done per-file in fileModeIterateFilesSeq()

	if(!progArgs->getUseRandomOffsets() ) // sequential
		rwOffsetGen = std::make_unique<OffsetGenSequential>(
			fileSize, 0, blockSize);
	else
	if(progArgs->getUseRandomAligned() ) // random aligned
		rwOffsetGen = std::make_unique<OffsetGenRandomAligned>(*progArgs, *randOffsetAlgo,
			fileSize, 0, blockSize);
	else // random unaligned
		rwOffsetGen = std::make_unique<OffsetGenRandom>(*progArgs, *randOffsetAlgo,
			fileSize, 0, blockSize);
}

/**
 * Just set all phase-dependent function pointers to NULL.
 */
void LocalWorker::nullifyPhaseFunctionPointers()
{
	funcRWBlockSized = NULL;
	funcPositionalRW = NULL;
	funcAioRwPrepper = NULL;
	funcPreWriteCudaMemcpy = NULL;
	funcPostReadCudaMemcpy = NULL;
	funcPreWriteCudaMemcpy = NULL;
	funcPreWriteBlockModifier = NULL;
	funcPostReadBlockChecker = NULL;
	funcCuFileHandleReg = NULL;
	funcCuFileHandleDereg = NULL;
}

/**
 * Prepare read/write function pointers for dirModeIterateFiles and fileModeIterateFiles.
 */
void LocalWorker::initPhaseFunctionPointers()
{
	const BenchPhase benchPhase = workersSharedData->currentBenchPhase;
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

	nullifyPhaseFunctionPointers(); // set all function pointers to NULL

	if(benchPhase == BenchPhase_CREATEFILES)
	{
		funcRWBlockSized = (ioDepth == 1) ?
			&LocalWorker::rwBlockSized : &LocalWorker::aioBlockSized;

		funcPositionalRW = useCuFileAPI ?
			&LocalWorker::cuFileWriteWrapper : &LocalWorker::pwriteWrapper;

		if(rwMixPercent && (funcPositionalRW == &LocalWorker::pwriteWrapper) )
			funcPositionalRW = &LocalWorker::pwriteRWMixWrapper;
		else
		if(rwMixPercent && (funcPositionalRW == &LocalWorker::cuFileWriteWrapper) )
			funcPositionalRW = &LocalWorker::cuFileRWMixWrapper;

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
				funcPositionalRW = &LocalWorker::pwriteAndReadWrapper;
			else
			{
				funcPositionalRW = &LocalWorker::cuFileWriteAndReadWrapper;
				funcPostReadCudaMemcpy = &LocalWorker::cudaMemcpyGPUToHost;
			}

			funcPostReadBlockChecker = &LocalWorker::postReadIntegrityCheckVerifyBuf;
		}
	}
	else // BenchPhase_READFILES (and others which don't use these function pointers)
	{
		funcRWBlockSized = (ioDepth == 1) ?
			&LocalWorker::rwBlockSized : &LocalWorker::aioBlockSized;

		funcPositionalRW = useCuFileAPI ?
			&LocalWorker::cuFileReadWrapper : &LocalWorker::preadWrapper;

		funcAioRwPrepper = (ioDepth == 1) ? NULL : &LocalWorker::aioReadPrepper;

		funcPreWriteCudaMemcpy = &LocalWorker::noOpCudaMemcpy;
		funcPostReadCudaMemcpy = (areGPUsGiven && !useCuFileAPI) ?
			&LocalWorker::cudaMemcpyHostToGPU : &LocalWorker::noOpCudaMemcpy;

		if(useCuFileAPI && integrityCheckEnabled)
			funcPostReadCudaMemcpy = &LocalWorker::cudaMemcpyGPUToHost;

		funcPreWriteBlockModifier = &LocalWorker::noOpIntegrityCheck;
		funcPostReadBlockChecker = integrityCheckEnabled ?
			&LocalWorker::postReadIntegrityCheckVerifyBuf : &LocalWorker::noOpIntegrityCheck;
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

	progArgs->getCustomTreeFilesNonShared().getWorkerSublistNonShared(
		workerRank, progArgs->getNumDataSetThreads(), customTreeFiles);

	progArgs->getCustomTreeFilesShared().getWorkerSublistShared(
		workerRank, progArgs->getNumDataSetThreads(), customTreeFiles);

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
			std::cerr << "ERROR: GPU DMA buffer deregistration via cuFileBufDeregister failed. "
				"GPU ID: " << gpuID << "; "
				"cuFile Error: " << CUFILE_ERRSTR(deregRes.err) << std::endl;
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
				std::cerr << "ERROR: CPU DMA buffer deregistration via cudaHostUnregister failed. "
					"GPU ID: " << gpuID << "; "
					"CUDA Error: " << cudaGetErrorString(unregRes) << std::endl;
		}
	}
#endif

	// free host memory buffers
	for(char* ioBuf : ioBufVec)
		SAFE_FREE(ioBuf);
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
	while(rwOffsetGen->getNumBytesLeftToSubmit() )
	{
		const uint64_t currentOffset = rwOffsetGen->getNextOffset();
		const size_t blockSize = rwOffsetGen->getNextBlockSizeToSubmit();
		const size_t fileHandleIdx = numIOPSSubmitted % fileHandles.fdVecPtr->size();

		std::chrono::steady_clock::time_point ioStartT = std::chrono::steady_clock::now();

		((*this).*funcPreWriteBlockModifier)(ioBufVec[0], blockSize, currentOffset); // fill buffer
		((*this).*funcPreWriteCudaMemcpy)(ioBufVec[0], gpuIOBufVec[0], blockSize);

		ssize_t rwRes = ((*this).*funcPositionalRW)(
			fileHandleIdx, ioBufVec[0], blockSize, currentOffset);

		IF_UNLIKELY(rwRes <= 0)
		{ // unexpected result
			std::cerr << "rw failed: " << "blockSize: " << blockSize << "; " <<
				"currentOffset:" << currentOffset << "; " <<
				"leftToSubmit:" << rwOffsetGen->getNumBytesLeftToSubmit() << "; " <<
				"rank:" << workerRank << std::endl;

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

		iopsLatHisto.addLatency(ioElapsedMicroSec.count() );

		numIOPSSubmitted++;
		rwOffsetGen->addBytesSubmitted(rwRes);
		atomicLiveOps.numBytesDone += rwRes;
		atomicLiveOps.numIOPSDone++;

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
 */
int64_t LocalWorker::aioBlockSized()
{
	const size_t maxIODepth = progArgs->getIODepth();

	size_t numPending = 0; // num requests submitted and pending for completion
	size_t numBytesDone = 0; // after successfully completed requests

	io_context_t ioContext = {}; // zeroing required by io_queue_init
	std::vector<struct iocb> iocbVec(maxIODepth);
	std::vector<struct iocb*> iocbPointerVec(maxIODepth);
	std::vector<std::chrono::steady_clock::time_point> ioStartTimeVec(maxIODepth);
	struct io_event ioEvents[AIO_MAX_EVENTS];
	struct timespec ioTimeout;

	int initRes = io_queue_init(maxIODepth, &ioContext);
	IF_UNLIKELY(initRes)
		throw WorkerException(std::string("Initializing async IO (io_queue_init) failed. ") +
			"SysErr: " + strerror(-initRes) ); // (io_queue_init returns negative errno)


	// P H A S E 1: initial seed of io submissions up to full ioDepth

	while(rwOffsetGen->getNumBytesLeftToSubmit() && (numPending < maxIODepth) )
	{
		const size_t blockSize = rwOffsetGen->getNextBlockSizeToSubmit();
		const uint64_t currentOffset = rwOffsetGen->getNextOffset();
		const int fd = (*fileHandles.fdVecPtr)[numIOPSSubmitted % fileHandles.fdVecPtr->size() ];
		const size_t ioVecIdx = numPending; // iocbVec index

		iocbPointerVec[ioVecIdx] = &iocbVec[ioVecIdx];

		((*this).*funcAioRwPrepper)(&iocbVec[ioVecIdx], fd, ioBufVec[ioVecIdx], blockSize,
			currentOffset);
		iocbVec[ioVecIdx].data = (void*)ioVecIdx; /* the vec index of this request; ioctl.data
						is caller's private data returned after io_getevents as ioEvents[].data */

		ioStartTimeVec[ioVecIdx] = std::chrono::steady_clock::now();

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
			// ioEvents[].res2 is positive errno, so 0 means success
			// ioEvents[].res is number of actually read/written bytes when res2==0

			IF_UNLIKELY(ioEvents[eventIdx].res2 ||
				(ioEvents[eventIdx].res != ioEvents[eventIdx].obj->u.c.nbytes) )
			{ // unexpected result
				io_queue_release(ioContext);

				return (ioEvents[eventIdx].res2 != 0) ?
					-ioEvents[eventIdx].res2 :
					(numBytesDone + ioEvents[eventIdx].obj->u.c.nbytes);
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

			iopsLatHisto.addLatency(ioElapsedMicroSec.count() );

			numBytesDone += ioEvents[eventIdx].res;
			atomicLiveOps.numBytesDone += ioEvents[eventIdx].res;
			atomicLiveOps.numIOPSDone++;

			// inc rwmix stats
			if( (funcAioRwPrepper == &LocalWorker::aioRWMixPrepper) &&
				(ioEvents[eventIdx].obj->aio_lio_opcode == IO_CMD_PREAD) )
			{
				atomicLiveRWMixReadOps.numBytesDone += ioEvents[eventIdx].res;
				atomicLiveRWMixReadOps.numIOPSDone++;
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
			const size_t fileHandlesIdx = numIOPSSubmitted % fileHandles.fdVecPtr->size();
			const int fd = (*fileHandles.fdVecPtr)[fileHandlesIdx];

			((*this).*funcAioRwPrepper)(ioEvents[eventIdx].obj, fd, ioBufVec[ioVecIdx], blockSize,
				currentOffset);
			ioEvents[eventIdx].obj->data = (void*)ioVecIdx; // caller's private data

			ioStartTimeVec[ioVecIdx] = std::chrono::steady_clock::now();

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
 * Refill some percentage of buffers with random data. The percentage of buffers to refill is
 * defined in progArgs::blockVariancePercent.
 */
void LocalWorker::preWriteBufRandRefill(char* buf, size_t bufLen, off_t fileOffset)
{
	// example: 40% means we refill 40 out of 100 buffers and the remaining 60 buffers are identical

	/* note: keep in mind that this also needs to work with lots of small files, so percentage needs
		to work between different files. (numIOPSSubmitted ensures that below; numIOPSDone would not
		work for this because aio would not inc counter directly on submission.) */

	// note: workerRank is used to have skew between different worker threads
	if( ( (workerRank + numIOPSSubmitted) % 100) >= progArgs->getBlockVariancePercent() )
		return;

	// refill buffer with random data

	randBlockVarAlgo->fillBuf(buf, bufLen);
}

/**
 * Refill some percentage of buffers with random data. The percentage of buffers to refill is
 * defined in progArgs::blockVariancePercent.
 *
 * Note: The only reason why this function exists separate from preWriteBufRandRefill() which does
 * the same with RandAlgoGoldenPrime::fillBuf() is that test have shown 30% lower perf for
 * "-w -t 1 -b 128k --iodepth 128 --blockvarpct 100 --rand --direct" when the function in the
 * RandAlgo object is called (which is quite mysterious).
 */
void LocalWorker::preWriteBufRandRefillFast(char* buf, size_t bufLen, off_t fileOffset)
{
	// example: 40% means we refill 40 out of 100 buffers and the remaining 60 buffers are identical

	/* note: keep in mind that this also needs to work with lots of small files, so percentage needs
		to work between different files. (numIOPSSubmitted ensures that below; numIOPSDone would not
		work for this because aio would not inc counter directly on submission.) */

	// note: workerRank is used to have skew between different worker threads
	if( ( (workerRank + numIOPSSubmitted) % 100) >= progArgs->getBlockVariancePercent() )
		return;

	// refill buffer with random data

	uint64_t state = randBlockVarReseed->next();

	size_t numBytesDone = 0;

	for(uint64_t i=0; i < (bufLen / sizeof(uint64_t) ); i++)
	{
		uint64_t* uint64Buf = (uint64_t*)buf;
		state *= RANDALGO_GOLDEN_RATIO_PRIME;
		state >>= 3;
		*uint64Buf = state;

		buf += sizeof(uint64_t);
		numBytesDone += sizeof(uint64_t);
	}

	if(numBytesDone == bufLen)
		return; // all done, complete buffer filled

	// we have a remainder to fill, which can only be smaller than sizeof(uint64_t)
	state *= RANDALGO_GOLDEN_RATIO_PRIME;
	state >>= 3;
	uint64_t randUint64 = state;

	memcpy(buf, &randUint64, bufLen - numBytesDone);
}

/**
 * Simple wrapper for io_prep_pwrite().
 */
void LocalWorker::aioWritePrepper(struct iocb* iocb, int fd, void* buf, size_t count,
	long long offset)
{
	io_prep_pwrite(iocb, fd, buf, count, offset);
}

/**
 * Simple wrapper for io_prep_pread().
 */
void LocalWorker::aioReadPrepper(struct iocb* iocb, int fd, void* buf, size_t count,
	long long offset)
{
	io_prep_pread(iocb, fd, buf, count, offset);
}

/**
 * Within a write phase, send user-defined pecentage of block reads for mixed r/w.
 *
 * Parameters are similar to io_prep_p{write,read}.
 */
void LocalWorker::aioRWMixPrepper(struct iocb* iocb, int fd, void* buf, size_t count,
	long long offset)
{
	// example: 40% means 40 out of 100 submitted blocks will be reads, the remaining 60 are writes

	/* note: keep in mind that this also needs to work with lots of small files, so percentage needs
		to work between different files. (numIOPSSubmitted ensures that below; numIOPSDone would not
		work for this because aio would not inc counter directly on submission.) */

	// note: workerRank is used to have skew between different worker threads
	if( ( (workerRank + numIOPSSubmitted) % 100) >= progArgs->getRWMixPercent() )
		io_prep_pwrite(iocb, fd, buf, count, offset);
	else
		io_prep_pread(iocb, fd, buf, count, offset);
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
	{
		ssize_t readRes = pread(fd, buf, nbytes, offset);

		IF_UNLIKELY(readRes <= 0)
			return readRes;

		atomicLiveRWMixReadOps.numBytesDone += readRes;
		atomicLiveRWMixReadOps.numIOPSDone++;

		return readRes;
	}
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
	{
		ssize_t readRes = cuFileRead(fileHandles.cuFileHandleDataPtrVec[fileHandleIdx]->cfr_handle,
			gpuIOBufVec[0], nbytes, offset, 0);

		IF_UNLIKELY(readRes <= 0)
			return readRes;

		atomicLiveRWMixReadOps.numBytesDone += readRes;
		atomicLiveRWMixReadOps.numIOPSDone++;

		return readRes;
	}
#endif
}

/**
 * Iterate over all directories to create or remove them.
 *
 * @throw WorkerException on error.
 */
void LocalWorker::dirModeIterateDirs()
{
	std::array<char, PATH_BUF_LEN> currentPath;
	const BenchPhase benchPhase = workersSharedData->currentBenchPhase;
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
			if(printRes >= PATH_BUF_LEN)
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
		if(printRes >= PATH_BUF_LEN)
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

		// calc entry operations latency. (for create, this includes open/rw/close.)
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
			if(printRes >= PATH_BUF_LEN)
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
	const BenchPhase benchPhase = workersSharedData->currentBenchPhase;
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
 * Uses a unique dir per worker and fills up each dir before moving on to the next.
 *
 * @throw WorkerException on error.
 */
void LocalWorker::dirModeIterateFiles()
{
	const BenchPhase benchPhase = workersSharedData->currentBenchPhase;
	const size_t numDirs = progArgs->getNumDirs();
	const size_t numFiles = progArgs->getNumFiles();
	const uint64_t fileSize = progArgs->getFileSize();
	const IntVec& pathFDs = progArgs->getBenchPathFDs();
	const StringVec& pathVec = progArgs->getBenchPaths();
	const int openFlags = getDirModeOpenFlags(benchPhase);
	std::array<char, PATH_BUF_LEN> currentPath;
	const size_t workerDirRank = progArgs->getDoDirSharing() ? 0 : workerRank; /* for dir sharing,
		all workers use the dirs of worker rank 0 */

	int& fd = fileHandles.fdVec[0];
	CuFileHandleData& cuFileHandleData = fileHandles.cuFileHandleDataVec[0];

	// walk over each unique dir per worker

	for(size_t dirIndex = 0; dirIndex < numDirs; dirIndex++)
	{
		// occasional interruption check
		if( (dirIndex % INTERRUPTION_CHECK_INTERVAL) == 0)
			checkInterruptionRequest();

		// fill up this dir with all files before moving on to the next dir

		for(size_t fileIndex = 0; fileIndex < numFiles; fileIndex++)
		{
			// occasional interruption check
			if( (fileIndex % INTERRUPTION_CHECK_INTERVAL) == 0)
				checkInterruptionRequest();

			// generate current dir path
			int printRes = snprintf(currentPath.data(), PATH_BUF_LEN, "r%zu/d%zu/r%zu-f%zu",
				workerDirRank, dirIndex, workerRank, fileIndex);
			if(printRes >= PATH_BUF_LEN)
				throw WorkerException("mkdir path too long for static buffer. "
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

			entriesLatHisto.addLatency(ioElapsedMicroSec.count() );

			atomicLiveOps.numEntriesDone++;

		} // end of files for loop
	} // end of dirs for loop

}

/**
 * This is for directory mode with custom files. Iterate over all files to create/read/remove them.
 * Each worker uses a subset of the files from the non-shared.
 *
 * Note: With a custom tree, multiple benchmark paths are not supported (because otherwise we
 * 	can't ensure in file creation phase that the matching parent dir has been created for the
 * 	current bench path).
 *
 * @throw WorkerException on error.
 */
void LocalWorker::dirModeIterateCustomFiles()
{
	const BenchPhase benchPhase = workersSharedData->currentBenchPhase;
	const IntVec& benchPathFDs = progArgs->getBenchPathFDs();
	const unsigned benchPathFDIdx = 0; // multiple bench paths not supported with custom tree
	const int benchPathFD = progArgs->getBenchPathFDs()[0];
	const std::string benchPathStr = progArgs->getBenchPaths()[0];
	const int openFlags = getDirModeOpenFlags(benchPhase);
	const size_t blockSize = progArgs->getBlockSize();
	const bool ignoreDelErrors = true; // shared files are unliked by all workers, so no errs
	const PathList& customTreePaths = customTreeFiles.getPaths();

	int& fd = fileHandles.fdVec[0];
	CuFileHandleData& cuFileHandleData = fileHandles.cuFileHandleDataVec[0];

	unsigned short numFilesDone = 0; // just for occasional interruption check (so short is ok)

	// walk over each unique dir per worker

	for(const PathStoreElem& currentPathElem : customTreePaths)
	{
		// occasional interruption check
		if( (numFilesDone % INTERRUPTION_CHECK_INTERVAL) == 0)
			checkInterruptionRequest();

		const char* currentPath = currentPathElem.path.c_str();

		std::chrono::steady_clock::time_point ioStartT = std::chrono::steady_clock::now();

		if( (benchPhase == BenchPhase_CREATEFILES) || (benchPhase == BenchPhase_READFILES) )
		{
			const uint64_t fileSize = currentPathElem.rangeLen;
			const uint64_t fileOffset = currentPathElem.rangeStart;

			rwOffsetGen = std::make_unique<OffsetGenSequential>(
					currentPathElem.rangeLen, fileOffset, blockSize);

			fd = dirModeOpenAndPrepFile(benchPhase, benchPathFDs, benchPathFDIdx,
				currentPathElem.path.c_str(), openFlags, fileSize);

			// try-block to ensure that fd is closed in case of exception
			try
			{
				((*this).*funcCuFileHandleReg)(fd, cuFileHandleData); // reg cuFile handle

				if(benchPhase == BenchPhase_CREATEFILES)
				{
					int64_t writeRes = ((*this).*funcRWBlockSized)();

					IF_UNLIKELY(writeRes == -1)
						throw WorkerException(std::string("File write failed. ") +
							"Path: " + benchPathStr + "/" + currentPath + "; "
							"SysErr: " + strerror(errno) );

					IF_UNLIKELY( (size_t)writeRes != currentPathElem.rangeLen)
						throw WorkerException(std::string("Unexpected short file write. ") +
							"Path: " + benchPathStr + "/" + currentPath + "; "
							"Bytes written: " + std::to_string(writeRes) + "; "
							"Expected written: " + std::to_string(fileSize) );
				}

				if(benchPhase == BenchPhase_READFILES)
				{
					ssize_t readRes = ((*this).*funcRWBlockSized)();

					IF_UNLIKELY(readRes == -1)
						throw WorkerException(std::string("File read failed. ") +
							"Path: " + benchPathStr + "/" + currentPath + "; "
							"SysErr: " + strerror(errno) );

					IF_UNLIKELY( (size_t)readRes != fileSize)
						throw WorkerException(std::string("Unexpected short file read. ") +
							"Path: " + benchPathStr + "/" + currentPath + "; "
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

		entriesLatHisto.addLatency(ioElapsedMicroSec.count() );

		atomicLiveOps.numEntriesDone++;
		numFilesDone++;

	} // end of dirs for loop

}

/**
 * This is for file/bdev mode. Send random I/Os round-robin to all given files across full file
 * range.
 *
 * @throw WorkerException on error.
 */
void LocalWorker::fileModeIterateFilesRand()
{
	const BenchPhase benchPhase = workersSharedData->currentBenchPhase;

	// funcRWBlockSized() will send IOs round-robin to all user-given files.

	if(benchPhase == BenchPhase_CREATEFILES)
	{
		ssize_t writeRes = ((*this).*funcRWBlockSized)();

		IF_UNLIKELY(writeRes == -1)
			throw WorkerException(std::string("File write failed. ") +
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
	const BenchPhase benchPhase = workersSharedData->currentBenchPhase;
	const size_t numFiles = progArgs->getBenchPathFDs().size();
	const uint64_t fileSize = progArgs->getFileSize();
	const size_t blockSize = progArgs->getBlockSize();
	const size_t numThreads = progArgs->getNumDataSetThreads();
	const IntVec& pathFDs = progArgs->getBenchPathFDs();

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
		fileHandles.cuFileHandleDataPtrVec[0] =
			&progArgs->getCuFileHandleDataVec()[currentFileIndex];

		const uint64_t currentBlockInFile = currentBlockIdx % numBlocksPerFile;
		const uint64_t currentIOStart = currentBlockInFile * blockSize;

		// calc byte offset in file and range length
		const uint64_t remainingWorkerLen = (endBlock - currentBlockIdx) * blockSize;
		const uint64_t remainingFileLen = fileSize - (currentBlockInFile * blockSize);
		const uint64_t currentIOLen = std::min(remainingWorkerLen, remainingFileLen);

		// prep offset generator for current file range
		rwOffsetGen = std::make_unique<OffsetGenSequential>(
			currentIOLen, currentIOStart, blockSize);

		// write/read our range of this file

		if(benchPhase == BenchPhase_CREATEFILES)
		{
			ssize_t writeRes = ((*this).*funcRWBlockSized)();

			IF_UNLIKELY(writeRes == -1)
				throw WorkerException(std::string("File write failed. ") +
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
	const size_t numFiles = progArgs->getBenchPathFDs().size();

	// walk over all files and delete each of them
	// (note: each worker starts with a different file (based on workerRank) to spread the load)

	for(size_t fileIndex = 0; fileIndex < numFiles; fileIndex++)
	{
		// occasional interruption check
		if( (fileIndex % INTERRUPTION_CHECK_INTERVAL) == 0)
			checkInterruptionRequest();

		// delete current file

		const std::string& path =
			benchPaths[ (workerRank + fileIndex) % benchPaths.size() ];

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
 * @throw WorkerException on error.
 */
void LocalWorker::anyModeSync()
{
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
}

/**
 * Prints 3 to /proc/sys/vm/drop_caches to drop cached data from the Linux page cache.
 *
 * @throw WorkerException on error.
 */
void LocalWorker::anyModeDropCaches()
{
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
