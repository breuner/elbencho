// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifdef SPDK_SUPPORT

#include <algorithm>
#include <arpa/inet.h>
#include <boost/json.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <sched.h>
#include <vector>
#include <spdk/bdev.h>
#include <spdk/env.h>
#include <spdk/fd_group.h>
#include <spdk/module/bdev/nvme.h>
#include <spdk/nvme.h>
#include <spdk/string.h>
#include <spdk/thread.h>
#include <spdk/uuid.h>
#include <sstream>

#include "Logger.h"
#include "SpdkBdevManager.h"
#include "SpdkNvmeClient.h"
#include "toolkits/NumaTk.h"
#include "toolkits/StringTk.h"


/**
 * One resolved logical subsystem: a name, its NQN and the list of (traddr, trsvcid) paths to
 * create bdevs for. One config entry can resolve to more than one of these in discovery mode, if
 * the target's discovery service reports more than one distinct subsystem.
 */
struct SpdkBdevManager::SubsystemGroup
{
    std::string groupName;
    std::string subNqn;
    std::vector<std::pair<std::string, std::string>> addrs; // (traddr, trsvcid) pairs
};

/**
 * Anonymous namespace for internal helper functions.
 */
namespace
{

/**
 * Trim whitespace from right side of the given string.
 */
std::string trimRight(const std::string& str)
{
    size_t end = str.find_last_not_of(" \t\n\r");

    return (end == std::string::npos) ? "" : str.substr(0, end + 1);
}

/**
 * Derive a default human-friendly subsystem name from a subsystem NQN, used when the config
 * doesn't give an explicit "name". Formal NQNs (e.g. "nqn.2014-08.org.nvmexpress:uuid:<uuid>")
 * are truncated to their last ':'-separated segment to keep the result short; short custom NQNs
 * without a ':' (e.g. "dnode-8-ssds-0") are used as-is.
 */
std::string defaultSubsystemNameFromNqn(const std::string& subNqn)
{
    size_t lastColon = subNqn.find_last_of(':');

    return (lastColon == std::string::npos) ? subNqn : subNqn.substr(lastColon + 1);
}

/**
 * Expand a "start-end" IPv4 address range (e.g. "10.0.0.1-10.0.0.20") into the list of individual
 * addresses it covers, inclusive of both ends.
 *
 * @return the expanded address list, or an empty list if the range string is invalid.
 */
std::vector<std::string> expandTraddrRange(const std::string& rangeStr)
{
    std::vector<std::string> result;

    size_t dashPos = rangeStr.find('-');

    if(dashPos == std::string::npos)
    {
        ERRLOGGER(Log_NORMAL, "[SPDK] Invalid traddr_range (expected \"start-end\"): " <<
            rangeStr << std::endl);
        return result;
    }

    std::string startStr = rangeStr.substr(0, dashPos);
    std::string endStr = rangeStr.substr(dashPos + 1);

    struct in_addr startAddr, endAddr;

    if( (inet_pton(AF_INET, startStr.c_str(), &startAddr) != 1) ||
        (inet_pton(AF_INET, endStr.c_str(), &endAddr) != 1) )
    {
        ERRLOGGER(Log_NORMAL, "[SPDK] Invalid IPv4 address in traddr_range: " << rangeStr <<
            std::endl);
        return result;
    }

    uint32_t start = ntohl(startAddr.s_addr);
    uint32_t end = ntohl(endAddr.s_addr);

    if(end < start)
    {
        ERRLOGGER(Log_NORMAL, "[SPDK] traddr_range end is before start: " << rangeStr <<
            std::endl);
        return result;
    }

    constexpr uint32_t maxRangeSize = 4096; // sanity cap to avoid accidental huge expansions

    if( (end - start + 1) > maxRangeSize)
    {
        ERRLOGGER(Log_NORMAL, "[SPDK] traddr_range covers too many addresses (max " <<
            maxRangeSize << "): " << rangeStr << std::endl);
        return result;
    }

    for(uint32_t addr = start; ; addr++)
    {
        struct in_addr inAddr;
        inAddr.s_addr = htonl(addr);

        char addrBuf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &inAddr, addrBuf, sizeof(addrBuf) );

        result.push_back(addrBuf);

        if(addr == end) // check here (instead of in the loop condition) to avoid uint32 overflow
            break;
    }

    return result;
}

/**
 * Parse the "traddr" (single string or array of strings) and "traddr_range" fields of a
 * subsystem config entry into one de-duplicated address list.
 */
std::vector<std::string> parseTraddrList(const boost::json::object& sub)
{
    std::vector<std::string> result;

    if(sub.contains("traddr") )
    {
        const auto& traddrVal = sub.at("traddr");

        if(traddrVal.is_array() )
        {
            for(const auto& val : traddrVal.as_array() )
                result.push_back(std::string(val.as_string().c_str() ) );
        }
        else
            result.push_back(std::string(traddrVal.as_string().c_str() ) );
    }

    if(sub.contains("traddr_range") )
    {
        std::string rangeStr = std::string(sub.at("traddr_range").as_string().c_str() );
        std::vector<std::string> rangeAddrs = expandTraddrRange(rangeStr);

        result.insert(result.end(), rangeAddrs.begin(), rangeAddrs.end() );
    }

    // de-duplicate while preserving order
    std::vector<std::string> dedupedResult;

    for(const auto& addr : result)
    {
        if(std::find(dedupedResult.begin(), dedupedResult.end(), addr) == dedupedResult.end() )
            dedupedResult.push_back(addr);
    }

    return dedupedResult;
}

/**
 * Expand a "start-end" CPU core number range (e.g. "0-7") into the individual core numbers it
 * covers, inclusive of both ends. Mirrors expandTraddrRange() above for "traddr_range".
 *
 * @return the expanded core list, or an empty list if the range string is invalid.
 */
std::vector<int> expandCpuCoreRange(const std::string& rangeStr)
{
    std::vector<int> result;

    size_t dashPos = rangeStr.find('-');

    if(dashPos == std::string::npos)
    {
        ERRLOGGER(Log_NORMAL, "[SPDK] Invalid cpu_core_range (expected \"start-end\"): " <<
            rangeStr << std::endl);
        return result;
    }

    int start = std::atoi(rangeStr.substr(0, dashPos).c_str() );
    int end = std::atoi(rangeStr.substr(dashPos + 1).c_str() );

    if(end < start)
    {
        ERRLOGGER(Log_NORMAL, "[SPDK] cpu_core_range end is before start: " << rangeStr <<
            std::endl);
        return result;
    }

    for(int core = start; core <= end; core++)
        result.push_back(core);

    return result;
}

/**
 * Determine all CPU cores this process is currently allowed to run on (i.e. its own affinity
 * mask), used as the default "cpu_cores" list below. Note this must run after
 * SpdkNvmeClient::globalInit() has restored the calling thread's original affinity (narrowed by
 * spdk_env_init() ), so that it reflects the full set of cores actually available to elbencho
 * rather than just the single core DPDK's main lcore ends up on.
 *
 * @return the list of allowed core numbers, or just {0} if the affinity mask can't be read.
 */
std::vector<int> getAllAvailableCpuCores()
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);

    if(sched_getaffinity(0, sizeof(cpuset), &cpuset) != 0)
        return {0};

    std::vector<int> result;

    for(int core = 0; core < CPU_SETSIZE; core++)
    {
        if(CPU_ISSET(core, &cpuset) )
            result.push_back(core);
    }

    return result.empty() ? std::vector<int>{0} : result;
}

