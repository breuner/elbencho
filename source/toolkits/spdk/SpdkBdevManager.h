// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef TOOLKITS_SPDKBDEVMANAGER_H_
#define TOOLKITS_SPDKBDEVMANAGER_H_

#include <atomic>
#include <cstdint>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// forward declarations to keep this header independent of the spdk headers
struct spdk_thread;
struct spdk_bdev_desc;
struct spdk_io_channel;
struct spdk_nvme_transport_id;


/**
 * Process-wide, refcounted manager for a shared pool of SPDK bdev_nvme connections.
 *
 * This parses the JSON config exactly once (on the first attach() ), creates
 * the bdev_nvme controllers/bdevs for the configured (or discovered) subsystems and paths, and
 * runs a small, fixed-size pool of dedicated SPDK-polled threads ("I/O threads", count is
 * configurable via the "io_threads" JSON field) through which all elbencho worker threads submit
 * their I/O. Because bdev_nvme opens exactly one I/O qpair per (spdk_thread, path) pair, the
 * number of actual NVMe-oF connections becomes io_threads * usable_paths, independent of the
 * number of elbencho worker threads attached.
 *
 * attach()/detach() are refcounted: the first attach() does the full setup, further attach()
 * calls just increment the refcount (and must use an identical config, which is the normal case
 * since all elbencho worker threads share the same config), and the detach() that brings the
 * refcount back to zero tears everything down again, so that no NVMe-oF connections are left
 * open once the last worker thread is done.
 */
class SpdkBdevManager
{
    public:
        struct NamespaceHandle
        {
            uint32_t id;
            std::string fullName; // format: "Subsys:BdevName"
            uint64_t sizeBytes;
            uint32_t sectorSize;
            std::string uuid; // target-provided UUID; empty if the target has none
            std::string nguid; // target-provided NGUID; empty if the target has none
            std::string model;
            struct spdk_bdev_desc* desc;
            std::vector<struct spdk_io_channel*> channels; // one per I/O thread, same order
        };

        static SpdkBdevManager& instance();

        bool attach(const std::string& configJsonStr);
        void detach();

        int32_t getNamespaceId(const std::string& name);
        std::string getNamespaceName(uint32_t nsId);
        std::vector<uint32_t> getNamespaceIds();
        NamespaceHandle* getNamespace(uint32_t nsId); // valid only while attached

        size_t getNumIoThreads();
        struct spdk_thread* getIoThread(size_t ioThreadIdx);
        size_t assignIoThreadIdx(); // round-robin across the I/O thread pool

    private:
        SpdkBdevManager() {}

        struct SubsystemGroup; // defined in the .cpp file

        struct Filters
        {
            std::vector<std::string> allowedModels;
        };

        /**
         * One entry from a target's discovery log page, describing one path to one subsystem.
         */
        struct DiscoveredPath
        {
            uint8_t trtype;
            std::string subNqn;
            std::string traddr;
            std::string trsvcid;
        };

        std::mutex mutex; // serializes attach()/detach() and guards all members below

        int refCount{0};

        std::map<uint32_t, NamespaceHandle> namespaces;
        std::map<std::string, uint32_t> nameToId;
        uint32_t nextNsId{0};

        std::vector<std::string> createdBaseNames; // bdev_nvme groups to delete on teardown

        struct spdk_thread* appThread{nullptr};
        std::thread appThreadRunner;
        std::atomic<bool> appThreadStop{false};

        std::vector<struct spdk_thread*> ioThreads;
        std::vector<std::thread> ioThreadRunners;
        std::atomic<bool> ioThreadsStop{false};
        std::atomic<size_t> nextIoThreadAssign{0};

        std::vector<int> cpuCoreList; // from "cpu_cores"/"cpu_core_range", else all available cores
        std::atomic<size_t> nextCpuCoreAssign{0}; // round-robin cursor into cpuCoreList

        bool doAttach(const std::string& configJsonStr); // actual setup, called on first attach()
        void doDetach(); // actual teardown, called when refcount reaches zero

        void runAppThread(std::promise<void> readyPromise);
        void runIoThread(size_t ioThreadIdx, std::promise<void> readyPromise);

        int assignCpuCore(); // round-robin across cpuCoreList; only call if !cpuCoreList.empty()
        void bindCallingThreadToAssignedCore(); // no-op if cpuCoreList is empty

        std::string readSystemHostNqn();
        std::vector<DiscoveredPath> queryDiscoveryLog(const struct spdk_nvme_transport_id& baseTrid,
            const std::string& hostNqn);

        bool isNamespaceAllowed(const Filters& filters, const std::string& model);
};

#endif /* TOOLKITS_SPDKBDEVMANAGER_H_ */
