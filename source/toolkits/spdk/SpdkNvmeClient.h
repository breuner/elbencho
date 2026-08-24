// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef TOOLKITS_SPDKNVMECLIENT_H_
#define TOOLKITS_SPDKNVMECLIENT_H_

#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "SpdkBdevManager.h"

// forward declarations to keep this header independent of the spdk headers
struct spdk_thread;
struct spdk_bdev_io;


/**
 * High-level C++ wrapper for SPDK bdev_nvme (NVMe-oF) I/O, one instance per elbencho worker
 * thread.
 *
 * The actual NVMe-oF connections (bdevs, qpairs) are owned process-wide by SpdkBdevManager, which
 * is refcounted across all SpdkNvmeClient instances (see init()/disconnect() ); this decouples
 * the number of NVMe-oF connections from the number of worker threads, since bdev_nvme opens
 * exactly one I/O qpair per (dedicated SPDK I/O thread, path) pair rather than per worker.
 *
 * Note on thread-safety: Instances of this class are NOT thread-safe. A single instance may only
 * be used (init()/read()/write()/trim()/poll()/disconnect() ) from a single thread, though its
 * actual I/O runs on one of SpdkBdevManager's shared dedicated I/O threads under the hood.
 *
 * The static globalInit() and globalUninit() affect process-wide state and must be called exactly
 * once (e.g. from the main thread) before starting worker threads and after they have all been
 * joined, respectively.
 */
class SpdkNvmeClient
{
    public:
        struct IoContext
        {
            SpdkBdevManager::NamespaceHandle* nsHandle;
            uint64_t lba;
            uint32_t lbaCount;
            void* buffer; // pointer to the user-provided I/O buffer
            SpdkNvmeClient* owner{nullptr}; // set on submission; used by ioCompleteCb() to notify
            void* userData{nullptr}; // free for caller's own use, e.g. a pool slot index
            std::atomic<bool> done{false}; // set by ioCompleteCb() on the shared I/O thread
            bool ioSuccess{false};
        };

        static int globalInit(const std::string& configJsonStr);
        static void globalUninit();

        /* the process-wide spdk_thread created by globalInit(); bdev_nvme internally posts some
           continuations to spdk's global "app thread" (spdk_thread_get_app_thread() ) regardless
           of which thread actually drives a given bdev_nvme call, and this is that same thread
           (see globalInit() ), so SpdkBdevManager must reuse it as its own "app thread" rather
           than creating a second, separate one that spdk_thread_get_app_thread() doesn't know */
        static struct spdk_thread* getGlobalAppThread() { return bootstrapThread; }

        SpdkNvmeClient();
        ~SpdkNvmeClient();

        bool init(const std::string& configJsonStr);
        void disconnect();

        int32_t getNamespaceId(const std::string& name) const;
        std::string getNamespaceName(uint32_t nsId) const;
        std::vector<uint32_t> getNamespaceIds() const;
        uint64_t getNamespaceSize(uint32_t nsId) const;
        uint32_t getNamespaceSectorSize(uint32_t nsId) const;
        std::string getNamespaceUuid(uint32_t nsId) const;
        std::string getNamespaceNguid(uint32_t nsId) const;
        std::string getNamespaceModel(uint32_t nsId) const;

        int read(IoContext* ioCtx, uint32_t nsId, uint64_t lba, uint32_t lbaCount, void* buffer);
        int write(IoContext* ioCtx, uint32_t nsId, uint64_t lba, uint32_t lbaCount, void* buffer);
        int trim(IoContext* ioCtx, uint32_t nsId, uint64_t lba, uint32_t lbaCount);

        int poll();
        int waitForCompletions(std::vector<IoContext*>& outCompleted, int timeoutMs);

        static void* allocDmaBuf(size_t size);
        static void freeDmaBuf(void* buf);

    private:
        static bool globalInitCalled; // to make uninit a no-op if init wasn't called
        static std::recursive_mutex initMutex; // serializes globalInit()/globalUninit()

        /* spdk's bdev layer remembers the very first spdk_thread ever created in the process as
           its "app thread" and keeps sending it messages (e.g. module_examine_done() ) for as
           long as any bdev exists, so this thread - created in globalInit() - must stay alive
           until globalUninit() instead of being destroyed right after framework init */
        static struct spdk_thread* bootstrapThread;

        bool isAttached{false};
        size_t ioThreadIdx{0}; // this instance's assigned shared I/O thread, see init()
        struct spdk_thread* ioThread{NULL}; // == SpdkBdevManager::instance().getIoThread(ioThreadIdx)

        std::vector<IoContext*> pendingIo; /* in-flight I/Os submitted by this instance; only
            ever touched by this instance's own (single) thread, so no locking needed here even
            though ioCompleteCb() flips each IoContext's "done" flag from the shared I/O thread */

        std::mutex completionMutex; // guards completionCv only, not pendingIo (see above)
        std::condition_variable completionCv; // notified by ioCompleteCb(), see waitForCompletions()

        static void ioCompleteCb(struct spdk_bdev_io* bdevIo, bool success, void* cbArg);
        static void logFilterCb(int level, const char* file, const int line, const char* func,
            const char* format, va_list args);

        static std::string getNvmeErrorString(uint8_t sct, uint8_t sc);

        void submitOnIoThread(IoContext* ioCtx, std::function<void(IoContext*)> submitFn);
        int submitRead(IoContext* ioCtx, uint32_t nsId, uint64_t lba, uint32_t lbaCount,
            void* buffer);
        int submitWrite(IoContext* ioCtx, uint32_t nsId, uint64_t lba, uint32_t lbaCount,
            void* buffer);
        int submitTrim(IoContext* ioCtx, uint32_t nsId, uint64_t lba, uint32_t lbaCount);
        bool anyPendingIoDone() const; // true if at least one pendingIo entry is done
};

#endif /* TOOLKITS_SPDKNVMECLIENT_H_ */
