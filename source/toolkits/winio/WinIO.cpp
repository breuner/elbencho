// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fcntl.h>

#include "Logger.h"
#include "ProgArgs.h"
#include "WinIO.h"
#include "WinIOTk.h"
#include "workers/LocalWorker.h"
#include "workers/WorkerException.h"

#ifdef CYGWIN_SUPPORT
    #ifndef _WIN32_WINNT
        #define _WIN32_WINNT 0x0A00 // (GetQueuedCompletionStatusEx & co need Vista or later)
    #endif
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
#endif // CYGWIN_SUPPORT

#define WINIO_AIO_MAX_WAIT_MS   5000 // completion wait timeout before interruption check
#define WINIO_AIO_MAX_EVENTS    4 // max number of completions to dequeue at once
#define WINIO_DRAIN_WAIT_MS     1000 // wait per cancelled I/O in uninit()


#ifdef CYGWIN_SUPPORT

/**
 * Per-slot state of the async I/O path, indexed like LocalWorker::ioBufVec.
 */
struct WinIO::AsyncCtx
{
    struct Slot
    {
        OVERLAPPED overlapped; // must stay first, the completion returns a pointer to it
        size_t fileHandleIdx;
        uint64_t offset;
        size_t len;
    };

    std::vector<Slot> slots;
    std::vector<std::chrono::steady_clock::time_point> ioStartTimeVec;
    size_t numPending{0}; // submitted and not yet completed, across engine calls
};

#else // CYGWIN_SUPPORT

struct WinIO::AsyncCtx {}; // (unused empty shell for non-Cygwin builds)

#endif // CYGWIN_SUPPORT


WinIO::WinIO(LocalWorker& worker) :
    worker(worker), progArgs(worker.progArgs), workerRank(worker.workerRank),
    opsLog(worker.opsLog)
{
}

WinIO::~WinIO()
{
}

#ifndef CYGWIN_SUPPORT

// empty shells for non-Cygwin builds
void WinIO::init() {}
void WinIO::uninit() {}
void WinIO::initPhaseHandleVecs() {}

#else // CYGWIN_SUPPORT

/**
 * Create the per-thread resources and open the per-thread handles of the bench paths in file/bdev
 * mode. (In dir mode, the files get opened dynamically through openDirModeFile().) Nothing to do
 * in the other benchmark modes.
 *
 * @throw WorkerException on error.
 */
void WinIO::init()
{
    if(progArgs->getBenchMode() != BenchMode_POSIX)
        return;

    syncEvent = CreateEventW(NULL, TRUE /*manualReset*/, FALSE, NULL);
    if(!syncEvent)
        throw WorkerException("Creating event for Windows I/O failed. "
            "SysErr: " + WinIOTk::lastErrorStr() );

    const size_t ioDepth = progArgs->getIODepth();

    if(ioDepth > 1)
    {
        iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1 /*numConcurrentThreads*/);
        if(!iocp)
            throw WorkerException("Creating I/O completion port failed. "
                "SysErr: " + WinIOTk::lastErrorStr() );

        asyncCtx = std::make_unique<AsyncCtx>();
        asyncCtx->slots.resize(ioDepth);
        asyncCtx->ioStartTimeVec.resize(ioDepth);
    }

    if( (progArgs->getBenchPathType() == BenchPathType_DIR) ||
        (!progArgs->getRunCreateFilesPhase() && !progArgs->getRunReadPhase() ) )
        return; // no data I/O without a read or write phase, so no handles needed

    // note: keep flags in sync with ProgArgs::prepareBenchPathFDsVec
    int openFlags = progArgs->getRunCreateFilesPhase() ? O_RDWR : O_RDONLY;

    if(progArgs->getUseDirectIO() )
        openFlags |= O_DIRECT;

    if(progArgs->getRunCreateFilesPhase() )
        openFlags |= O_CREAT;

    for(const std::string& path : progArgs->getBenchPaths() )
    { // (not in the ops log, like progArgs' shared fds on linux: this is setup, not benchmark)
        void* handle = openHandle(path, openFlags);

        if(!handle)
            throw WorkerException("Unable to open benchmark path: " + path + "; "
                "SysErr: " + WinIOTk::lastErrorStr() );

        threadHandleVec.push_back(handle);
    }
}

