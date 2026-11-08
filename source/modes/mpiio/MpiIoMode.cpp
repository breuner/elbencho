// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>

#include "Logger.h"
#include "modes/mpiio/MpiIoLauncher.h"
#include "modes/mpiio/MpiIoMode.h"
#include "modes/mpiio/MpiIoProgArgs.h"
#include "ProgArgs.h"
#include "workers/LocalWorker.h"
#include "workers/WorkerException.h"


MpiIoMode::MpiIoMode(LocalWorker& worker) :
    worker(worker), progArgs(worker.progArgs), mpiIoArgs(worker.progArgs->getMpiIoArgs() ),
    workerRank(worker.workerRank)
{
}

#ifndef MPI_SUPPORT

// without MPI support, ProgArgs rejects "--mpiio", so none of this gets called

void MpiIoMode::init() {}
void MpiIoMode::uninit() {}
void MpiIoMode::initPhase() {}
void MpiIoMode::finishCollectivePhase() {}
bool MpiIoMode::continueRepeat(bool) { return true; }
void MpiIoMode::setCurrentFileIdx(size_t) {}

ssize_t MpiIoMode::readAt(size_t, void*, size_t, off_t)
{
    throw WorkerException("MPI-IO called, but this executable was built without MPI support.");
}

ssize_t MpiIoMode::writeAt(size_t, void*, size_t, off_t)
{
    throw WorkerException("MPI-IO called, but this executable was built without MPI support.");
}

void MpiIoMode::openDirModeFile(const std::string&, int)
{
    throw WorkerException("MPI-IO called, but this executable was built without MPI support.");
}

void MpiIoMode::prepDirModeFile(const std::string&, uint64_t) {}
void MpiIoMode::closeCurrentFile(const std::string&, bool) {}
void MpiIoMode::statCurrentFile(const std::string&) {}

#else // MPI_SUPPORT

namespace
{
    /* the communicators of the worker threads, process-wide: thread index t of every I/O rank
        gets the t-th dup of the I/O ranks' communicator. grow-only and never freed before
        MPI_Finalize, so that the number and order of the (collective) dups stays the same on all
        ranks, also across runs with different thread counts and after errors. */
    std::mutex threadCommsMutex;
    std::vector<MPI_Comm> threadComms;
}

/**
 * @return the communicator of the given thread index, created on first use (collective over
 *      the I/O ranks, so all ranks need to get here with the same thread count).
 */
MPI_Comm MpiIoMode::getThreadComm(size_t threadIdx)
{
    std::lock_guard<std::mutex> lock(threadCommsMutex);

    while(threadComms.size() <= threadIdx)
    {
        MPI_Comm newComm;

        int dupRes = MPI_Comm_dup(MpiIoLauncher::getIoComm(), &newComm);
        if(dupRes != MPI_SUCCESS)
            throw WorkerException("Duplicating the MPI communicator for a worker thread failed. "
                "Thread index: " + std::to_string(threadComms.size() ) + "; "
                "MPI error: " + errStr(dupRes) );

        threadComms.push_back(newComm);
    }

    return threadComms[threadIdx];
}

std::string MpiIoMode::errStr(int mpiErr)
{
    char buf[MPI_MAX_ERROR_STRING] = {};
    int len = 0;

    MPI_Error_string(mpiErr, buf, &len);

    return std::string(buf, len);
}

/**
 * Prepare this thread for MPI-IO: communicator, hints and (in file mode) the open shared files.
 * Opening is collective over the thread's group, so all I/O ranks open the same paths in the
 * same order.
 *
 * @throw WorkerException on error.
 */
void MpiIoMode::init()
{
    if(progArgs->getBenchMode() != BenchMode_MPIIO)
        return;

    if(!MpiIoLauncher::isActive() )
        throw WorkerException("MPI-IO mode requires \"--" ARG_MPIIO_LONG "\" on the command line "
            "of this process (also for a service instance), so that MPI gets initialized at "
            "startup.");

    /* only collective I/O needs the thread's group communicator (and the collective open and
        close that come with it); independent I/O opens every file for this thread alone, so that
        an MPI-IO layer which serializes MPI-IO calls per process (MPICH's ROMIO) cannot deadlock
        between the threads of a rank */
    comm = mpiIoArgs.getUseCollective() ?
        getThreadComm(workerRank - progArgs->getRankOffset() ) : MPI_COMM_SELF;

    createInfo();

    if(progArgs->getBenchPathType() == BenchPathType_DIR)
        return; // dir mode files get opened per file

    if(!progArgs->getRunCreateFilesPhase() && !progArgs->getRunReadPhase() )
        return; // no data phase, so nothing to open

    const int amode = progArgs->getRunCreateFilesPhase() ?
        (MPI_MODE_RDWR | MPI_MODE_CREATE) : MPI_MODE_RDONLY;

    files.resize(progArgs->getBenchPaths().size(), MPI_FILE_NULL);

    for(size_t i=0; i < files.size(); i++)
        openFile(progArgs->getBenchPaths()[i], amode, comm, &files[i] );
}

