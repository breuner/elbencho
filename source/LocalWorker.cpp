#include <libaio.h>
#include "LocalWorker.h"
#include "WorkerException.h"
#include "WorkersSharedData.h"


#define PATH_BUF_LEN					64
#define MKDIR_MODE						0777
#define MKFILE_MODE						(S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH)
#define INTERRUPTION_CHECK_INTERVAL		128
#define AIO_MAX_WAIT_SEC				5
#define AIO_MAX_EVENTS					4 // max number of events to retrieve in io_getevents()


LocalWorker::~LocalWorker()
{
	if(ioBuf)
		free(ioBuf);
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

		// signal coordinator that our preparations phase is done
		incNumWorkersDone();
		phaseFinished = true;

		for( ; ; )
		{
			// wait for coordinator to set new bench ID to signal us that we are good to start
			waitForNextPhase(currentBenchID);

			currentBenchID = workersSharedData->currentBenchID;

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

					iterateDirs();
				} break;
				case BenchPhase_CREATEFILES:
				case BenchPhase_READFILES:
				case BenchPhase_DELETEFILES:
				{
					if(progArgs->getBenchPathType() == BenchPathType_DIR)
						dirModeIterateFiles();
					else
						fileModeIterateFiles();
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

	std::chrono::milliseconds elapsedDurationMS =
		std::chrono::duration_cast<std::chrono::milliseconds>
		(now - workersSharedData->phaseStartT);
	size_t finishElapsedMS = elapsedDurationMS.count();

	elapsedMSVec.resize(1);
	elapsedMSVec[0] = finishElapsedMS;

	incNumWorkersDone();

	phaseFinished = true;
}

/**
 * Allocate aligned I/O buffer and fill with random data.
 *
 * @throw WorkerException if allocation fails.
 */
void LocalWorker::allocIOBuffer()
{
	if(!progArgs->getBlockSize() )
	{
		ioBuf = NULL;
		return;
	}

	// alloc I/O buffer appropriately aligned for O_DIRECT
	int allocAlignedRes = posix_memalign(&ioBuf, sysconf(_SC_PAGESIZE),
			progArgs->getBlockSize() );

	if(allocAlignedRes)
		throw WorkerException("Aligned memory allocation failed. "
			"Buffer size: " + std::to_string(progArgs->getBlockSize() ) + "; "
			"Page size: " + std::to_string(sysconf(_SC_PAGESIZE) ) + "; "
			"SysErr: " + strerror(allocAlignedRes) ); // yes, not errno here

	// fill buffer with random data
	unsigned seed = 0;
	int* intIOBuf = (int*)ioBuf;
	for(size_t i=0; i < (progArgs->getBlockSize() / sizeof(unsigned) ); i++)
		intIOBuf[i] = rand_r(&seed);
}

/**
 * Loop around pread/pwrite to use user-defined block size instead of full given count in one call.
 * Reads/writes the pre-allocated ioBuf.
 *
 * @positional_rw pread or pwrite as in "man 2 pread"; fd may be shared by multiple threads in
 * 		file/blockdev mode, so pwrite/pread for offset.
 * @return similar to pread/pwrite.
 */
template <POSITIONAL_RW positional_rw>
ssize_t LocalWorker::rwBlockSized(int fd, FileOffsetGenerator& offsetGen)
{
	while(offsetGen.getNumBytesLeftToSubmit() )
	{
		size_t currentOffset = offsetGen.getNextOffset();
		size_t blockSize = offsetGen.getNextBlockSizeToSubmit();

		std::chrono::steady_clock::time_point ioStartT = std::chrono::steady_clock::now();

		ssize_t rwRes = positional_rw(fd, ioBuf, blockSize, currentOffset);

		IF_UNLIKELY(rwRes <= 0)
		{ // unexpected result
			std::cerr << "rw failed: " << "blockSize: " << blockSize << "; " <<
				"currentOffset:" << currentOffset << "; " <<
				"leftToSubmit:" << offsetGen.getNumBytesLeftToSubmit() << "; " <<
				"rank:" << workerRank << std::endl; // todo delme

			return (rwRes < 0) ?
				rwRes :
				(offsetGen.getNumBytesTotal() - offsetGen.getNumBytesLeftToSubmit() );
		}

		// calc io operation latency
		std::chrono::steady_clock::time_point ioEndT = std::chrono::steady_clock::now();
		std::chrono::microseconds ioElapsedMicroSec =
			std::chrono::duration_cast<std::chrono::microseconds>
			(ioEndT - ioStartT);

		iopsLatHisto.addLatency(ioElapsedMicroSec.count() );

		offsetGen.addBytesSubmitted(rwRes);
		atomicLiveOps.numBytesDone += rwRes;
		atomicLiveOps.numIOPSDone++;

		checkInterruptionRequest();
	}

	return offsetGen.getNumBytesTotal();
}

/**
 * Loop around libaio read/write to use user-defined block size instead of full file size in one
 * call.
 * Reads/writes the pre-allocated ioBuf. Uses iodepth from progArgs.
 *
 * @aio_rw_prepper io_prep_pwrite or io_prep_read from libaio.
 * @return similar to pwrite()
 */
template <AIO_RW_PREPPER aio_rw_prepper>
ssize_t LocalWorker::aioBlockSized(int fd, FileOffsetGenerator& offsetGen)
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
	while(offsetGen.getNumBytesLeftToSubmit() && (numPending < maxIODepth) )
	{
		size_t blockSize = offsetGen.getNextBlockSizeToSubmit();
		size_t currentOffset = offsetGen.getNextOffset();
		size_t vecIdx = numPending;

		iocbPointerVec[vecIdx] = &iocbVec[vecIdx];

		aio_rw_prepper(&iocbVec[vecIdx], fd, ioBuf, blockSize, currentOffset);
		iocbVec[vecIdx].data = (void*)vecIdx; /* the vec index of this request; ioctl.data
						is caller's private data returned after io_getevents as ioEvents[].data */

		ioStartTimeVec[vecIdx] = std::chrono::steady_clock::now();

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
		offsetGen.addBytesSubmitted(blockSize);
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

			if(!offsetGen.getNumBytesLeftToSubmit() )
			{
				numPending--;
				continue;
			}

			// request complete, so reuse iocb for the next request...

			size_t blockSize = offsetGen.getNextBlockSizeToSubmit();
			size_t currentOffset = offsetGen.getNextOffset();

			ioStartTimeVec[vecIdx] = std::chrono::steady_clock::now();

			aio_rw_prepper(ioEvents[eventIdx].obj, fd, ioBuf, blockSize, currentOffset);
			ioEvents[eventIdx].obj->data = (void*)vecIdx; // caller's private data

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

			offsetGen.addBytesSubmitted(blockSize);

		} // end of for loop to resubmit completed iocbs

	} // end of while loop until all blocks completed

	io_queue_release(ioContext);

	return offsetGen.getNumBytesTotal();
}