/**
 * Cancel what might still be in flight (after an error or interruption), close handles and
 * release the per-thread resources.
 */
void WinIO::uninit()
{
    for(void* handle : threadHandleVec)
        CancelIoEx(handle, NULL);

    for(void* handle : handleVec)
        if(handle)
            CancelIoEx(handle, NULL);

    /* drain the completion port, because the kernel still writes to the OVERLAPPED of a cancelled
        I/O when it completes, so asyncCtx must not go away before that */
    while(asyncCtx && asyncCtx->numPending)
    {
        OVERLAPPED_ENTRY entries[WINIO_AIO_MAX_EVENTS];
        ULONG numEntries = 0;

        if(!GetQueuedCompletionStatusEx(iocp, entries, WINIO_AIO_MAX_EVENTS, &numEntries,
            WINIO_DRAIN_WAIT_MS, FALSE) )
            break; // timeout; nothing more we can do

        asyncCtx->numPending -= std::min<size_t>(numEntries, asyncCtx->numPending);
    }

    for(void* handle : threadHandleVec)
        CloseHandle(handle);

    for(void* handle : handleVec)
        if(handle)
            CloseHandle(handle);

    threadHandleVec.clear();
    handleVec.clear();
    handleVecPtr = nullptr;

    asyncCtx.reset();

    if(iocp)
        CloseHandle(iocp);

    if(syncEvent)
        CloseHandle(syncEvent);

    iocp = nullptr;
    syncEvent = nullptr;
}

/**
 * Init the handle vectors for the current phase, mirroring LocalWorker::initPhaseFileHandleVecs().
 */
void WinIO::initPhaseHandleVecs()
{
    handleVec.assign(1, nullptr);

    if( (progArgs->getBenchPathType() == BenchPathType_DIR) ||
        (!progArgs->getUseRandomOffsets() && !progArgs->getUseStridedAccess() ) )
        handleVecPtr = &handleVec; // single current file, set by openDirModeFile/setCurrentFile
    else
        handleVecPtr = &threadHandleVec; // random file/bdev mode selects from the full set
}

/**
 * Open a handle and bind it to the completion port if async I/O is used.
 *
 * @return nullptr on error, in which case errno is set.
 * @throw WorkerException if binding to the completion port fails.
 */
void* WinIO::openHandle(const std::string& path, int openFlags)
{
    void* handle = WinIOTk::openFile(path, openFlags);
    if(!handle)
        return nullptr;

    if(iocp && (CreateIoCompletionPort(handle, iocp, 0, 0) != iocp) )
    {
        const std::string sysErr = WinIOTk::lastErrorStr();
        CloseHandle(handle);
        throw WorkerException("Binding file to I/O completion port failed. "
            "Path: " + path + "; "
            "SysErr: " + sysErr);
    }

    return handle;
}

/**
 * Open the current file in dir mode. Counterpart of the openat() in
 * LocalWorker::dirModeOpenAndPrepFile(), which also keeps the error messages.
 *
 * @return false on error, in which case errno is set.
 */
bool WinIO::openDirModeFile(const std::string& path, int openFlags)
{
    OPLOG_PRE_OP("openat", path, 0, 0);

    handleVec[0] = openHandle(path, openFlags);
    currentName = path;

    OPLOG_POST_OP("openat", path, 0, 0, !handleVec[0]);

    return (handleVec[0] != nullptr);
}

/**
 * Apply "--trunctosize" and "--preallocfile" to the current dir mode file (in a write phase).
 *
 * @throw WorkerException on error.
 */