void MpiIoMode::uninit()
{
    for(size_t i=0; i < files.size(); i++)
        if(files[i] != MPI_FILE_NULL)
            closeFile(&files[i], progArgs->getBenchPaths()[i], false);

    files.clear();

    // (in file mode currentFile only aliases one of the files closed above)
    if( (progArgs->getBenchPathType() == BenchPathType_DIR) && (currentFile != MPI_FILE_NULL) )
        closeFile(&currentFile, "(dir mode file)", false);

    currentFile = MPI_FILE_NULL;

    if(info != MPI_INFO_NULL)
        MPI_Info_free(&info);

    comm = MPI_COMM_NULL;
    handles = nullptr;
}

/**
 * Build the MPI_Info from the user's "key=value,..." hints (format checked in ProgArgs).
 */
void MpiIoMode::createInfo()
{
    const std::string& hintsStr = mpiIoArgs.getHintsStr();

    MPI_Info_create(&info);

    for(size_t pos = 0; pos < hintsStr.size(); )
    {
        const size_t end = hintsStr.find(',', pos);
        const std::string pair = hintsStr.substr(pos,
            (end == std::string::npos) ? std::string::npos : (end - pos) );
        const size_t eq = pair.find('=');

        MPI_Info_set(info, pair.substr(0, eq).c_str(), pair.substr(eq + 1).c_str() );

        pos = (end == std::string::npos) ? hintsStr.size() : (end + 1);
    }
}

/**
 * Per-phase setup: handle lookup, independent vs. collective calls and the planned call count
 * of a collective phase. Called from LocalWorker::initPhaseFunctionPointers().
 */
void MpiIoMode::initPhase()
{
    collectivePhase = false;
    repeatVotePending = false;

    if(progArgs->getBenchMode() != BenchMode_MPIIO)
        return;

    const bool isDirMode = (progArgs->getBenchPathType() == BenchPathType_DIR);
    const bool isRandOrStrided =
        progArgs->getUseRandomOffsets() || progArgs->getUseStridedAccess();
    const bool isDataPhase = (worker.benchPhase == BenchPhase_CREATEFILES) ||
        (worker.benchPhase == BenchPhase_READFILES);

    /* dir mode and sequential file mode work on one current file (index 0), random/strided file
        mode indexes all files like LocalWorker's fdVecPtr */
    handles = (isDirMode || !isRandOrStrided) ? &currentFile : files.data();

    phaseIsWrite = (worker.benchPhase == BenchPhase_CREATEFILES);
    numOpsDone = 0;

    if(mpiIoArgs.getUseCollective() && !isDirMode && isDataPhase)
    {
        readFn = MPI_File_read_at_all;
        writeFn = MPI_File_write_at_all;
        readOpName = "MPI_File_read_at_all";
        writeOpName = "MPI_File_write_at_all";

        /* ProgArgs made the amounts a multiple of block size times dataset threads, so every
            thread of the group has this many blocks (sequential, strided and random alike) */
        const uint64_t blockSize = progArgs->getBlockSize();
        const uint64_t numDataSetThreads = progArgs->getNumDataSetThreads();

        plannedOps = progArgs->getUseRandomOffsets() ?
            (progArgs->getRandomAmount() / numDataSetThreads) / blockSize :
            (progArgs->getFileSize() / blockSize) / numDataSetThreads;

        collectivePhase = true;
        repeatVotePending = progArgs->getDoInfiniteIOLoop();
    }
    else
    {
        readFn = MPI_File_read_at;
        writeFn = MPI_File_write_at;
        readOpName = "MPI_File_read_at";
        writeOpName = "MPI_File_write_at";
    }
}

/**
 * Complete the planned call count of a collective phase with empty calls, so that the other
 * threads of the group never wait forever when this thread ended early (interruption, error).
 * Nothing to do when the phase completed normally. Called before the collective file close.
 */