/**
 * Parse the "cpu_cores" (array of core numbers) and "cpu_core_range" ("start-end" string) fields
 * of the top-level config into one de-duplicated core number list. Mirrors parseTraddrList()
 * above for "traddr"/"traddr_range".
 *
 * @return the parsed/deduplicated core list, or every CPU core available to this process (see
 *     getAllAvailableCpuCores() ) if neither "cpu_cores" nor "cpu_core_range" is set, so that
 *     multiple I/O reactor threads (per "io_threads") spread across cores by default instead of
 *     all ending up on the same core.
 */
std::vector<int> parseCpuCoreList(const boost::json::object& configObj)
{
    std::vector<int> result;

    if(configObj.contains("cpu_cores") )
    {
        for(const auto& val : configObj.at("cpu_cores").as_array() )
            result.push_back(static_cast<int>(val.as_int64() ) );
    }

    if(configObj.contains("cpu_core_range") )
    {
        std::string rangeStr = std::string(configObj.at("cpu_core_range").as_string().c_str() );
        std::vector<int> rangeCores = expandCpuCoreRange(rangeStr);

        result.insert(result.end(), rangeCores.begin(), rangeCores.end() );
    }

    if(result.empty() )
        return getAllAvailableCpuCores();

    // de-duplicate while preserving order
    std::vector<int> dedupedResult;

    for(int core : result)
    {
        if(std::find(dedupedResult.begin(), dedupedResult.end(), core) == dedupedResult.end() )
            dedupedResult.push_back(core);
    }

    return dedupedResult;
}

/**
 * Parse a space-padded, not necessarily null-terminated fixed-size field from a discovery log
 * page entry (traddr/trsvcid) into a normal string. Mirrors how spdk's own
 * nvme_fabric_discover_probe() parses these same fields.
 */
std::string parsePaddedDiscoveryField(const uint8_t* field, size_t fieldSize)
{
    char buf[SPDK_NVMF_TRADDR_MAX_LEN + 1] = {}; // large enough for traddr/trsvcid alike
    size_t len = std::min(spdk_strlen_pad(field, fieldSize, ' '), sizeof(buf) - 1);

    memcpy(buf, field, len);
    buf[len] = '\0';
    spdk_str_chomp(buf);

    return std::string(buf);
}

/**
 * Run the given function on the given spdk_thread and block the *calling* (non-spdk) thread until
 * it has finished executing there. The dedicated background poller of "thread" (see
 * SpdkBdevManager::runAppThread()/runIoThread() ) picks up and executes the message the next time
 * it polls, so this must never be called from within a message/poller already running on
 * "thread" itself (that would deadlock).
 *
 * Note: fn must complete synchronously; it must NOT itself wait for some later, separate
 * asynchronous completion on the same thread (see runBdevNvmeCreate()/runBdevNvmeDelete() below
 * for that case), since fn runs to completion within a single spdk_thread_poll() call.
 */
void runOnThreadBlocking(struct spdk_thread* thread, const std::function<void()>& fn)
{
    struct SyncCtx
    {
        const std::function<void()>& fn;
        std::mutex mutex;
        std::condition_variable cv;
        bool done{false};
    } ctx{fn, {}, {}, false};

    spdk_thread_send_msg(thread, [](void* arg)
    {
        auto* ctxPtr = static_cast<SyncCtx*>(arg);
        ctxPtr->fn();

        std::lock_guard<std::mutex> lock(ctxPtr->mutex);
        ctxPtr->done = true;
        ctxPtr->cv.notify_one();
    }, &ctx);

    std::unique_lock<std::mutex> lock(ctx.mutex);
    ctx.cv.wait(lock, [&ctx] { return ctx.done; } );
}

/**
 * Tracks the outcome of an asynchronous spdk_bdev_nvme_create() call; owns copies of every
 * input the app thread needs, so the caller doesn't have to keep its own locals alive while
 * several of these are in flight concurrently (see kickoffBdevNvmeCreate() )
 */
struct CreateBdevState
{
    static constexpr size_t maxNames = 64; // generous upper bound on namespaces per subsystem

    std::mutex mutex;
    std::condition_variable cv;
    bool done{false};
    int rc{0};
    size_t bdevCount{0};
    const char* namesBuf[maxNames] = {};

    struct spdk_nvme_transport_id trid;
    std::string baseName;
    struct spdk_nvme_ctrlr_opts drvOpts;
    struct spdk_bdev_nvme_ctrlr_opts bdevOpts;
};

/**
 * Kick off spdk_bdev_nvme_create() for one (subsystem, path) combination, without waiting for
 * completion, so that several paths of the same multipath group can be connecting concurrently
 * (see waitBdevNvmeCreate() ). Every input is copied into the returned state up front, so it
 * stays valid regardless of what the caller's own locals do afterwards.
 */
std::shared_ptr<CreateBdevState> kickoffBdevNvmeCreate(struct spdk_thread* appThread,
    const struct spdk_nvme_transport_id& trid, const std::string& baseName,
    const struct spdk_nvme_ctrlr_opts& drvOpts, const struct spdk_bdev_nvme_ctrlr_opts& bdevOpts)
{
    auto state = std::make_shared<CreateBdevState>();
    state->trid = trid;
    state->baseName = baseName;
    state->drvOpts = drvOpts;
    state->bdevOpts = bdevOpts;

    auto* ctxPtr = new std::shared_ptr<CreateBdevState>(state);

    spdk_thread_send_msg(appThread, [](void* arg)
    {
        std::unique_ptr<std::shared_ptr<CreateBdevState>> ctx(
            static_cast<std::shared_ptr<CreateBdevState>*>(arg) );
        std::shared_ptr<CreateBdevState> state = *ctx;

        int createRc = spdk_bdev_nvme_create(&state->trid, state->baseName.c_str(),
            state->namesBuf, CreateBdevState::maxNames,
            [](void* cbArg, size_t bdevCount, int rc)
            {
                auto* statePtr = static_cast<CreateBdevState*>(cbArg);

                std::lock_guard<std::mutex> lock(statePtr->mutex);
                statePtr->rc = rc;
                statePtr->bdevCount = bdevCount;
                statePtr->done = true;
                statePtr->cv.notify_one();
            }, state.get(), &state->drvOpts, &state->bdevOpts);

        if(createRc != 0) // synchronous failure: the callback above will never fire
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->rc = createRc;
            state->done = true;
            state->cv.notify_one();
        }
    }, ctxPtr);

    return state;
}

/**
 * Block the calling (non-spdk) thread until the given kicked-off create finishes.
 *
 * @param outNames receives the bdev names belonging to this group (same content regardless of
 *     which path of the group created them).
 * @return true on success (this path is now part of the base_name's multipath group).
 */
bool waitBdevNvmeCreate(const std::shared_ptr<CreateBdevState>& state,
    std::vector<std::string>& outNames)
{
    std::unique_lock<std::mutex> lock(state->mutex);
    state->cv.wait(lock, [&state] { return state->done; } );

    if(state->rc != 0)
        return false;

    for(size_t i = 0; i < std::min(state->bdevCount, CreateBdevState::maxNames); i++)
        outNames.push_back(state->namesBuf[i]);

    return true;
}

/**
 * Delete the bdev_nvme group with the given base name (all its paths/bdevs) and wait for the
 * (possibly asynchronous) result.
 */