void WinIO::prepDirModeFile(const std::string& path, uint64_t fileLen, uint64_t preallocOffset)
{
    if(progArgs->getDoTruncToSize() )
    { /* (sparse like ftruncate() on linux, so there is no zero-fill up to a later write offset;
          but a sparse file can't be preallocated, so not when that is requested) */
        if(!progArgs->getDoPreallocFile() && !WinIOTk::setSparse(handleVec[0]) )
            throw WorkerException("Unable to mark file sparse. "
                "Path: " + path + "; "
                "SysErr: " + WinIOTk::lastErrorStr() );

        if(!WinIOTk::setFileSize(handleVec[0], fileLen) )
            throw WorkerException("Unable to set file size. "
                "Path: " + path + "; "
                "Size: " + std::to_string(fileLen) + "; "
                "SysErr: " + WinIOTk::lastErrorStr() );
    }

    if(progArgs->getDoPreallocFile() &&
        !WinIOTk::preallocate(handleVec[0], preallocOffset, fileLen - preallocOffset) )
        throw WorkerException("Unable to preallocate file size. "
            "File: " + path + "; "
            "Offset: " + std::to_string(preallocOffset) + "; "
            "Size: " + std::to_string(fileLen - preallocOffset) + "; "
            "SysErr: " + WinIOTk::lastErrorStr() );
}

/**
 * Inline stat of the current dir mode file ("--statinline").
 *
 * @throw WorkerException on error.
 */
void WinIO::statCurrentFile(const std::string& path)
{
    OPLOG_PRE_OP("fstat", path, 0, 0);

    const bool statRes = WinIOTk::statFile(handleVec[0]);

    OPLOG_POST_OP("fstat", path, 0, 0, !statRes);

    IF_UNLIKELY(!statRes)
        throw WorkerException("Inline file stat failed. "
            "Path: " + path + "; "
            "SysErr: " + WinIOTk::lastErrorStr() );
}

/**
 * Close the current dir mode file.
 *
 * @throwOnError throw WorkerException on close error, otherwise just log it.
 */
void WinIO::closeCurrentFile(const std::string& path, bool throwOnError)
{
    void* handle = handleVec[0];
    handleVec[0] = nullptr;

    OPLOG_PRE_OP("close", path, 0, 0);

    const bool closeRes = WinIOTk::closeFile(handle);

    OPLOG_POST_OP("close", path, 0, 0, !closeRes);

    IF_UNLIKELY(!closeRes)
    {
        if(throwOnError)
            throw WorkerException("File close failed. "
                "Path: " + path + "; "
                "SysErr: " + WinIOTk::lastErrorStr() );

        ERRLOGGER(Log_NORMAL, "File close failed. "
            "Path: " << path << "; "
            "SysErr: " << WinIOTk::lastErrorStr() << std::endl);
    }
}

/**
 * @return the path behind the given file handle index, for the ops log. (Linux logs the fd there,
 *      which is shared between the threads in file/bdev mode, but the handles here are not.)
 */
const std::string& WinIO::entryName(size_t fileHandleIdx)
{
    return (handleVecPtr == &threadHandleVec) ?
        progArgs->getBenchPaths()[fileHandleIdx] : currentName;
}

/**
 * Sync positional read or write through the overlapped handle, with pread/pwrite semantics.
 *
 * @return number of transferred bytes, 0 at end of file, -1 on error with errno set.
 */