/**
 * Iterate over all directories to create or remove them.
 *
 * @doMkdir true to create dirs. Existing dir is not an error.
 * @doRmdir true to remove dirs.
 * @throw WorkerException on error.
 */
void LocalWorker::iterateDirs()
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
	const size_t fileSize = progArgs->getFileSize();
	const size_t blockSize = progArgs->getBlockSize();
	const IntVec& pathFDs = progArgs->getBenchPathFDs();
	const StringVec& pathVec = progArgs->getBenchPaths();
	const size_t ioDepth = progArgs->getIODepth();
	const int openFlags = getDirModeOpenFlags(benchPhase);
	std::array<char, PATH_BUF_LEN> currentPath;

	// walk over each unique dir per worker

	for(size_t dirIndex = 0; dirIndex < numDirs; dirIndex++)
	{
		if( (dirIndex % INTERRUPTION_CHECK_INTERVAL) == 0)
			checkInterruptionRequest();

		// fill up this dir with all files before moving on to the next dir

		for(size_t fileIndex = 0; fileIndex < numFiles; fileIndex++)
		{
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

			std::chrono::steady_clock::time_point ioStartT = std::chrono::steady_clock::now();

			if( (benchPhase == BenchPhase_CREATEFILES) || (benchPhase == BenchPhase_READFILES) )
			{
				int fd = openat(pathFDs[pathFDsIndex], currentPath.data(), openFlags, MKFILE_MODE);

				IF_UNLIKELY(fd == -1)
					throw WorkerException(std::string("File open failed. ") +
						"Path: " + pathVec[pathFDsIndex] + "/" + currentPath.data() + "; "
						"SysErr: " + strerror(errno) );

				// try block to ensure that fd is closed in case of exception
				try
				{
					if(benchPhase == BenchPhase_CREATEFILES)
					{
						FileOffsetGenSequential offsetGen(fileSize, 0, blockSize);

						ssize_t writeRes = (ioDepth == 1) ?
							rwBlockSized<pwriteWrapper>(fd, offsetGen) :
							aioBlockSized<io_prep_pwrite>(fd, offsetGen);

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
						FileOffsetGenSequential offsetGen(fileSize, 0, blockSize);

						ssize_t readRes = (ioDepth == 1) ?
							rwBlockSized<preadWrapper>(fd, offsetGen) :
							aioBlockSized<io_prep_pread>(fd, offsetGen);

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
					close(fd);
					throw;
				}

				int closeRes = close(fd);

				IF_UNLIKELY(closeRes == -1)
					throw WorkerException(std::string("File close failed. ") +
						"Path: " + pathVec[pathFDsIndex] + "/" + currentPath.data() + "; "
						"FD: " + std::to_string(fd) + "; "
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
	const size_t fileSize = progArgs->getFileSize();
	const size_t blockSize = progArgs->getBlockSize();
	const size_t numBlocks = blockSize ? (fileSize / blockSize) : 0;
	const size_t numDataSetThreads = progArgs->getNumDataSetThreads();
	const IntVec& pathFDs = progArgs->getBenchPathFDs();
	const size_t ioDepth = progArgs->getIODepth();


	// each worker writes its own range of the file.
	// (note: progArgs ensure that there is at least one full block to write per thread.)
	// (note: we use blockSize here to avoid misalignment.)
	size_t fileRangeStart = workerRank * blockSize * (numBlocks / numDataSetThreads);

	// end of last worker's range is user-defined file end to avoid rounding problems
	// (note: last worker may get more range than all others, but progArgs warns user about this.)
	size_t fileRangeEnd = (workerRank == (numDataSetThreads - 1) ) ?
		fileSize - 1 :
		( (workerRank+1) * blockSize * (numBlocks / numDataSetThreads) ) - 1;

	size_t fileRangeLen = 1 + (fileRangeEnd - fileRangeStart);

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
		if( (fileIndex % INTERRUPTION_CHECK_INTERVAL) == 0)
			checkInterruptionRequest();

		size_t currentFileIndex = (workerRank + fileIndex) % pathFDs.size();

		// each worker starts with a different file (based on workerRank) to spread the load
		int fd = pathFDs[currentFileIndex];

		// write/read our range of this file and then move on to the next

		if(benchPhase == BenchPhase_CREATEFILES)
		{
			ssize_t writeRes;

			if(!progArgs->getUseRandomOffsets() )
			{
				FileOffsetGenSequential offsetGen(fileRangeLen, fileRangeStart, blockSize);

				writeRes = (ioDepth == 1) ?
					rwBlockSized<pwriteWrapper>(fd, offsetGen) :
					aioBlockSized<io_prep_pwrite>(fd, offsetGen);
			}
			else
			if(progArgs->getUseRandomAligned() )
			{
				FileOffsetGenRandomAligned offsetGen(*progArgs, randGen,
					fileRangeLen, fileRangeStart, blockSize);

				writeRes = (ioDepth == 1) ?
					rwBlockSized<pwriteWrapper>(fd, offsetGen) :
					aioBlockSized<io_prep_pwrite>(fd, offsetGen);
			}
			else
			{
				FileOffsetGenRandom offsetGen(*progArgs, randGen,
					fileRangeLen, fileRangeStart, blockSize);

				writeRes = (ioDepth == 1) ?
					rwBlockSized<pwriteWrapper>(fd, offsetGen) :
					aioBlockSized<io_prep_pwrite>(fd, offsetGen);
			}

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
			ssize_t readRes;

			if(!progArgs->getUseRandomOffsets() )
			{
				FileOffsetGenSequential offsetGen(fileRangeLen, fileRangeStart, blockSize);

				readRes = (ioDepth == 1) ?
					rwBlockSized<preadWrapper>(fd, offsetGen) :
					aioBlockSized<io_prep_pread>(fd, offsetGen);
			}
			else
			if(progArgs->getUseRandomAligned() )
			{
				FileOffsetGenRandomAligned offsetGen(*progArgs, randGen,
					fileRangeLen, fileRangeStart, blockSize);

				readRes = (ioDepth == 1) ?
					rwBlockSized<preadWrapper>(fd, offsetGen) :
					aioBlockSized<io_prep_pread>(fd, offsetGen);
			}
			else
			{
				FileOffsetGenRandom offsetGen(*progArgs, randGen,
					fileRangeLen, fileRangeStart, blockSize);

				readRes = (ioDepth == 1) ?
					rwBlockSized<preadWrapper>(fd, offsetGen) :
					aioBlockSized<io_prep_pread>(fd, offsetGen);
			}

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