void runBdevNvmeDelete(struct spdk_thread* appThread, const std::string& baseName)
{
    auto state = std::make_shared<CreateBdevState>();

    struct KickoffCtx
    {
        std::string baseName;
        std::shared_ptr<CreateBdevState> state;
    };

    auto* kickoffCtx = new KickoffCtx{baseName, state};

    spdk_thread_send_msg(appThread, [](void* arg)
    {
        std::unique_ptr<KickoffCtx> ctx(static_cast<KickoffCtx*>(arg) );

        /* spdk_bdev_nvme_delete() requires a non-NULL path_id or it fails with -EINVAL, but the
           real "struct spdk_nvme_path_id" is only forward-declared in the public headers (its
           full definition is bdev_nvme-internal); a zeroed buffer at least as large as its real
           fields (trid + hostid + a TAILQ link + a tsc) has the same effect as a zeroed real
           struct - i.e. "match/delete every path of this group" - without needing that internal
           header */
        std::vector<uint8_t> pathIdBuf(sizeof(struct spdk_nvme_transport_id) +
            sizeof(struct spdk_nvme_host_id) + 64, 0);
        auto* pathId = reinterpret_cast<const struct spdk_nvme_path_id*>(pathIdBuf.data() );

        int deleteRc = spdk_bdev_nvme_delete(ctx->baseName.c_str(), pathId,
            [](void* cbArg, int rc)
            {
                auto* statePtr = static_cast<CreateBdevState*>(cbArg);

                std::lock_guard<std::mutex> lock(statePtr->mutex);
                statePtr->rc = rc;
                statePtr->done = true;
                statePtr->cv.notify_one();
            }, ctx->state.get() );

        if(deleteRc != 0) // synchronous failure: the callback above will never fire
        {
            std::lock_guard<std::mutex> lock(ctx->state->mutex);
            ctx->state->rc = deleteRc;
            ctx->state->done = true;
            ctx->state->cv.notify_one();
        }
    }, kickoffCtx);

    std::unique_lock<std::mutex> lock(state->mutex);
    state->cv.wait(lock, [&state] { return state->done; } );

    if(state->rc != 0)
        ERRLOGGER(Log_NORMAL, "[SPDK] Failed to delete bdev_nvme group '" << baseName << "': " <<
            spdk_strerror(-state->rc) << std::endl);
}

/**
 * Real per-namespace identity info that the plain bdev API doesn't expose (see
 * getNvmeIdentityInfo() below for why this needs the low-level ctrlr/ns objects instead)
 */
struct NvmeIdentityInfo
{
    std::string model; // real controller model number, e.g. from the ctrlr's identify data
    std::string uuid; // real target-provided namespace UUID, formatted with dashes; empty if none
    std::string nguid; // target-provided NGUID, formatted as plain hex; empty if none
    size_t connectedPathCount{0}; // number of currently connected paths for this ctrlr group
};

/**
 * Look up the real controller model, namespace UUID/NGUID and connected path count for one
 * bdev_nvme group + namespace ID, using the low-level spdk_nvme_ctrlr/spdk_nvme_ns objects that
 * bdev_nvme manages internally underneath the bdev API.
 *
 * This is needed because the generic bdev-level fields don't carry this info faithfully:
 * spdk_bdev_get_product_name() is always one of a handful of fixed, generic strings (e.g.
 * "NVMe disk"), not the target's actual model, and spdk_bdev_get_uuid() actually returns the raw
 * NGUID bytes whenever the target has no real UUID (that's bdev_nvme's own fallback order), so
 * displaying it unconditionally as "UUID" would be misleading for such targets.
 *
 * Must be called from the app thread, like the spdk_bdev_nvme_ctrlr_*() lookups it uses.
 *
 * @return best-effort info; any field stays at its default if it could not be determined (e.g.
 *     because the group/namespace was not found).
 */
NvmeIdentityInfo getNvmeIdentityInfo(const std::string& baseName, uint32_t nsId)
{
    NvmeIdentityInfo info;

    struct spdk_bdev_nvme_ctrlr* nbdevCtrlr = NULL;

    for(struct spdk_bdev_nvme_ctrlr* c = spdk_bdev_nvme_first_bdev_ctrlr(); c;
        c = spdk_bdev_nvme_next_bdev_ctrlr(c) )
    {
        if(baseName == spdk_bdev_nvme_ctrlr_get_name(c) )
        {
            nbdevCtrlr = c;
            break;
        }
    }

    if(!nbdevCtrlr)
        return info;

    for(struct spdk_nvme_ctrlr* ctrlr = spdk_bdev_nvme_ctrlr_first_ctrlr(nbdevCtrlr); ctrlr;
        ctrlr = spdk_bdev_nvme_ctrlr_next_ctrlr(nbdevCtrlr, ctrlr) )
    {
        info.connectedPathCount++;

        if(info.model.empty() )
        {
            const struct spdk_nvme_ctrlr_data* cdata = spdk_nvme_ctrlr_get_data(ctrlr);
            const char* mn = reinterpret_cast<const char*>(cdata->mn); // mn is int8_t[40]
            size_t mnLen = strnlen(mn, sizeof(cdata->mn) ); // space-padded, not necessarily
                // null-terminated, but some vendors terminate it early anyway
            info.model = trimRight(std::string(mn, mnLen) );
        }

        if(!info.uuid.empty() || !info.nguid.empty() )
            continue; // already have namespace identity info from an earlier path

        struct spdk_nvme_ns* ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsId);

        if(!ns)
            continue;

        const struct spdk_uuid* uuidPtr = spdk_nvme_ns_get_uuid(ns);

        if(uuidPtr)
        {
            char uuidBuf[SPDK_UUID_STRING_LEN];
            spdk_uuid_fmt_lower(uuidBuf, sizeof(uuidBuf), uuidPtr);
            info.uuid = uuidBuf;
            continue;
        }

        const uint8_t* nguidPtr = spdk_nvme_ns_get_nguid(ns);

        if(nguidPtr)
        {
            char nguidBuf[33];

            for(size_t i = 0; i < 16; i++)
                snprintf(&nguidBuf[i * 2], 3, "%02x", nguidPtr[i]);

            info.nguid = nguidBuf;
        }
    }

    return info;
}

} // anonymous namespace


SpdkBdevManager& SpdkBdevManager::instance()
{
    static SpdkBdevManager mgr;
    return mgr;
}

/**
 * Attach this caller's interest in the shared bdev_nvme connection pool. The first caller (per
 * process, or after a preceding detach() ) does the full setup from the given config; subsequent
 * concurrent/later callers just join it (their configJsonStr is assumed identical, which holds
 * for elbencho since all worker threads share the same config).
 *
 * @return true if the pool is now usable (i.e. has at least one namespace), false on setup
 *     failure (nothing is left attached in that case).
 */
bool SpdkBdevManager::attach(const std::string& configJsonStr)
{
    std::lock_guard<std::mutex> lock(mutex);

    if(refCount > 0)
    {
        refCount++;
        return true;
    }

    bool attachRes = doAttach(configJsonStr);

    if(!attachRes)
    {
        doDetach(); // clean up whatever partial state doAttach() left behind
        return false;
    }

    refCount = 1;
    return true;
}

/**
 * Release this caller's interest in the shared bdev_nvme connection pool. Once the last caller
 * has detached, every NVMe-oF connection is torn down and the I/O thread pool is stopped.
 */