void MpiIoMode::finishCollectivePhase()
{
    if(!collectivePhase)
        return;

    collectivePhase = false; // (so a second call becomes a no-op)

    if(files.empty() || (files[0] == MPI_FILE_NULL) )
        return;

    char dummy;
    MPI_Status status;

    for( ; numOpsDone < plannedOps; numOpsDone++)
    {
        if(phaseIsWrite)
            writeFn(files[0], 0, &dummy, 0, MPI_BYTE, &status);
        else
            readFn(files[0], 0, &dummy, 0, MPI_BYTE, &status);
    }

    continueRepeat(true); // (infinite loop: the group still waits for our vote on this repeat)
}

/**
 * Infinite I/O loop in a collective phase: the threads of the group decide together whether to
 * start another repeat, so that no thread waits in a collective call for a peer that stopped.
 * One small collective call per repeat; no-op outside such phases.
 *
 * @stop true if this thread wants to stop (interruption, error).
 * @return false if any thread of the group wants to stop.
 */
bool MpiIoMode::continueRepeat(bool stop)
{
    if(!repeatVotePending)
        return true;

    repeatVotePending = false;

    int myStop = stop;
    int anyStop = 0;

    MPI_Allreduce(&myStop, &anyStop, 1, MPI_INT, MPI_LOR, comm);

    return !anyStop;
}

/**
 * Positional read, independent or collective as set for the phase.
 *
 * @return number of bytes read, -1 on error with errno set to EIO (the MPI error is logged).
 */
ssize_t MpiIoMode::readAt(size_t fileHandleIdx, void* buf, size_t nbytes, off_t offset)
{
    OpsLogger& opsLog = worker.opsLog;
    MPI_Status status;

    OPLOG_PRE_OP(readOpName, std::to_string(fileHandleIdx), offset, nbytes);

    int readRes = readFn(handles[fileHandleIdx], offset, buf, (int)nbytes, MPI_BYTE, &status);

    numOpsDone++;

    OPLOG_POST_OP(readOpName, std::to_string(fileHandleIdx), offset, nbytes,
        readRes != MPI_SUCCESS);

    IF_UNLIKELY(readRes != MPI_SUCCESS)
    {
        ERRLOGGER(Log_NORMAL, readOpName << " failed. MPI error: " << errStr(readRes) <<
            std::endl);
        errno = EIO;
        return -1;
    }

    int count;
    MPI_Get_count(&status, MPI_BYTE, &count);

    return count;
}

/**
 * Positional write, independent or collective as set for the phase.
 *
 * @return number of bytes written, -1 on error with errno set to EIO (the MPI error is logged).
 */
ssize_t MpiIoMode::writeAt(size_t fileHandleIdx, void* buf, size_t nbytes, off_t offset)
{
    OpsLogger& opsLog = worker.opsLog;
    MPI_Status status;

    OPLOG_PRE_OP(writeOpName, std::to_string(fileHandleIdx), offset, nbytes);

    int writeRes = writeFn(handles[fileHandleIdx], offset, buf, (int)nbytes, MPI_BYTE, &status);

    numOpsDone++;

    OPLOG_POST_OP(writeOpName, std::to_string(fileHandleIdx), offset, nbytes,
        writeRes != MPI_SUCCESS);

    IF_UNLIKELY(writeRes != MPI_SUCCESS)
    {
        ERRLOGGER(Log_NORMAL, writeOpName << " failed. MPI error: " << errStr(writeRes) <<
            std::endl);
        errno = EIO;
        return -1;
    }

    int count;
    MPI_Get_count(&status, MPI_BYTE, &count);

    return count;
}

/**
 * Sequential file mode: switch the current file (called where LocalWorker sets fdVec[0]).
 */
void MpiIoMode::setCurrentFileIdx(size_t fileIdx)
{
    if(fileIdx < files.size() )
        currentFile = files[fileIdx];
}

/**
 * Dir mode: open the current per-thread file on MPI_COMM_SELF.
 *
 * @openFlags posix flags as in LocalWorker::getDirModeOpenFlags(); O_CREAT selects create mode,
 *      O_TRUNC truncates the file after open.
 * @throw WorkerException on error.
 */
void MpiIoMode::openDirModeFile(const std::string& path, int openFlags)
{
    const int amode = (openFlags & O_CREAT) ? (MPI_MODE_RDWR | MPI_MODE_CREATE) : MPI_MODE_RDONLY;

    openFile(path, amode, MPI_COMM_SELF, &currentFile);

    if(openFlags & O_TRUNC)
    {
        int truncRes = MPI_File_set_size(currentFile, 0);
        if(truncRes != MPI_SUCCESS)
        {
            closeFile(&currentFile, path, false);
            throw WorkerException("Truncating file failed. "
                "Path: " + path + "; "
                "MPI error: " + errStr(truncRes) );
        }
    }
}

