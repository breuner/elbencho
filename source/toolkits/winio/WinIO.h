// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef TOOLKITS_WINIO_WINIO_H_
#define TOOLKITS_WINIO_WINIO_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Common.h"
#include "toolkits/OpsLogger.h"

class LocalWorker;
class ProgArgs;


/**
 * The native Windows I/O path of a LocalWorker thread for files and block devices (Cygwin builds,
 * CYGWIN_SUPPORT). Cygwin's own I/O layer is slow and has no pread/pwrite for block devices, so
 * the benchmark data goes through Win32 (ReadFile/WriteFile, FILE_FLAG_NO_BUFFERING for "--direct",
 * an I/O completion port for "--iodepth"), while everything that is not benchmark data (metadata
 * phases, config and log files, ...) stays with cygwin.
 *
 * This is a friend of LocalWorker and works directly on its buffers, offset generator and
 * statistics, so that it adds no indirection to the I/O path. The handles are always opened for
 * overlapped I/O and are per worker thread: a handle opened for synchronous I/O serializes all
 * I/Os on it across threads, and a handle can only be bound to one completion port.
 *
 * On non-Cygwin builds this is an empty shell.
 */
class WinIO
{
    public:
        explicit WinIO(LocalWorker& worker);
        ~WinIO();

        void init();
        void uninit();
        void initPhaseHandleVecs();

        bool openDirModeFile(const std::string& path, int openFlags);
        void prepDirModeFile(const std::string& path, uint64_t fileLen, uint64_t preallocOffset);
        void statCurrentFile(const std::string& path);
        void closeCurrentFile(const std::string& path, bool throwOnError);

        ssize_t pread(size_t fileHandleIdx, void* buf, size_t nbytes, off_t offset);
        ssize_t pwrite(size_t fileHandleIdx, void* buf, size_t nbytes, off_t offset);
        ssize_t pwriteAndRead(size_t fileHandleIdx, void* buf, size_t nbytes, off_t offset);
        int64_t aioBlockSized();

    private:
        LocalWorker& worker;
        ProgArgs* progArgs; // shortcut for worker member
        size_t workerRank; // shortcut for worker member
        OpsLogger& opsLog; // shortcut for worker member (name is required by OPLOG_* macros)

        std::vector<void*> threadHandleVec; // one per bench path in file/bdev mode
        std::vector<void*> handleVec; // current file in dir mode and seq file/bdev mode
        const std::vector<void*>* handleVecPtr{nullptr}; // what the I/O path indexes

        std::string currentName; // path of handleVec[0] for the ops log

        void* syncEvent{nullptr}; // for the overlapped waits of the sync I/O path
        void* iocp{nullptr}; // I/O completion port for the async I/O path (iodepth > 1)

        struct AsyncCtx; // per-slot state of the async path, needs <windows.h>
        std::unique_ptr<AsyncCtx> asyncCtx;

        void* openHandle(const std::string& path, int openFlags);
        ssize_t transfer(bool isRead, size_t fileHandleIdx, void* buf, size_t nbytes,
            off_t offset);
        const std::string& entryName(size_t fileHandleIdx);

    public:
        // inliners

        /**
         * Make the given bench path the current file of the sequential file/bdev mode.
         */
        void setCurrentFile(size_t benchPathIdx)
        {
        #ifdef CYGWIN_SUPPORT
            handleVec[0] = threadHandleVec[benchPathIdx];
            currentName = progArgs->getBenchPaths()[benchPathIdx];
        #endif // CYGWIN_SUPPORT
        }
};

#endif /* TOOLKITS_WINIO_WINIO_H_ */