void SpdkBdevManager::detach()
{
    std::lock_guard<std::mutex> lock(mutex);

    if(refCount <= 0)
        return;

    refCount--;

    if(refCount == 0)
        doDetach();
}

/**
 * Assign the next CPU core to a reactor thread, round-robin across cpuCoreList.
 *
 * @return the assigned core number; only meaningful if !cpuCoreList.empty().
 */
int SpdkBdevManager::assignCpuCore()
{
    size_t idx = nextCpuCoreAssign.fetch_add(1, std::memory_order_relaxed) % cpuCoreList.size();

    return cpuCoreList[idx];
}

/**
 * Bind the calling OS thread to its round-robin-assigned CPU core from cpuCoreList. No-op only if
 * cpuCoreList is unexpectedly empty (see parseCpuCoreList(), which normally defaults it to every
 * core available to this process). Only ever affects the calling thread itself.
 */
void SpdkBdevManager::bindCallingThreadToAssignedCore()
{
    if(cpuCoreList.empty() )
        return;

    int core = assignCpuCore();

    try
    {
        NumaTk::bindToCPUCore(core);
    }
    catch(const std::exception& e)
    {
        ERRLOGGER(Log_NORMAL, "[SPDK] Warning: failed to bind reactor thread to CPU core " <<
            core << ": " << e.what() << std::endl);
    }
}

/**
 * Run the given spdk_thread's poll loop until *stop becomes true, using interrupt-mode (blocking
 * epoll wait, no busy polling) if spdk_interrupt_mode_is_enabled(), or a plain busy-poll loop
 * otherwise.
 *
 * @param idleSleepDuration if non-zero and not in interrupt mode, sleep for this long whenever a
 *     poll call processed no events, trading a bit of latency for much lower CPU usage on
 *     threads that are mostly idle (e.g. the app thread); pass 0 (the default) to keep busy-
 *     polling on every idle iteration, which is appropriate for latency-critical I/O threads.
 */
static void pollThreadUntilStopped(struct spdk_thread* thread, const std::atomic<bool>& stop,
    std::chrono::microseconds idleSleepDuration = std::chrono::microseconds(0) )
{
    if(!spdk_interrupt_mode_is_enabled() )
    {
        while(!stop.load(std::memory_order_relaxed) )
        {
            int numEvents = spdk_thread_poll(thread, 0, 0);

            if( (idleSleepDuration.count() != 0) && (numEvents == 0) )
                std::this_thread::sleep_for(idleSleepDuration);
        }

        return;
    }

    spdk_set_thread(thread);
    spdk_thread_set_interrupt_mode(true);
    struct spdk_fd_group* fdGroup = spdk_thread_get_interrupt_fd_group(thread);
    spdk_set_thread(NULL);

    while(!stop.load(std::memory_order_relaxed) )
    {
        spdk_fd_group_wait(fdGroup, 1000); // blocks (epoll) up to 1s; wakes early on activity
        spdk_thread_poll(thread, 0, 0);
    }
}

/**
 * Background poll loop for the "app" spdk_thread, which owns all bdev_nvme setup/teardown
 * (create/open/close/delete). Runs for as long as this manager is attached.
 *
 * Note: this reuses SpdkNvmeClient::getGlobalAppThread() (created once in globalInit() and kept
 * alive for the whole process) rather than creating a separate spdk_thread here, because
 * bdev_nvme internally posts some of its own continuations (e.g. module examine, parts of
 * controller teardown) to spdk's single global "app thread" (spdk_thread_get_app_thread() )
 * regardless of which thread actually issued the call; a second, separate thread would never see
 * those messages and any operation relying on them would hang.
 *
 * Note: true SPDK interrupt mode can't be used here independent of the "interrupt_mode" config
 * (see spdk_interrupt_mode_is_enabled() ), because it is a single process-wide, one-way switch
 * and bdev_nvme unconditionally refuses to create any non-PCIe (i.e. TCP/RDMA) bdev once it is
 * enabled; so instead this thread falls back to a short idle sleep between busy-poll iterations
 * (see APP_THREAD_IDLE_SLEEP below) to avoid spinning at 100% CPU while otherwise idle.
 */
static const std::chrono::microseconds APP_THREAD_IDLE_SLEEP(1000);

void SpdkBdevManager::runAppThread(std::promise<void> readyPromise)
{
#ifdef THREADNAME_SUPPORT
    // set thread name (max 15 chars plus '\0')
    pthread_setname_np(pthread_self(), "elb-spdk-app");
#endif

    bindCallingThreadToAssignedCore();

    appThread = SpdkNvmeClient::getGlobalAppThread();
    readyPromise.set_value();

    pollThreadUntilStopped(appThread, appThreadStop, APP_THREAD_IDLE_SLEEP);

    // do NOT exit/destroy appThread here: it is process-wide and owned by globalUninit()
    appThread = NULL;
}

/**
 * Background loop for one of the dedicated "I/O" spdk_threads, through which elbencho worker
 * threads submit their actual read/write/trim commands (see SpdkNvmeClient::read() etc). Runs
 * for as long as this manager is attached.
 */
void SpdkBdevManager::runIoThread(size_t ioThreadIdx, std::promise<void> readyPromise)
{
#ifdef THREADNAME_SUPPORT
    // set thread name (max 15 chars plus '\0')
    std::string threadName = "elb-spdk-io" + std::to_string(ioThreadIdx);
    pthread_setname_np(pthread_self(), threadName.c_str() );
#endif

    bindCallingThreadToAssignedCore();

    std::string spdkThreadName = "elb_spdk_io" + std::to_string(ioThreadIdx);

    ioThreads[ioThreadIdx] = spdk_thread_create(spdkThreadName.c_str(), NULL);
    readyPromise.set_value();

    pollThreadUntilStopped(ioThreads[ioThreadIdx], ioThreadsStop);

    spdk_set_thread(ioThreads[ioThreadIdx]);
    spdk_thread_exit(ioThreads[ioThreadIdx]);

    while(!spdk_thread_is_exited(ioThreads[ioThreadIdx]) )
        spdk_thread_poll(ioThreads[ioThreadIdx], 0, 0);

    spdk_thread_destroy(ioThreads[ioThreadIdx]);
    ioThreads[ioThreadIdx] = NULL;
}

/**
 * Actual one-time setup: parse the config, spawn the app/I/O threads, resolve every subsystem
 * entry into its logical (multipath) groups and create/open the corresponding bdevs.
 *
 * @return true if at least one namespace ended up usable.
 */