ssize_t WinIO::transfer(bool isRead, size_t fileHandleIdx, void* buf, size_t nbytes,
    off_t offset)
{
    HANDLE handle = (*handleVecPtr)[fileHandleIdx];

    OVERLAPPED overlapped = {};
    overlapped.Offset = (DWORD)offset;
    overlapped.OffsetHigh = (DWORD)( (uint64_t)offset >> 32);
    overlapped.hEvent = (HANDLE)( (uintptr_t)syncEvent | 1); /* (the set low bit keeps the
        completion away from the completion port, if the handle is bound to one) */

    const BOOL submitRes = isRead ?
        ReadFile(handle, buf, (DWORD)nbytes, NULL, &overlapped) :
        WriteFile(handle, buf, (DWORD)nbytes, NULL, &overlapped);

    IF_UNLIKELY(!submitRes && (GetLastError() != ERROR_IO_PENDING) )
    {
        errno = WinIOTk::errnoFromLastError();
        return -1;
    }

    DWORD numBytesDone;

    IF_UNLIKELY(!GetOverlappedResult(handle, &overlapped, &numBytesDone, TRUE /*wait*/) )
    {
        if(GetLastError() == ERROR_HANDLE_EOF)
            return 0;

        errno = WinIOTk::errnoFromLastError();
        return -1;
    }

    return numBytesDone;
}

ssize_t WinIO::pread(size_t fileHandleIdx, void* buf, size_t nbytes, off_t offset)
{
    OPLOG_PRE_OP("pread", entryName(fileHandleIdx), offset, nbytes);

    const ssize_t readRes = transfer(true, fileHandleIdx, buf, nbytes, offset);

    OPLOG_POST_OP("pread", entryName(fileHandleIdx), offset, nbytes, readRes == -1);

    return readRes;
}

ssize_t WinIO::pwrite(size_t fileHandleIdx, void* buf, size_t nbytes, off_t offset)
{
    OPLOG_PRE_OP("pwrite", entryName(fileHandleIdx), offset, nbytes);

    const ssize_t writeRes = transfer(false, fileHandleIdx, buf, nbytes, offset);

    OPLOG_POST_OP("pwrite", entryName(fileHandleIdx), offset, nbytes, writeRes <= 0);

    return writeRes;
}

/**
 * Write followed by an immediate read of the same block (for "--verifydirect"/"--readinline").
 */
ssize_t WinIO::pwriteAndRead(size_t fileHandleIdx, void* buf, size_t nbytes, off_t offset)
{
    const ssize_t writeRes = pwrite(fileHandleIdx, buf, nbytes, offset);

    IF_UNLIKELY(writeRes <= 0)
        return writeRes;

    return pread(fileHandleIdx, buf, writeRes, offset);
}

/**
 * Async read/write of the whole range of the offset generator with up to iodepth I/Os in flight
 * through the completion port. Same structure as LocalWorker::spdkAioBlockSized().
 *
 * @return similar to pread/pwrite: full byte count on success, -1 with errno set on I/O error,
 *      fewer bytes on a short I/O.
 * @throw WorkerException on submission or completion port errors.
 */
