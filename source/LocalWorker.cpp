#include <libaio.h>
#include "LocalWorker.h"
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


LocalWorker::~LocalWorker()
{
	SAFE_DELETE(offsetGen);

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
 * Entry point for the thread.
 * Kick off the work that this worker has to do. Each phase is sychronized to wait for notification
 * by coordinator.
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

		// signal coordinator that our preparations phase is done
		incNumWorkersDone();
		phaseFinished = true;

		for( ; ; )
		{
			// wait for coordinator to set new bench ID to signal us that we are good to start
			waitForNextPhase(currentBenchID);

			currentBenchID = workersSharedData->currentBenchID;

			initPhaseOffsetGen();
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

					dirModeIterateDirs();
				} break;
				case BenchPhase_CREATEFILES:
				case BenchPhase_READFILES:
				case BenchPhase_DELETEFILES:
				case BenchPhase_STATFILES:
				{
					if(progArgs->getBenchPathType() == BenchPathType_DIR)
						dirModeIterateFiles();
					else
						fileModeIterateFiles();
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

	incNumWorkersDone();

	phaseFinished = true;
}

/**
 * Prepare file range for dirModeIterateFiles and fileModeIterateFiles. This is typically used to
 * initialize the offset generator and to calculate the expected read/write result.
 */
void LocalWorker::getPhaseFileRange(uint64_t& outFileRangeStart, uint64_t& outFileRangeLen)
{
	if(progArgs->getBenchPathType() == BenchPathType_DIR)
	{
		// in dir mode, we have file-per-thread, so each worker thread writes full files

		outFileRangeStart = 0;
		outFileRangeLen = progArgs->getFileSize();
	}
	else
	{
		// in bdev & file mode, paths are shared between workers, so each has its part of the range

		const uint64_t fileSize = progArgs->getFileSize();
		const size_t blockSize = progArgs->getBlockSize();
		const size_t numBlocks = blockSize ? (fileSize / blockSize) : 0;
		const size_t numDataSetThreads = progArgs->getNumDataSetThreads();

		// each worker writes its own range of the file.
		// (note: progArgs ensure that there is at least one full block to write per thread.)
		// (note: we use blockSize here to avoid misalignment.)
		outFileRangeStart = workerRank * blockSize * (numBlocks / numDataSetThreads);

		// end of last worker's range is user-defined file end to avoid rounding problems
		// (note: last worker may get more range than all others, but progArgs warns user about this.)
		size_t fileRangeEnd = (workerRank == (numDataSetThreads - 1) ) ?
			fileSize - 1 :
			( (workerRank+1) * blockSize * (numBlocks / numDataSetThreads) ) - 1;

		outFileRangeLen = 1 + (fileRangeEnd - outFileRangeStart);
	}
}

/**
 * Prepare offset generator for dirModeIterateFiles and fileModeIterateFiles.
 *
 * Note: offsetGen will always be set to an object here (even if phase is not read or write) to
 * prevent extra NULL pointer checks in file/dir loops.
 */
void LocalWorker::initPhaseOffsetGen()
{
	const size_t blockSize = progArgs->getBlockSize();

	// delete offset gen from previous phase
	SAFE_DELETE(offsetGen);

	if(progArgs->getBenchPathType() == BenchPathType_DIR)
	{
		// in dir mode, we have file-per-thread, so each worker thread writes full files

		const uint64_t fileSize = progArgs->getFileSize();

		if(!progArgs->getUseRandomOffsets() )
			offsetGen = new FileOffsetGenSequential(fileSize, 0, blockSize);
		else
		if(progArgs->getUseRandomAligned() )
			offsetGen = new FileOffsetGenRandomAligned(*progArgs, randGen, fileSize, 0, blockSize);
		else // random unaligned
			offsetGen = new FileOffsetGenRandom(*progArgs, randGen, fileSize, 0, blockSize);
	}
	else
	{
		// in bdev & file mode, paths are shared between workers, so each has its part of the range

		uint64_t fileRangeStart;
		uint64_t fileRangeLen;

		getPhaseFileRange(fileRangeStart, fileRangeLen);

		if(!progArgs->getUseRandomOffsets() )
			offsetGen = new FileOffsetGenSequential(fileRangeLen, fileRangeStart, blockSize);
		else
		if(progArgs->getUseRandomAligned() )
			offsetGen = new FileOffsetGenRandomAligned(*progArgs, randGen,
				fileRangeLen, fileRangeStart, blockSize);
		else // random unaligned
			offsetGen = new FileOffsetGenRandom(*progArgs, randGen,
				fileRangeLen, fileRangeStart, blockSize);
	}
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

	if(benchPhase == BenchPhase_CREATEFILES)
	{
		funcRWBlockSized = (ioDepth == 1) ?
			&LocalWorker::rwBlockSized : &LocalWorker::aioBlockSized;

		funcPositionalRW = useCuFileAPI ?
			&LocalWorker::cuFileWriteWrapper : &LocalWorker::pwriteWrapper;
		funcAioRwPrepper = &io_prep_pwrite;

		funcPreWriteCudaMemcpy = (areGPUsGiven && !useCuFileAPI) ?
			&LocalWorker::cudaMemcpyGPUToHost : &LocalWorker::noOpCudaMemcpy;
		funcPostReadCudaMemcpy = &LocalWorker::noOpCudaMemcpy;

		funcPreWriteIntegrityCheck = integrityCheckEnabled ?
			&LocalWorker::preWriteIntegrityCheckFillBuf : &LocalWorker::noOpIntegrityCheck;
		funcPostReadIntegrityCheck = &LocalWorker::noOpIntegrityCheck;
	}
	else // BenchPhase_READFILES (and others which don't use these function pointers)
	{
		funcRWBlockSized = (ioDepth == 1) ?
			&LocalWorker::rwBlockSized : &LocalWorker::aioBlockSized;

		funcPositionalRW = useCuFileAPI ?
			&LocalWorker::cuFileReadWrapper : &LocalWorker::preadWrapper;
		funcAioRwPrepper = &io_prep_pread;

		funcPreWriteCudaMemcpy = &LocalWorker::noOpCudaMemcpy;
		funcPostReadCudaMemcpy = (areGPUsGiven && !useCuFileAPI) ?
			&LocalWorker::cudaMemcpyHostToGPU : &LocalWorker::noOpCudaMemcpy;

		funcPreWriteIntegrityCheck = &LocalWorker::noOpIntegrityCheck;
		funcPostReadIntegrityCheck = integrityCheckEnabled ?
			&LocalWorker::postReadIntegrityCheckVerifyBuf : &LocalWorker::noOpIntegrityCheck;
	}

	// independent of whether current phase is read or write...

	if(useCuFileAPI)
	{
		funcCuFileHandleReg = (benchPathType == BenchPathType_DIR) ?
			&LocalWorker::dirModeCuFileHandleReg : &LocalWorker::fileModeCuFileHandleReg;
		funcCuFileHandleDereg = (benchPathType == BenchPathType_DIR) ?
			&LocalWorker::dirModeCuFileHandleDereg : &LocalWorker::fileModeCuFileHandleDereg;
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
}

/**
 * Loop around pread/pwrite to use user-defined block size instead of full given count in one call.
 * Reads/writes the pre-allocated ioBuf.
 *
 * @positional_rw pread or pwrite as in "man 2 pread"; fd may be shared by multiple threads in
 * 		file/blockdev mode, so pwrite/pread for offset.
 * @return similar to pread/pwrite.
 */
int64_t LocalWorker::rwBlockSized(int fd)
{
	while(offsetGen->getNumBytesLeftToSubmit() )
	{
		uint64_t currentOffset = offsetGen->getNextOffset();
		size_t blockSize = offsetGen->getNextBlockSizeToSubmit();

		std::chrono::steady_clock::time_point ioStartT = std::chrono::steady_clock::now();

		((*this).*funcPreWriteIntegrityCheck)(ioBufVec[0], blockSize, currentOffset); // fill buffer
		((*this).*funcPreWriteCudaMemcpy)(ioBufVec[0], gpuIOBufVec[0], blockSize);

		ssize_t rwRes = ((*this).*funcPositionalRW)(fd, ioBufVec[0], blockSize, currentOffset);

		IF_UNLIKELY(rwRes <= 0)
		{ // unexpected result
			std::cerr << "rw failed: " << "blockSize: " << blockSize << "; " <<
				"currentOffset:" << currentOffset << "; " <<
				"leftToSubmit:" << offsetGen->getNumBytesLeftToSubmit() << "; " <<
				"rank:" << workerRank << std::endl;

			return (rwRes < 0) ?
				rwRes :
				(offsetGen->getNumBytesTotal() - offsetGen->getNumBytesLeftToSubmit() );
		}

		((*this).*funcPostReadCudaMemcpy)(ioBufVec[0], gpuIOBufVec[0], blockSize);
		((*this).*funcPostReadIntegrityCheck)(ioBufVec[0], blockSize, currentOffset); // verify buf

		// calc io operation latency
		std::chrono::steady_clock::time_point ioEndT = std::chrono::steady_clock::now();
		std::chrono::microseconds ioElapsedMicroSec =
			std::chrono::duration_cast<std::chrono::microseconds>
			(ioEndT - ioStartT);

		iopsLatHisto.addLatency(ioElapsedMicroSec.count() );

		offsetGen->addBytesSubmitted(rwRes);
		atomicLiveOps.numBytesDone += rwRes;
		atomicLiveOps.numIOPSDone++;

		checkInterruptionRequest();
	}

	return offsetGen->getNumBytesTotal();
}

/**
 * Loop around libaio read/write to use user-defined block size instead of full file size in one
 * call.
 * Reads/writes the pre-allocated ioBuf. Uses iodepth from progArgs.
 *
 * @aio_rw_prepper io_prep_pwrite or io_prep_read from libaio.
 * @return similar to pread/pwrite.
 */
int64_t LocalWorker::aioBlockSized(int fd)
{
	size_t maxIODepth = progArgs->getIODepth();

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

	// initial seed of io submissions up to ioDepth
	while(offsetGen->getNumBytesLeftToSubmit() && (numPending < maxIODepth) )
	{
		size_t blockSize = offsetGen->getNextBlockSizeToSubmit();
		uint64_t currentOffset = offsetGen->getNextOffset();
		size_t vecIdx = numPending;

		iocbPointerVec[vecIdx] = &iocbVec[vecIdx];

		funcAioRwPrepper(&iocbVec[vecIdx], fd, ioBufVec[vecIdx], blockSize, currentOffset);
		iocbVec[vecIdx].data = (void*)vecIdx; /* the vec index of this request; ioctl.data
						is caller's private data returned after io_getevents as ioEvents[].data */

		ioStartTimeVec[vecIdx] = std::chrono::steady_clock::now();

		((*this).*funcPreWriteIntegrityCheck)(ioBufVec[vecIdx], blockSize, currentOffset); // fill
		((*this).*funcPreWriteCudaMemcpy)(ioBufVec[vecIdx], gpuIOBufVec[vecIdx], blockSize);

		int submitRes = io_submit(ioContext, 1, &iocbPointerVec[vecIdx] );
		IF_UNLIKELY(submitRes != 1)
		{
			io_queue_release(ioContext);

			throw WorkerException(std::string("Async IO submission (io_submit) failed. ") +
				"NumRequests: " + std::to_string(numPending) + "; "
				"ReturnCode: " + std::to_string(submitRes) + "; "
				"SysErr: " + strerror(-initRes) ); // (io_queue_init returns negative errno)
		}

		numPending++;
		offsetGen->addBytesSubmitted(blockSize);
	}

	// wait for submissions to complete and submit new requests if bytes left
	while(numPending)
	{
		ioTimeout.tv_sec = AIO_MAX_WAIT_SEC;
		ioTimeout.tv_nsec = 0;

		int eventsRes = io_getevents(ioContext, 1, AIO_MAX_EVENTS, ioEvents, &ioTimeout);
		IF_UNLIKELY(!eventsRes)
		{ // timeout expired; that's ok, as we set a short timeout to check interruptions
			checkInterruptionRequest();
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

			size_t vecIdx = (size_t)ioEvents[eventIdx].data; // caller's private data is vec index

			((*this).*funcPostReadCudaMemcpy)(ioBufVec[vecIdx], gpuIOBufVec[vecIdx],
				ioEvents[eventIdx].obj->u.c.nbytes);
			((*this).*funcPostReadIntegrityCheck)( (char*)ioEvents[eventIdx].obj->u.c.buf,
				ioEvents[eventIdx].obj->u.c.nbytes, ioEvents[eventIdx].obj->u.c.offset); // verify

			// calc io operation latency
			std::chrono::steady_clock::time_point ioEndT = std::chrono::steady_clock::now();
			std::chrono::microseconds ioElapsedMicroSec =
				std::chrono::duration_cast<std::chrono::microseconds>
				(ioEndT - ioStartTimeVec[vecIdx] );

			iopsLatHisto.addLatency(ioElapsedMicroSec.count() );

			numBytesDone += ioEvents[eventIdx].res;
			atomicLiveOps.numBytesDone += ioEvents[eventIdx].res;
			atomicLiveOps.numIOPSDone++;

			checkInterruptionRequest();

			if(!offsetGen->getNumBytesLeftToSubmit() )
			{
				numPending--;
				continue;
			}

			// request complete, so reuse iocb for the next request...

			size_t blockSize = offsetGen->getNextBlockSizeToSubmit();
			uint64_t currentOffset = offsetGen->getNextOffset();

			funcAioRwPrepper(ioEvents[eventIdx].obj, fd, ioBufVec[vecIdx], blockSize,
				currentOffset);
			ioEvents[eventIdx].obj->data = (void*)vecIdx; // caller's private data

			ioStartTimeVec[vecIdx] = std::chrono::steady_clock::now();

			((*this).*funcPreWriteIntegrityCheck)(ioBufVec[vecIdx], blockSize, currentOffset);
			((*this).*funcPreWriteCudaMemcpy)(ioBufVec[vecIdx], gpuIOBufVec[vecIdx], blockSize);

			int submitRes = io_submit(
				ioContext, 1, &iocbPointerVec[vecIdx] );
			IF_UNLIKELY(submitRes != 1)
			{
				io_queue_release(ioContext);

				throw WorkerException(std::string("Async IO resubmission (io_submit) failed. ") +
					"NumRequests: " + std::to_string(numPending) + "; "
					"ReturnCode: " + std::to_string(submitRes) + "; "
					"SysErr: " + strerror(-initRes) ); // (io_queue_init returns negative errno)
			}

			offsetGen->addBytesSubmitted(blockSize);

		} // end of for loop to resubmit completed iocbs

	} // end of while loop until all blocks completed

	io_queue_release(ioContext);

	return offsetGen->getNumBytesTotal();
}

/**
 * Noop for the case when no integrity check selected by user.
 */
void LocalWorker::noOpIntegrityCheck(char* buf, size_t bufLen, off_t fileOffset)
{
	return; // noop
}

/**
 * Fill buf with 64bit values made of offset plus integrityCheckBase.
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

	/* note: offset and nbytes are not guaranteed to be a multiple of uint64_t (e.g. if blocksize is
	   1B), so to allow reading with different blocksize later, we only copy the relevant part of
	   the uint64_t. */

	while(numBytesLeft)
	{
		/* checksum value is always calculated aligned to 8 byte block size, even if we only copy a
		   partial block. */

		// (note: after the 1st loop pass, the remaining offsets will be 8 byte aligned.)

		// 8 byte aligned offset for checksum value calculation
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
 * Verify buffer contents are according to preWriteIntegrityCheckFullBuf.
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
 * cuFile handle register as preparation for cuFileRead/Write in file/blockdev mode. Call
 * cuFileHandleDereg when done with file access.
 *
 * @fd posix file handle from ProgArgs::getBenchPathFDs that was registered for cuFile access
 * 		by ProgArgs.
 * @outHandleData corresponding cuFileHandleData from ProgArgs::getCuFileHandleDataVec to be
 * 		assigned to cuFileHandleDataPtr for cuFileRead/WriteWrapper.
 */
void LocalWorker::fileModeCuFileHandleReg(int fd, CuFileHandleData& outHandleData)
{
	cuFileHandleDataPtr = &outHandleData;
}

/**
 * Counterpart to cuFileHandleReg to deregister a file handle in file/blockdev mode. This is safe to
 * call even if registration was not called or if it failed. This is actually a noop.
 *
 * @handleData the same that was passed to cuFileHandleReg before.
 */
void LocalWorker::fileModeCuFileHandleDereg(CuFileHandleData& handleData)
{
	return;
}

/**
 * Wrapper for positional sync read.
 */
ssize_t LocalWorker::preadWrapper(int fd, void* buf, size_t nbytes, off_t offset)
{
	return pread(fd, buf, nbytes, offset);
}

/**
 * Wrapper for positional sync write.
 */
ssize_t LocalWorker::pwriteWrapper(int fd, void* buf, size_t nbytes, off_t offset)
{
	return pwrite(fd, buf, nbytes, offset);
}

/**
 * Wrapper for positional sync cuFile read.
 *
 * @fd ignored, using cuFileHandleData.cfr_handle instead.
 * @buf ignored, using gpuIOBuf instead.
 */
ssize_t LocalWorker::cuFileReadWrapper(int fd, void* buf, size_t nbytes, off_t offset)
{
	throw WorkerException("cuFileReadWrapper called, but this executable was built without cuFile "
		"API support");
}

/**
 * Wrapper for positional sync cuFile write.
 *
 * @fd ignored, using cuFileHandleData.cfr_handle instead.
 * @buf ignored, using gpuIOBuf instead.
 */
ssize_t LocalWorker::cuFileWriteWrapper(int fd, void* buf, size_t nbytes, off_t offset)
{
	throw WorkerException("cuFileWriteWrapper called, but this executable was built without cuFile "
		"API support");
}

/**
 * Iterate over all directories to create or remove them.
 *
 * @doMkdir true to create dirs. Existing dir is not an error.
 * @doRmdir true to remove dirs.
 * @throw WorkerException on error.
 */
void LocalWorker::dirModeIterateDirs()
{
	std::array<char, PATH_BUF_LEN> currentPath;
	BenchPhase benchPhase = workersSharedData->currentBenchPhase;
	size_t numDirs = progArgs->getNumDirs();
	const IntVec& pathFDs = progArgs->getBenchPathFDs();
	const StringVec& pathVec = progArgs->getBenchPaths();

	// create rank dir inside each pathFD
	if(benchPhase == BenchPhase_CREATEDIRS)
	{
		for(unsigned pathFDsIndex = 0; pathFDsIndex < pathFDs.size(); pathFDsIndex++)
		{
			// create rank dir for current pathFD...

			checkInterruptionRequest();

			// generate path
			int printRes = snprintf(currentPath.data(), PATH_BUF_LEN, "r%zu", workerRank);
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
			workerRank, dirIndex);
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

			if( (rmdirRes == -1) && ( (errno != ENOENT) || !progArgs->getIgnoreDelErrors() ) )
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
			int printRes = snprintf(currentPath.data(), PATH_BUF_LEN, "r%zu", workerRank);
			if(printRes >= PATH_BUF_LEN)
				throw WorkerException("mkdir path too long for static buffer. "
					"Buffer size: " + std::to_string(PATH_BUF_LEN) + "; "
					"workerRank: " + std::to_string(workerRank) );

			int rmdirRes = unlinkat(pathFDs[pathFDsIndex], currentPath.data(), AT_REMOVEDIR);

			if( (rmdirRes == -1) && ( (errno != ENOENT) || !progArgs->getIgnoreDelErrors() ) )
				throw WorkerException(std::string("Directory deletion failed. ") +
					"Path: " + pathVec[pathFDsIndex] + "/" + currentPath.data() + "; "
					"SysErr: " + strerror(errno) );
		}
	}

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

	cuFileHandleDataPtr = &cuFileHandleData; // prep for cuFileRead/WriteWrapper

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
			int printRes = snprintf(currentPath.data(), PATH_BUF_LEN, "r%zu/d%zu/f%zu",
				workerRank, dirIndex, fileIndex);
			if(printRes >= PATH_BUF_LEN)
				throw WorkerException("mkdir path too long for static buffer. "
					"Buffer size: " + std::to_string(PATH_BUF_LEN) + "; "
					"workerRank: " + std::to_string(workerRank) + "; "
					"dirIndex: " + std::to_string(dirIndex) + "; "
					"fileIndex: " + std::to_string(fileIndex) );

			unsigned pathFDsIndex = (workerRank + dirIndex) % pathFDs.size();

			offsetGen->reset(); // reset for next file

			std::chrono::steady_clock::time_point ioStartT = std::chrono::steady_clock::now();

			if( (benchPhase == BenchPhase_CREATEFILES) || (benchPhase == BenchPhase_READFILES) )
			{
				int fd = openat(pathFDs[pathFDsIndex], currentPath.data(), openFlags, MKFILE_MODE);

				IF_UNLIKELY(fd == -1)
					throw WorkerException(std::string("File open failed. ") +
						"Path: " + pathVec[pathFDsIndex] + "/" + currentPath.data() + "; "
						"SysErr: " + strerror(errno) );

				// try-block to ensure that fd is closed in case of exception
				try
				{
					((*this).*funcCuFileHandleReg)(fd, cuFileHandleData); // reg cuFile handle

					if(benchPhase == BenchPhase_CREATEFILES)
					{
						int64_t writeRes = ((*this).*funcRWBlockSized)(fd);

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
						ssize_t readRes = ((*this).*funcRWBlockSized)(fd);

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
 * This is for file mode. Iterate over all files to create/write or read them.
 *
 * @throw WorkerException on error.
 */
void LocalWorker::fileModeIterateFiles()
{
	const BenchPhase benchPhase = workersSharedData->currentBenchPhase;
	const size_t numFiles = progArgs->getBenchPathFDs().size();
	const uint64_t fileSize = progArgs->getFileSize();
	const size_t blockSize = progArgs->getBlockSize();
	const size_t numDataSetThreads = progArgs->getNumDataSetThreads();
	const IntVec& pathFDs = progArgs->getBenchPathFDs();
	CuFileHandleDataVec& cuFileHandleDataVec = progArgs->getCuFileHandleDataVec();

	uint64_t fileRangeStart;
	uint64_t fileRangeLen;

	getPhaseFileRange(fileRangeStart, fileRangeLen);

	uint64_t fileRangeEnd = fileRangeStart + fileRangeLen - 1; // "-1" for inclusive last offset

	size_t expectedIORes = fileRangeLen;
	if(progArgs->getUseRandomOffsets() )
		expectedIORes = progArgs->getRandomAmount() / numDataSetThreads;

	LOGGER(Log_DEBUG, "fsize: " << fileSize << "; " "bsize: " << blockSize << "; "
		"threads: " << numDataSetThreads << "; " "numFiles: " << numFiles << std::endl);
	LOGGER(Log_DEBUG, "rank: " << workerRank << "; "
		"shared: " << !progArgs->getIsBenchPathNotShared() << "; "
		"range: " << fileRangeStart << " - " << fileRangeEnd << " (" << fileRangeLen << ")" <<
		std::endl);

	// walk over all files and write our range of each

	for(size_t fileIndex = 0; fileIndex < numFiles; fileIndex++)
	{
		// occasional interruption check
		if( (fileIndex % INTERRUPTION_CHECK_INTERVAL) == 0)
			checkInterruptionRequest();

		size_t currentFileIndex = (workerRank + fileIndex) % pathFDs.size();

		// each worker starts with a different file (based on workerRank) to spread the load
		int fd = pathFDs[currentFileIndex];

		((*this).*funcCuFileHandleReg)(fd, cuFileHandleDataVec[currentFileIndex]); // reg handle

		offsetGen->reset(); // reset for next file

		// write/read our range of this file and then move on to the next

		if(benchPhase == BenchPhase_CREATEFILES)
		{
			ssize_t writeRes = ((*this).*funcRWBlockSized)(fd);

			IF_UNLIKELY(writeRes == -1)
				throw WorkerException(std::string("File write failed. ") +
					"Path: " + progArgs->getBenchPaths()[currentFileIndex] + "; "
					"SysErr: " + strerror(errno) );

			IF_UNLIKELY( (size_t)writeRes != expectedIORes)
				throw WorkerException(std::string("Unexpected short file write. ") +
					"Path: " + progArgs->getBenchPaths()[currentFileIndex] + "; "
					"Bytes written: " + std::to_string(writeRes) + "; "
					"Expected written: " + std::to_string(fileRangeLen) );
		}

		if(benchPhase == BenchPhase_READFILES)
		{
			ssize_t readRes = ((*this).*funcRWBlockSized)(fd);

			IF_UNLIKELY(readRes == -1)
				throw WorkerException(std::string("File read failed. ") +
					"Path: " + progArgs->getBenchPaths()[currentFileIndex] + "; "
					"SysErr: " + strerror(errno) );

			IF_UNLIKELY( (size_t)readRes != expectedIORes)
				throw WorkerException(std::string("Unexpected short file read. ") +
					"Path: " + progArgs->getBenchPaths()[currentFileIndex] + "; "
					"Bytes read: " + std::to_string(readRes) + "; "
					"Expected read: " + std::to_string(fileRangeLen) );
		}

		if(benchPhase == BenchPhase_DELETEFILES)
		{
			const StringVec& benchPaths = progArgs->getBenchPaths();
			const std::string& path =
				benchPaths[ (workerRank + fileIndex) % benchPaths.size() ];

			int unlinkRes = unlink(path.c_str() );

			if( (unlinkRes == -1) && ( (errno != ENOENT) || !progArgs->getIgnoreDelErrors() ) )
				throw WorkerException(std::string("File delete failed. ") +
					"Path: " + path + "; "
					"SysErr: " + strerror(errno) );
		}

		((*this).*funcCuFileHandleDereg)(cuFileHandleData); // dereg cuFile handle

	} // end of files for loop

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