/**
 * Dir mode: apply "--trunctosize" and "--preallocfile" to the current file in a write phase.
 *
 * @fileLen total length of the file, i.e. min offset plus size.
 * @throw WorkerException on error (file stays open, caller closes).
 */
void MpiIoMode::prepDirModeFile(const std::string& path, uint64_t fileLen)
{
    if(progArgs->getDoTruncToSize() )
    {
        int truncRes = MPI_File_set_size(currentFile, fileLen);
        if(truncRes != MPI_SUCCESS)
            throw WorkerException("Unable to set file size through MPI_File_set_size. "
                "Path: " + path + "; "
                "Size: " + std::to_string(fileLen) + "; "
                "MPI error: " + errStr(truncRes) );
    }

    if(progArgs->getDoPreallocFile() )
    {
        int preallocRes = MPI_File_preallocate(currentFile, fileLen);
        if(preallocRes != MPI_SUCCESS)
            throw WorkerException("Unable to preallocate file through MPI_File_preallocate. "
                "Path: " + path + "; "
                "Size: " + std::to_string(fileLen) + "; "
                "MPI error: " + errStr(preallocRes) );
    }
}

/**
 * Dir mode: close the current file.
 *
 * @throwOnError throw WorkerException on close error, otherwise just log it (for error paths).
 */
void MpiIoMode::closeCurrentFile(const std::string& path, bool throwOnError)
{
    closeFile(&currentFile, path, throwOnError);
}

/**
 * Dir mode "--statinline": there is no fd, so stat the path.
 *
 * @throw WorkerException on error.
 */
void MpiIoMode::statCurrentFile(const std::string& path)
{
    OpsLogger& opsLog = worker.opsLog;
    struct stat statBuf;

    OPLOG_PRE_OP("stat", path, 0, 0);

    int statRes = stat(path.c_str(), &statBuf);

    OPLOG_POST_OP("stat", path, 0, 0, statRes == -1);

    IF_UNLIKELY(statRes == -1)
        throw WorkerException(std::string("Inline file stat failed. ") +
            "Path: " + path + "; "
            "SysErr: " + strerror(errno) );
}

/**
 * @throw WorkerException on error.
 */
void MpiIoMode::openFile(const std::string& path, int amode, MPI_Comm fileComm,
    MPI_File* outFile)
{
    OpsLogger& opsLog = worker.opsLog;

    OPLOG_PRE_OP("MPI_File_open", path, 0, 0);

    int openRes = MPI_File_open(fileComm, path.c_str(), amode, info, outFile);

    OPLOG_POST_OP("MPI_File_open", path, 0, 0, openRes != MPI_SUCCESS);

    IF_UNLIKELY(openRes != MPI_SUCCESS)
    {
        *outFile = MPI_FILE_NULL;

        int errClass;
        MPI_Error_class(openRes, &errClass);

        if( (errClass == MPI_ERR_NO_SUCH_FILE) && (amode & MPI_MODE_CREATE) &&
            !progArgs->getRunCreateDirsPhase() &&
            (progArgs->getBenchPathType() == BenchPathType_DIR) )
            throw WorkerException("File create/open failed. "
                "Did you forget to enable directory creation ('--" ARG_CREATEDIRS_LONG "')? "
                "Path: " + path + "; "
                "MPI error: " + errStr(openRes) );

        throw WorkerException("File open failed. "
            "Path: " + path + "; "
            "MPI error: " + errStr(openRes) );
    }

    // errors are returned instead of aborting the process, so that they get reported normally
    MPI_File_set_errhandler(*outFile, MPI_ERRORS_RETURN);
}

/**
 * Close is collective over the communicator the file was opened with.
 *
 * @throwOnError throw WorkerException on close error, otherwise just log it (for error paths).
 */
void MpiIoMode::closeFile(MPI_File* file, const std::string& path, bool throwOnError)
{
    OpsLogger& opsLog = worker.opsLog;

    OPLOG_PRE_OP("MPI_File_close", path, 0, 0);

    int closeRes = MPI_File_close(file);

    OPLOG_POST_OP("MPI_File_close", path, 0, 0, closeRes != MPI_SUCCESS);

    *file = MPI_FILE_NULL;

    IF_UNLIKELY(closeRes != MPI_SUCCESS)
    {
        if(throwOnError)
            throw WorkerException("File close failed. "
                "Path: " + path + "; "
                "MPI error: " + errStr(closeRes) );

        ERRLOGGER(Log_NORMAL, "File close failed. "
            "Path: " << path << "; "
            "MPI error: " << errStr(closeRes) << std::endl);
    }
}

#endif // MPI_SUPPORT