int64_t WinIO::aioBlockSized()
{
    const BenchPhase globalBenchPhase = worker.workersSharedData->currentBenchPhase;
    const size_t fileHandlesVecSize = handleVecPtr->size();
    const uint64_t fileSize = progArgs->getFileSize();
    const uint64_t fileOffsetBase = progArgs->getFileOffset();
    const bool isSingleFile = (fileHandlesVecSize == 1);
    const unsigned rwMixReadPercent = progArgs->getRWMixReadPercent();
    const bool journalEnabled = progArgs->isJournalingEnabled();

    size_t& numPending = asyncCtx->numPending; // num requests submitted and pending for completion
    size_t numBytesDone = 0; // after successfully completed requests

    worker.resetAsyncIOSlots(); // (members, so that the hot path doesn't allocate)

    std::vector<AsyncCtx::Slot>& slots = asyncCtx->slots;
    std::vector<std::chrono::steady_clock::time_point>& ioStartTimeVec = asyncCtx->ioStartTimeVec;
    std::vector<uint8_t>& isReadVec = worker.asyncIsReadVec;
    std::vector<uint8_t>& isRWMixReadVec = worker.asyncIsRWMixReadVec;
    std::vector<size_t>& freeSlots = worker.asyncFreeSlots; // slots without I/O in flight

    LocalWorker::JournalSlotsReleaser journalSlotsReleaser(worker); // for the throwing paths

    // decide read vs write for the next request, mirroring rwBlockSized()/aioRWMixPrepper()
    auto isNextOpARead = [&]() -> bool
    {
        if(worker.benchPhase == BenchPhase_READFILES) // also covers rwmixthreads readers
            return true;

        return rwMixReadPercent &&
            ( ( (workerRank + worker.numIOPSSubmitted) % 100) < rwMixReadPercent);
    };

    /* Submit request for the given slot, using rwOffsetGen for the next offset & block size;
       throws on submission error. Return values as in spdkAioBlockSized(). */
    auto trySubmitSlot = [&](const size_t slotIdx) __attribute__( (always_inline) )
        -> LocalWorker::SubmitRes
    {
        uint64_t rwOffsetGenNext;
        size_t blockSize;

        if(!journalEnabled)
        {
            rwOffsetGenNext = worker.rwOffsetGen->getNextOffset();
            blockSize = worker.rwOffsetGen->getNextBlockSizeToSubmit();
        }
        else
        { // reuse a previously deferred draw, if any, so no offset gets lost or drawn twice
            LocalWorker::JournalPendingDraw& pendingDraw = worker.journalPendingDraw;

            if(!pendingDraw.isValid)
            {
                pendingDraw.rwOffsetGenNext = worker.rwOffsetGen->getNextOffset();
                pendingDraw.blockSize = worker.rwOffsetGen->getNextBlockSizeToSubmit();
                pendingDraw.isValid = true;
            }

            rwOffsetGenNext = pendingDraw.rwOffsetGenNext;
            blockSize = pendingDraw.blockSize;
        }

        uint64_t currentOffset;
        size_t fileHandlesIdx;

        worker.calcFileIdxAndOffsetStriped(rwOffsetGenNext, fileSize, fileOffsetBase,
            isSingleFile, fileHandlesIdx, currentOffset);

        bool isRead = isNextOpARead();

        if(journalEnabled)
        { /* let the journal decide & lock; it can turn a rwmix read into an initializing write or
             skip the I/O altogether. the lock stays held until this slot's I/O completes. */
            const JournalBlockIO::Action journalAction = worker.journalBeginIO(slotIdx,
                fileHandlesIdx, currentOffset, blockSize,
                worker.journalGetIntent(rwMixReadPercent) );

            IF_UNLIKELY(journalAction == JournalBlockIO::ACTION_DEFER)
                return LocalWorker::SubmitRes_DEFERRED;

            worker.journalPendingDraw.isValid = false; // this draw gets used up now

            IF_UNLIKELY(journalAction == JournalBlockIO::ACTION_SKIP)
            { // nothing was ever written here, so accept as verified-ok without reading it
                worker.numIOPSSubmitted++;
                worker.rwOffsetGen->addBytesSubmitted(blockSize);

                return LocalWorker::SubmitRes_SKIPPED;
            }

            isRead = (journalAction == JournalBlockIO::ACTION_READ);
        }

        isReadVec[slotIdx] = isRead;
        isRWMixReadVec[slotIdx] = isRead && (globalBenchPhase == BenchPhase_CREATEFILES);

        HANDLE handle = (*handleVecPtr)[fileHandlesIdx];
        AsyncCtx::Slot& slot = slots[slotIdx];

        slot.overlapped = {};
        slot.overlapped.Offset = (DWORD)currentOffset;
        slot.overlapped.OffsetHigh = (DWORD)(currentOffset >> 32);
        slot.fileHandleIdx = fileHandlesIdx;
        slot.offset = currentOffset;
        slot.len = blockSize;

        ioStartTimeVec[slotIdx] = std::chrono::steady_clock::now();

        bool hadToWait = worker.rwRateLimiter(blockSize);
        IF_UNLIKELY(hadToWait) // invalidate start time of all pending due to rate limiter wait
            for(std::chrono::steady_clock::time_point& startT : ioStartTimeVec)
                startT = std::chrono::steady_clock::time_point::min();

        BOOL submitRes;

        if(isRead)
        {
            OPLOG_PRE_OP("aioread", entryName(fileHandlesIdx), currentOffset, blockSize);
            submitRes = ReadFile(handle, worker.ioBufVec[slotIdx], (DWORD)blockSize, NULL,
                &slot.overlapped);
        }
        else
        {
            worker.preWriteBlockModifier(worker.ioBufVec[slotIdx], worker.gpuIOBufVec[slotIdx],
                blockSize, currentOffset);
            worker.preWriteCudaMemcpy(worker.ioBufVec[slotIdx], worker.gpuIOBufVec[slotIdx],
                blockSize);

            OPLOG_PRE_OP("aiowrite", entryName(fileHandlesIdx), currentOffset, blockSize);
            submitRes = WriteFile(handle, worker.ioBufVec[slotIdx], (DWORD)blockSize, NULL,
                &slot.overlapped);
        }

        IF_UNLIKELY(!submitRes && (GetLastError() != ERROR_IO_PENDING) )
        {
            const bool isInvalidParam = (GetLastError() == ERROR_INVALID_PARAMETER);
            const std::string sysErr = WinIOTk::lastErrorStr();

            OPLOG_POST_OP(isRead ? "aioread" : "aiowrite", entryName(fileHandlesIdx),
                currentOffset, blockSize, true);

            throw WorkerException(std::string("Async IO submission failed. ") +
                ( (progArgs->getUseDirectIO() && isInvalidParam) ?
                    "Can be caused by directIO misalignment. " : "") +
                "NumPending: " + std::to_string(numPending) + "; "
                "Offset: " + std::to_string(currentOffset) + "; "
                "IO size: " + std::to_string(blockSize) + "; "
                "SysErr: " + sysErr);
        }

        worker.numIOPSSubmitted++;
        worker.rwOffsetGen->addBytesSubmitted(blockSize);

        return LocalWorker::SubmitRes_SUBMITTED;
    };

    /* Keep all free slots busy while there are bytes left to submit. A slot can stay free when
        its next journaled I/O has to wait for one of this thread's own in-flight I/Os; the next
        completion re-runs this and gets it going then. */
    auto fillIdleSlots = [&]() __attribute__( (always_inline) )
    {
        while(!freeSlots.empty() && worker.rwOffsetGen->getNumBytesLeftToSubmit() )
        {
            const LocalWorker::SubmitRes submitRes = trySubmitSlot(freeSlots.back() );

            if(submitRes == LocalWorker::SubmitRes_SKIPPED)
                continue; // journal skip, so this slot is still free for the next draw

            IF_UNLIKELY(submitRes == LocalWorker::SubmitRes_DEFERRED)
                break; // retry after the next completion

            freeSlots.pop_back();
            numPending++;
        }
    };

    // P H A S E 1: initial seed of io submissions up to full ioDepth

    fillIdleSlots();

    // P H A S E 2: wait for submissions to complete and submit new requests if bytes left

    while(numPending)
    {
        OVERLAPPED_ENTRY entries[WINIO_AIO_MAX_EVENTS];
        ULONG numEntries = 0;

        const BOOL waitRes = GetQueuedCompletionStatusEx(iocp, entries, WINIO_AIO_MAX_EVENTS,
            &numEntries, WINIO_AIO_MAX_WAIT_MS, FALSE);

        IF_UNLIKELY(!waitRes)
        {
            if(GetLastError() == WAIT_TIMEOUT)
            { // that's ok, as we set a short timeout to check interruptions
                worker.checkInterruptionRequest();
                continue;
            }

            throw WorkerException("Waiting for async IO completions failed. "
                "NumPending: " + std::to_string(numPending) + "; "
                "SysErr: " + WinIOTk::lastErrorStr() );
        }

        for(ULONG entryIdx = 0; entryIdx < numEntries; entryIdx++)
        {
            AsyncCtx::Slot* slot =
                reinterpret_cast<AsyncCtx::Slot*>(entries[entryIdx].lpOverlapped);
            const size_t slotIdx = slot - slots.data();
            const bool isRead = isReadVec[slotIdx];
            const bool isRWMixRead = isRWMixReadVec[slotIdx];
            HANDLE handle = (*handleVecPtr)[slot->fileHandleIdx];

            numPending--;

            DWORD numBytesTransferred = 0;
            const BOOL ioRes = GetOverlappedResult(handle, &slot->overlapped, &numBytesTransferred,
                FALSE /*wait*/);

            IF_UNLIKELY(!ioRes || (numBytesTransferred != slot->len) )
            { /* unexpected result. (the journal guard of this slot - and of all others still in
                 flight - gets released by journalSlotsReleaser; journal cells of a failed write
                 are deliberately left at 0, i.e. "uninitialized", because a failed or partial
                 write cannot be assumed to have left the previous content intact.) */
                const int ioErrno = ioRes ? 0 : WinIOTk::errnoFromLastError();

                OPLOG_POST_OP(isRead ? "aioread" : "aiowrite", entryName(slot->fileHandleIdx),
                    slot->offset, slot->len, true);

                if(!ioRes)
                {
                    errno = ioErrno;
                    return -1;
                }

                // partial read/write, so return what we got so far
                return (numBytesDone + numBytesTransferred);
            }

            OPLOG_POST_OP(isRead ? "aioread" : "aiowrite", entryName(slot->fileHandleIdx),
                slot->offset, slot->len, false);

            if(journalEnabled) // make this slot's journal context the active one for the checker
                worker.activeJournalIO = &worker.journalIOSlots[slotIdx];

            if(isRead)
            {
                worker.postReadCudaMemcpy(worker.ioBufVec[slotIdx], worker.gpuIOBufVec[slotIdx],
                    slot->len);
                worker.postReadBlockChecker(worker.ioBufVec[slotIdx], worker.gpuIOBufVec[slotIdx],
                    slot->len, slot->offset);
            }

            freeSlots.push_back(slotIdx);

            if(journalEnabled)
                worker.journalCommitIO(slotIdx); /* commits new generations for a write and
                    releases the journal lock that this slot held since its submission */

            // calc io operation latency
            std::chrono::steady_clock::time_point ioEndT = std::chrono::steady_clock::now();
            std::chrono::microseconds ioElapsedMicroSec =
                std::chrono::duration_cast<std::chrono::microseconds>
                (ioEndT - ioStartTimeVec[slotIdx] );

            // don't count latency if this I/O had to wait for rate limiter
            const bool countLatency =
                (ioStartTimeVec[slotIdx] != std::chrono::steady_clock::time_point::min() );

            if(isRWMixRead)
            {
                IF_LIKELY(countLatency)
                    worker.iopsLatHistoReadMix.addLatency(ioElapsedMicroSec.count() );

                worker.atomicLiveOpsReadMix.numBytesDone += slot->len;
                worker.atomicLiveOpsReadMix.numIOPSDone++;
            }
            else
            {
                IF_LIKELY(countLatency)
                    worker.iopsLatHisto.addLatency(ioElapsedMicroSec.count() );

                worker.atomicLiveOps.numBytesDone += slot->len;
                worker.atomicLiveOps.numIOPSDone++;
            }

            numBytesDone += slot->len;

            worker.checkInterruptionRequest();
        }

        // completed slots are free now, so get them busy again
        fillIdleSlots();
    }

    return worker.rwOffsetGen->getNumBytesTotal();
}

#endif // CYGWIN_SUPPORT