bool SpdkBdevManager::doAttach(const std::string& configJsonStr)
{
    boost::system::error_code parseError;
    boost::json::value configVal = boost::json::parse(configJsonStr, parseError);

    if(parseError)
    {
        ERRLOGGER(Log_NORMAL, "[SPDK] JSON parse error: " << parseError.message() << std::endl);
        return false;
    }

    const auto& configObj = configVal.as_object();

    Filters filters;

    if(configObj.contains("filter") )
    {
        const auto& filterObj = configObj.at("filter").as_object();

        if(filterObj.contains("models") )
        {
            for(const auto& val : filterObj.at("models").as_array() )
                filters.allowedModels.push_back(std::string(val.as_string().c_str() ) );
        }
    }

    // determine the host NQN (empty means "use spdk's own default")

    std::string hostNqn;

    if(configObj.contains("host_nqn") )
        hostNqn = std::string(configObj.at("host_nqn").as_string().c_str() );
    else
        hostNqn = readSystemHostNqn();

    // determine the number of dedicated I/O threads (default 1)

    size_t numIoThreads = 1;

    if(configObj.contains("io_threads") )
        numIoThreads = std::max<int64_t>(1, configObj.at("io_threads").as_int64() );

    // determine the multipath policy (default: round-robin across all healthy paths)

    uint8_t multipathPolicy = SPDK_BDEV_NVME_MULTIPATH_POLICY_ACTIVE_ACTIVE;
    uint8_t multipathSelector = SPDK_BDEV_NVME_MULTIPATH_SELECTOR_ROUND_ROBIN;

    if(configObj.contains("multipath_policy") )
    {
        std::string policyStr = std::string(configObj.at("multipath_policy").as_string().c_str() );

        if(policyStr == "failover")
            multipathPolicy = SPDK_BDEV_NVME_MULTIPATH_POLICY_ACTIVE_PASSIVE;
        else if(policyStr != "roundrobin")
            ERRLOGGER(Log_NORMAL, "[SPDK] Unknown multipath_policy '" << policyStr << "', using "
                "'roundrobin'." << std::endl);
    }

    /* determine the CPU core list for the app/I/O reactor threads: explicit "cpu_cores"/
       "cpu_core_range" if given, otherwise every core available to this process (see
       parseCpuCoreList() ), so reactors round-robin across cores by default instead of all
       piling onto the same one */

    cpuCoreList = parseCpuCoreList(configObj);
    nextCpuCoreAssign = 0;

    if(!configObj.contains("subsystems") || !configObj.at("subsystems").is_array() )
    {
        ERRLOGGER(Log_NORMAL, "[SPDK] Config missing 'subsystems' array." << std::endl);
        return false;
    }

    const auto& subsystems = configObj.at("subsystems").as_array();

    // spawn the dedicated app thread, which owns every bdev_nvme create/open/close/delete call

    std::promise<void> appReadyPromise;
    std::future<void> appReadyFuture = appReadyPromise.get_future();

    appThreadStop = false;
    appThreadRunner = std::thread(&SpdkBdevManager::runAppThread, this, std::move(appReadyPromise) );
    appReadyFuture.wait();

    // spawn the dedicated I/O thread pool, through which all workers submit their actual I/O

    ioThreads.assign(numIoThreads, NULL);
    ioThreadsStop = false;

    std::vector<std::future<void>> ioReadyFutures;

    for(size_t i = 0; i < numIoThreads; i++)
    {
        std::promise<void> readyPromise;
        ioReadyFutures.push_back(readyPromise.get_future() );
        ioThreadRunners.emplace_back(&SpdkBdevManager::runIoThread, this, i,
            std::move(readyPromise) );
    }

    for(auto& future : ioReadyFutures)
        future.wait();

    // resolve every config entry into its logical subsystem group(s) and create their bdevs

    uint32_t defaultSubsystemIdx = 0;
    uint32_t groupCounter = 0;

    for(const auto& item : subsystems)
    {
        const auto& sub = item.as_object();

        bool hasExplicitName = sub.contains("name");
        std::string subName;

        if(hasExplicitName)
            subName = std::string(sub.at("name").as_string().c_str() );
        else
            subName = "sys" + std::to_string(defaultSubsystemIdx++); // only for pre-resolve logs

        std::string ctrlrName = sub.contains("controller_name") ?
            std::string(sub.at("controller_name").as_string().c_str() ) : "";

        spdk_nvme_transport_type trType = SPDK_NVME_TRANSPORT_TCP; // default to TCP

        std::string trTypeStr = sub.contains("trtype") ?
            std::string(sub.at("trtype").as_string().c_str() ) : "TCP";

        if(spdk_nvme_transport_id_parse_trtype(&trType, trTypeStr.c_str() ) != 0)
        {
            ERRLOGGER(Log_NORMAL, "[SPDK] Unknown transport type: " << trTypeStr << std::endl);
            continue;
        }

        std::vector<std::string> traddrList = parseTraddrList(sub);
        std::string trSvcId = sub.contains("trsvcid") ?
            std::string(sub.at("trsvcid").as_string().c_str() ) : "";
        std::string configuredNqn = sub.contains("nqn") ?
            std::string(sub.at("nqn").as_string().c_str() ) : "";
        bool useDiscoveredPaths = sub.contains("use_discovered_paths") &&
            sub.at("use_discovered_paths").as_bool();

        struct spdk_nvme_transport_id baseTrid = {};
        baseTrid.trtype = trType;
        baseTrid.adrfam = SPDK_NVMF_ADRFAM_IPV4;
        snprintf(baseTrid.trsvcid, sizeof(baseTrid.trsvcid), "%s", trSvcId.c_str() );

        std::vector<SubsystemGroup> groups;

        if(!configuredNqn.empty() )
        {
            /* explicit subsystem: one group, using the configured addresses (+ optionally
               discovery-learned extras for the same subnqn) */

            SubsystemGroup group;
            group.groupName = hasExplicitName ? subName : defaultSubsystemNameFromNqn(configuredNqn);
            group.subNqn = configuredNqn;

            for(const auto& addr : traddrList)
                group.addrs.push_back(std::make_pair(addr, trSvcId) );

            if(useDiscoveredPaths)
            {
                if(traddrList.empty() )
                    ERRLOGGER(Log_NORMAL, "[SPDK] \"use_discovered_paths\" requires at least one "
                        "'traddr'/'traddr_range' entry for subsystem '" << subName << "'." <<
                        std::endl);
                else
                {
                    struct spdk_nvme_transport_id discBaseTrid = baseTrid;
                    snprintf(discBaseTrid.traddr, sizeof(discBaseTrid.traddr), "%s",
                        traddrList[0].c_str() );

                    std::vector<DiscoveredPath> discovered = queryDiscoveryLog(discBaseTrid,
                        hostNqn);

                    for(const auto& discoveredPath : discovered)
                    {
                        if(discoveredPath.subNqn != configuredNqn)
                            continue;

                        auto addrPair = std::make_pair(discoveredPath.traddr,
                            discoveredPath.trsvcid);

                        if(std::find(group.addrs.begin(), group.addrs.end(), addrPair) ==
                            group.addrs.end() )
                            group.addrs.push_back(addrPair);
                    }
                }
            }

            if(group.addrs.empty() )
            {
                ERRLOGGER(Log_NORMAL, "[SPDK] Subsystem '" << subName << "' has no address to "
                    "connect to (add 'traddr'/'traddr_range', or enable "
                    "'use_discovered_paths')." << std::endl);
                continue;
            }

            groups.push_back(group);
        }
        else
        {
            /* discovery mode ('nqn' omitted): learn both the subnqn(s) and their paths, applying
               our own hostNqn to the discovery connection (see queryDiscoveryLog() ) */

            if(traddrList.empty() )
            {
                ERRLOGGER(Log_NORMAL, "[SPDK] Subsystem '" << subName << "' has neither 'nqn' "
                    "nor any 'traddr'/'traddr_range' to query for discovery." << std::endl);
                continue;
            }

            /* query every given traddr's own discovery service and merge the results, since a
               target's discovery service commonly only reports its own local path rather than
               every controller's paths (e.g. one discovery service per target IP in a multipath
               setup); paths are deduplicated per subnqn below, in case the same path gets
               reported by more than one discovery query */
            std::vector<std::string> orderedSubNqns;
            std::map<std::string, std::vector<std::pair<std::string, std::string>>> pathsBySubNqn;

            for(const auto& traddr : traddrList)
            {
                struct spdk_nvme_transport_id discBaseTrid = baseTrid;
                snprintf(discBaseTrid.traddr, sizeof(discBaseTrid.traddr), "%s", traddr.c_str() );

                std::vector<DiscoveredPath> discovered = queryDiscoveryLog(discBaseTrid, hostNqn);

                for(const auto& discoveredPath : discovered)
                {
                    if(pathsBySubNqn.find(discoveredPath.subNqn) == pathsBySubNqn.end() )
                        orderedSubNqns.push_back(discoveredPath.subNqn);

                    auto addrPair = std::make_pair(discoveredPath.traddr, discoveredPath.trsvcid);

                    auto& subNqnPaths = pathsBySubNqn[discoveredPath.subNqn];

                    if(std::find(subNqnPaths.begin(), subNqnPaths.end(), addrPair) ==
                        subNqnPaths.end() )
                        subNqnPaths.push_back(addrPair);
                }
            }

            if(orderedSubNqns.empty() )
            {
                ERRLOGGER(Log_NORMAL, "[SPDK] Discovery at " <<
                    StringTk::vecToStr(traddrList, ",") << ":" << trSvcId <<
                    " reported no subsystems for subsystem entry '" << subName << "'." <<
                    std::endl);
                continue;
            }

            for(size_t i = 0; i < orderedSubNqns.size(); i++)
            {
                SubsystemGroup group;
                group.subNqn = orderedSubNqns[i];

                /* discovered subNqns are unique per group, so no "_i" suffix is needed for the
                   NQN-based default name; only the explicit user name needs disambiguation. */
                if(hasExplicitName)
                    group.groupName = (orderedSubNqns.size() == 1) ? subName :
                        (subName + "_" + std::to_string(i) );
                else
                    group.groupName = defaultSubsystemNameFromNqn(group.subNqn);

                group.addrs = pathsBySubNqn[group.subNqn];

                groups.push_back(group);
            }
        }

        // create the bdev(s) for every resolved group

        for(const auto& group : groups)
        {
            std::string baseName = (ctrlrName.empty() ? "ctrlr" : ctrlrName) +
                std::to_string(groupCounter++);

            std::vector<std::string> groupBdevNames;
            bool anyPathOk = false;
            size_t connectedPathCount = 0;

            /* kick off every path's connect up front (they proceed concurrently on the app
               thread's poller) instead of waiting for each one before starting the next, since
               each path can independently take several seconds (e.g. first-contact ARP
               resolution to a not-yet-touched destination address) */

            struct PendingCreate
            {
                std::string addr;
                std::string svcId;
                spdk_nvme_transport_type trtype;
                std::shared_ptr<CreateBdevState> state;
            };

            std::vector<PendingCreate> pending;
            pending.reserve(group.addrs.size() );

            for(const auto& addrPair : group.addrs)
            {
                const std::string& addr = addrPair.first;
                const std::string& svcId = addrPair.second;

                struct spdk_nvme_transport_id trid = baseTrid;
                snprintf(trid.traddr, sizeof(trid.traddr), "%s", addr.c_str() );
                snprintf(trid.trsvcid, sizeof(trid.trsvcid), "%s", svcId.c_str() );
                snprintf(trid.subnqn, sizeof(trid.subnqn), "%s", group.subNqn.c_str() );

                struct spdk_nvme_ctrlr_opts drvOpts;
                spdk_nvme_ctrlr_get_default_ctrlr_opts(&drvOpts, sizeof(drvOpts) );

                if(!hostNqn.empty() )
                    snprintf(drvOpts.hostnqn, sizeof(drvOpts.hostnqn), "%s", hostNqn.c_str() );

                drvOpts.keep_alive_timeout_ms = 3600000; // 1 hour, because spdk lacks auto-reconnect
                drvOpts.fabrics_connect_timeout_us = 10 * 1000 * 1000; // 10s

                /* zero-init first: spdk_bdev_nvme_get_default_ctrlr_opts() leaves psk/dhchap_*
                   untouched, so a plain stack struct would pass garbage pointers to bdev_nvme */
                struct spdk_bdev_nvme_ctrlr_opts bdevOpts = {};
                spdk_bdev_nvme_get_default_ctrlr_opts(&bdevOpts);
                bdevOpts.multipath = true;
                bdevOpts.multipath_policy = multipathPolicy;
                bdevOpts.multipath_selector = multipathSelector;

                auto state = kickoffBdevNvmeCreate(appThread, trid, baseName, drvOpts, bdevOpts);
                pending.push_back({addr, svcId, trid.trtype, state});
            }

            for(auto& p : pending)
            {
                std::vector<std::string> pathBdevNames;

                bool createOk = waitBdevNvmeCreate(p.state, pathBdevNames);

                if(!createOk)
                {
                    ERRLOGGER(Log_NORMAL, "[SPDK] Failed to create/extend bdev for " <<
                        spdk_nvme_transport_id_trtype_str(p.trtype) << "://" << p.addr << ":" <<
                        p.svcId << "; Subsystem: " << group.subNqn << std::endl);
                    continue;
                }

                anyPathOk = true;
                connectedPathCount++;
                groupBdevNames = pathBdevNames; // same set again on every successful path
            }

            if(!anyPathOk)
            {
                ERRLOGGER(Log_NORMAL, "[SPDK] Warning: subsystem '" << group.groupName <<
                    "' has no usable path; skipping." << std::endl);
                continue;
            }

            LOGGER(Log_VERBOSE, "[SPDK] Subsystem '" << group.groupName << "' (" << baseName <<
                "): " << connectedPathCount << "/" << group.addrs.size() <<
                " announced paths connected" << std::endl);

            createdBaseNames.push_back(baseName);

            // open every bdev of this group and register it as a namespace

            for(const auto& bdevName : groupBdevNames)
            {
                struct spdk_bdev_desc* desc = NULL;

                runOnThreadBlocking(appThread, [&]()
                {
                    int openRc = spdk_bdev_open_ext(bdevName.c_str(), true,
                        [](enum spdk_bdev_event_type, struct spdk_bdev*, void*) {}, NULL, &desc);

                    if(openRc != 0)
                    {
                        ERRLOGGER(Log_NORMAL, "[SPDK] Failed to open bdev '" << bdevName <<
                            "': " << spdk_strerror(-openRc) << std::endl);
                        desc = NULL;
                    }
                });

                if(!desc)
                    continue;

                struct spdk_bdev* bdev = spdk_bdev_desc_get_bdev(desc);

                /* the bdev's "<base_name>n<nsid>" suffix also gives us the raw NVMe namespace ID,
                   needed below to look up its real identity info */
                std::string rawNsSuffix = (bdevName.size() > baseName.size() ) ?
                    bdevName.substr(baseName.size() ) : bdevName;
                uint32_t bdevNsId = (!rawNsSuffix.empty() && (rawNsSuffix[0] == 'n') ) ?
                    static_cast<uint32_t>(std::atoi(rawNsSuffix.c_str() + 1) ) : 0;

                /* the generic bdev-level fields don't carry this faithfully: product_name is
                   always one of a handful of fixed strings (e.g. "NVMe disk"), never the
                   target's actual model, and spdk_bdev_get_uuid() actually returns the raw NGUID
                   bytes whenever the target has no real UUID (bdev_nvme's own fallback), so get
                   the real model/UUID/NGUID from the underlying ctrlr/ns objects instead; this
                   must run on the app thread, like every other access to bdev_nvme's internal
                   state */
                NvmeIdentityInfo idInfo;

                runOnThreadBlocking(appThread, [&]()
                {
                    idInfo = getNvmeIdentityInfo(baseName, bdevNsId);
                });

                std::string model = !idInfo.model.empty() ? idInfo.model :
                    trimRight(spdk_bdev_get_product_name(bdev) ?
                        spdk_bdev_get_product_name(bdev) : "");

                if(!isNamespaceAllowed(filters, model) )
                {
                    LOGGER(Log_VERBOSE, "[SPDK] Info: Skipping namespace '" << bdevName << "' "
                        "(filter mismatch)" << std::endl);

                    runOnThreadBlocking(appThread, [desc]() { spdk_bdev_close(desc); } );
                    continue;
                }

                NamespaceHandle nsHandle;
                nsHandle.id = nextNsId++;
                nsHandle.desc = desc;
                nsHandle.sizeBytes = spdk_bdev_get_num_blocks(bdev) *
                    spdk_bdev_get_block_size(bdev);
                nsHandle.sectorSize = spdk_bdev_get_block_size(bdev);
                nsHandle.uuid = idInfo.uuid;
                nsHandle.nguid = idInfo.nguid;
                nsHandle.model = model;

                // friendly display name in the style of the old low-level API: turn the bdev's
                // "n<nsid>" suffix into "ns<nsid>"
                std::string nsSuffix = (!rawNsSuffix.empty() && (rawNsSuffix[0] == 'n') ) ?
                    ("s" + rawNsSuffix.substr(1) ) : rawNsSuffix;

                nsHandle.fullName = group.groupName + ":" + baseName + ":n" + nsSuffix;

                for(size_t i = 0; i < numIoThreads; i++)
                {
                    struct spdk_io_channel* channel = NULL;
                    struct spdk_thread* ioThread = ioThreads[i];

                    runOnThreadBlocking(ioThread, [desc, &channel]()
                    {
                        channel = spdk_bdev_get_io_channel(desc);
                    });

                    if(!channel)
                    {
                        /* typically means the target/controller has fewer I/O queues available
                           than the configured "io_threads" (e.g. a hard per-controller limit);
                           fail fast here rather than continuing with a namespace that would
                           later crash on a null spdk_io_channel */
                        ERRLOGGER(Log_NORMAL, "[SPDK] Target for '" << bdevName << "' only "
                            "supports " << i << " I/O queue(s) per path, but io_threads is "
                            "configured to " << numIoThreads << ". Reduce \"io_threads\" in "
                            "the SPDK JSON config to at most " << i << "." << std::endl);

                        /* nsHandle isn't registered in "namespaces" yet, so doDetach() (called
                           by the caller after this returns false) doesn't know about the
                           channels/desc already obtained for it; release them here to avoid
                           leaking open qpairs that would otherwise block bdev_nvme_delete() and
                           make the I/O threads exit with dangling channels */
                        for(size_t j = 0; j < nsHandle.channels.size(); j++)
                        {
                            struct spdk_io_channel* leakedChannel = nsHandle.channels[j];
                            struct spdk_thread* leakedChannelThread = ioThreads[j];

                            runOnThreadBlocking(leakedChannelThread,
                                [leakedChannel]() { spdk_put_io_channel(leakedChannel); } );
                        }

                        runOnThreadBlocking(appThread, [desc]() { spdk_bdev_close(desc); } );

                        return false;
                    }

                    nsHandle.channels.push_back(channel);
                }

                // fall back to the NGUID for display only if the target has no real UUID
                bool showNguid = nsHandle.uuid.empty() && !nsHandle.nguid.empty();

                LOGGER(Log_VERBOSE, "[SPDK] Attached: " << nsHandle.fullName << " -> ID " <<
                    nsHandle.id << " "
                    "(Model: " << nsHandle.model << "; "
                    "Size: " << (nsHandle.sizeBytes / (1024*1024) ) << " MiB; "
                    "Format: " << nsHandle.sectorSize << "B; " <<
                    (showNguid ? "NGUID: " : "UUID: ") <<
                    (showNguid ? nsHandle.nguid : nsHandle.uuid) << ")" << std::endl);

                nameToId[nsHandle.fullName] = nsHandle.id;
                namespaces[nsHandle.id] = std::move(nsHandle);
            }
        }
    }

    if(namespaces.empty() )
    {
        ERRLOGGER(Log_NORMAL, "[SPDK] Init couldn't find any namespaces" << std::endl);
        return false;
    }

    return true;
}

/**
 * Tear down everything doAttach() built up: close every bdev descriptor and I/O channel, delete
 * every bdev_nvme group (dropping every NVMe-oF connection), then stop the I/O and app threads.
 */
void SpdkBdevManager::doDetach()
{
    for(auto& [nsId, nsHandle] : namespaces)
    {
        for(size_t i = 0; i < nsHandle.channels.size(); i++)
        {
            struct spdk_io_channel* channel = nsHandle.channels[i];

            if(!channel)
                continue;

            struct spdk_thread* ioThread = ioThreads[i];
            runOnThreadBlocking(ioThread, [channel]() { spdk_put_io_channel(channel); } );
        }

        if(nsHandle.desc)
        {
            struct spdk_bdev_desc* desc = nsHandle.desc;
            runOnThreadBlocking(appThread, [desc]() { spdk_bdev_close(desc); } );
        }
    }

    namespaces.clear();
    nameToId.clear();
    nextNsId = 0;

    for(const auto& baseName : createdBaseNames)
        runBdevNvmeDelete(appThread, baseName);

    createdBaseNames.clear();

    if(!ioThreadRunners.empty() )
    {
        ioThreadsStop = true;

        for(auto& runner : ioThreadRunners)
            if(runner.joinable() )
                runner.join();

        ioThreadRunners.clear();
    }

    ioThreads.clear();
    ioThreadsStop = false;

    if(appThreadRunner.joinable() )
    {
        appThreadStop = true;
        appThreadRunner.join();
    }

    appThreadStop = false;
}

/**
 * Get the numeric ID of a namespace from its human-friendly name.
 *
 * @return namespace ID or -1 if not found.
 */
int32_t SpdkBdevManager::getNamespaceId(const std::string& name)
{
    std::lock_guard<std::mutex> lock(mutex);

    auto iter = nameToId.find(name);

    return (iter == nameToId.end() ) ? -1 : static_cast<int32_t>(iter->second);
}

/**
 * Get the human-friendly name of a namespace from its numeric ID.
 *
 * @return name string or empty string if not found.
 */
std::string SpdkBdevManager::getNamespaceName(uint32_t nsId)
{
    std::lock_guard<std::mutex> lock(mutex);

    auto iter = namespaces.find(nsId);

    return (iter == namespaces.end() ) ? "" : iter->second.fullName;
}

/**
 * Get the list of all attached namespace IDs.
 */
std::vector<uint32_t> SpdkBdevManager::getNamespaceIds()
{
    std::lock_guard<std::mutex> lock(mutex);

    std::vector<uint32_t> nsIds;

    for(const auto& nsPair : namespaces)
        nsIds.push_back(nsPair.first);

    return nsIds;
}

/**
 * Get the handle (bdev descriptor + per-I/O-thread channels) for the given namespace.
 *
 * Note: This does not take the manager mutex, unlike the other getters. It is only intended to
 * be called by an already-attached SpdkNvmeClient between its own init() and disconnect(), during
 * which the returned pointer stays valid because the referenced namespace cannot be torn down
 * (this instance's own attach() keeps the refcount above zero for exactly that duration).
 *
 * @return pointer to the namespace handle, or NULL if not found.
 */
SpdkBdevManager::NamespaceHandle* SpdkBdevManager::getNamespace(uint32_t nsId)
{
    auto iter = namespaces.find(nsId);

    return (iter == namespaces.end() ) ? NULL : &iter->second;
}

/**
 * @return the number of dedicated I/O threads in the pool (only meaningful while attached).
 */
size_t SpdkBdevManager::getNumIoThreads()
{
    return ioThreads.size();
}

/**
 * @return the I/O thread at the given index (only meaningful while attached).
 */
struct spdk_thread* SpdkBdevManager::getIoThread(size_t ioThreadIdx)
{
    return ioThreads[ioThreadIdx];
}

/**
 * Assign the next I/O thread index to a newly attached SpdkNvmeClient, round-robin across the
 * pool, so that many worker threads' I/O gets spread evenly across the (small) shared pool.
 */
size_t SpdkBdevManager::assignIoThreadIdx()
{
    size_t numThreads = ioThreads.size();

    if(numThreads == 0)
        return 0;

    return nextIoThreadAssign.fetch_add(1, std::memory_order_relaxed) % numThreads;
}

/**
 * Read the host NQN from the standard system config file.
 *
 * @return host NQN or empty string if it could not be read.
 */
std::string SpdkBdevManager::readSystemHostNqn()
{
    std::ifstream fileStream("/etc/nvme/hostnqn");

    if(!fileStream.is_open() )
        return "";

    std::string line;

    if(!std::getline(fileStream, line) )
        return "";

    return trimRight(line);
}

/**
 * Check the given namespace's model against the config's filters.
 *
 * @return true if the namespace passes all filters, false to skip it.
 */
bool SpdkBdevManager::isNamespaceAllowed(const Filters& filters, const std::string& model)
{
    if(!filters.allowedModels.empty() && (std::find(filters.allowedModels.begin(),
        filters.allowedModels.end(), model) == filters.allowedModels.end() ) )
        return false;

    return true;
}

/**
 * Query a target's NVMe-oF discovery service for the subsystems and paths it reports, connecting
 * with the given host NQN so that host-NQN-based ACLs on the target's discovery service are
 * honored. (spdk's own built-in probe/scan path doesn't apply a custom host NQN to its internal
 * discovery connection, see nvme_fabric_ctrlr_scan() in external/spdk/lib/nvme/nvme_fabric.c;
 * that's why we do our own discovery connect + log page fetch here instead of relying on it.)
 *
 * @return the discovered paths, or an empty list on any failure (best-effort; callers fall back
 *     to their explicitly configured addresses in that case).
 */
std::vector<SpdkBdevManager::DiscoveredPath> SpdkBdevManager::queryDiscoveryLog(
    const struct spdk_nvme_transport_id& baseTrid, const std::string& hostNqn)
{
    std::vector<DiscoveredPath> result;

    struct spdk_nvme_transport_id discoveryTrid = baseTrid;
    snprintf(discoveryTrid.subnqn, sizeof(discoveryTrid.subnqn), "%s", SPDK_NVMF_DISCOVERY_NQN);

    struct spdk_nvme_ctrlr_opts opts;
    spdk_nvme_ctrlr_get_default_ctrlr_opts(&opts, sizeof(opts) );

    if(!hostNqn.empty() )
        snprintf(opts.hostnqn, sizeof(opts.hostnqn), "%s", hostNqn.c_str() );

    opts.fabrics_connect_timeout_us = 10 * 1000 * 1000; // 10s, consistent with doAttach()

    struct spdk_nvme_ctrlr* discoveryCtrlr = spdk_nvme_connect(&discoveryTrid, &opts,
        sizeof(opts) );

    if(!discoveryCtrlr)
    {
        ERRLOGGER(Log_NORMAL, "[SPDK] Could not connect to discovery service at " <<
            discoveryTrid.traddr << ":" << discoveryTrid.trsvcid << std::endl);
        return result;
    }

    struct DiscoveryLogState
    {
        bool done{false};
        int rc{0};
        struct spdk_nvmf_discovery_log_page* logPage{NULL};
    } state;

    int getLogRes = spdk_nvme_ctrlr_get_discovery_log_page(discoveryCtrlr,
        [](void* cbArg, int rc, const struct spdk_nvme_cpl* cpl,
            struct spdk_nvmf_discovery_log_page* logPage)
        {
            (void)cpl;

            auto* logState = static_cast<DiscoveryLogState*>(cbArg);
            logState->rc = rc;
            logState->logPage = logPage;
            logState->done = true;
        }, &state);

    if(getLogRes != 0)
    {
        ERRLOGGER(Log_NORMAL, "[SPDK] Failed to request discovery log page from " <<
            discoveryTrid.traddr << ":" << discoveryTrid.trsvcid << std::endl);
        spdk_nvme_detach(discoveryCtrlr);
        return result;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10); // 10s

    while(!state.done)
    {
        spdk_nvme_ctrlr_process_admin_completions(discoveryCtrlr);

        if(!state.done && (std::chrono::steady_clock::now() > deadline) )
        {
            ERRLOGGER(Log_NORMAL, "[SPDK] Timed out waiting for discovery log page from " <<
                discoveryTrid.traddr << ":" << discoveryTrid.trsvcid << std::endl);
            spdk_nvme_detach(discoveryCtrlr);
            return result;
        }
    }

    if( (state.rc != 0) || !state.logPage)
    {
        ERRLOGGER(Log_NORMAL, "[SPDK] Failed to retrieve discovery log page from " <<
            discoveryTrid.traddr << ":" << discoveryTrid.trsvcid << std::endl);
        spdk_nvme_detach(discoveryCtrlr);
        return result;
    }

    for(uint64_t i = 0; i < state.logPage->numrec; i++)
    {
        const struct spdk_nvmf_discovery_log_page_entry& entry = state.logPage->entries[i];

        if(entry.subtype != SPDK_NVMF_SUBTYPE_NVME)
            continue; // skip discovery-service-only / referral entries

        // subnqn must be null terminated within its field, unlike traddr/trsvcid which are
        // space-padded (see nvme_fabric_discover_probe() in nvme_fabric.c)
        const void* subNqnEnd = memchr(entry.subnqn, '\0', SPDK_NVMF_NQN_MAX_LEN + 1);

        if(!subNqnEnd)
        {
            ERRLOGGER(Log_NORMAL, "[SPDK] Skipping discovery entry with non-terminated SUBNQN" <<
                std::endl);
            continue;
        }

        DiscoveredPath path;
        path.trtype = entry.trtype;
        path.subNqn = std::string(reinterpret_cast<const char*>(entry.subnqn),
            static_cast<const uint8_t*>(subNqnEnd) - entry.subnqn);
        path.traddr = parsePaddedDiscoveryField(entry.traddr, sizeof(entry.traddr) );
        path.trsvcid = parsePaddedDiscoveryField(entry.trsvcid, sizeof(entry.trsvcid) );

        result.push_back(path);
    }

    free(state.logPage); // caller-owned, per spdk_nvme_ctrlr_get_discovery_log_page() docs
    spdk_nvme_detach(discoveryCtrlr);

    return result;
}

#endif // SPDK_SUPPORT
