// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifdef SPDK_SUPPORT

#include <algorithm>
#include <boost/json.hpp>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iostream>
#include <memory>
#include <sched.h>
#include <spdk/accel.h>
#include <spdk/bdev.h>
#include <spdk/env.h>
#include <spdk/log.h>
#include <spdk/nvme.h>
#include <spdk/stdinc.h>
#include <spdk/string.h>
#include <spdk/thread.h>
#include <sstream>
#include <syslog.h>
#include <unistd.h>

#include "Logger.h"
#include "SpdkBdevManager.h"
#include "SpdkNvmeClient.h"


bool SpdkNvmeClient::globalInitCalled = false;
std::recursive_mutex SpdkNvmeClient::initMutex;
struct spdk_thread* SpdkNvmeClient::bootstrapThread = NULL;

static FILE* spdkLogFileHandle = NULL; // optional "log_file" target, NULL means stderr


/**
 * Reproduce spdk's own "[<timestamp>] " log line prefix (see get_timestamp_prefix() in spdk's
 * lib/log/log.c), so that log lines passed through by logFilterCb() below look exactly like
 * spdk's own default output.
 */
static std::string getLogTimestampPrefix()
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    struct tm timeInfo;
    localtime_r(&ts.tv_sec, &timeInfo);

    char dateBuf[24];
    strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d %H:%M:%S", &timeInfo);

    char prefixBuf[64];
    snprintf(prefixBuf, sizeof(prefixBuf), "[%s.%06ld] ", dateBuf, ts.tv_nsec / 1000);

    return prefixBuf;
}

/**
 * Get current process CPU affinity as a comma-separated string, e.g. "0,1,2".
 */
static std::string getCoreListFromAffinity()
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);

    if(sched_getaffinity(0, sizeof(cpu_set_t), &cpuset) != 0)
        return "0";

    std::stringstream stream;
    bool isFirst = true;

    for(int i = 0; i < CPU_SETSIZE; i++)
    {
        if(!CPU_ISSET(i, &cpuset) )
            continue;

        if(!isFirst)
            stream << ",";

        stream << i;
        isFirst = false;
    }

    std::string coreList = stream.str();

    return coreList.empty() ? "0" : coreList;
}

/**
 * Determine whether the top-level "interrupt_mode" JSON config key is enabled. Deliberately
 * does its own minimal parse (instead of reusing SpdkBdevManager's) because this must be known
 * before spdk_thread_lib_init(), i.e. before any per-subsystem config parsing has happened.
 *
 * Note: default is disabled, not enabled, even though interrupt mode avoids busy-polling on the
 * app/I/O reactor threads: spdk's bdev_nvme module unconditionally refuses to create any bdev
 * for a non-PCIe transport (see SPDK_ERRLOG("Interrupt mode is only supported with PCIe
 * transport") in bdev_nvme.c) whenever spdk_interrupt_mode_is_enabled() is true process-wide, so
 * enabling this by default would break every TCP/RDMA NVMe-oF connection. (Interrupt mode support
 * might come at least for RDMA with SPDK v26.09 or later.)
 *
 * @return true if interrupt mode is enabled, false if disabled (the default) or the config could
 *     not be parsed at all.
 */
static bool isInterruptModeConfigEnabled(const std::string& configJsonStr)
{
    boost::system::error_code parseError;
    boost::json::value configVal = boost::json::parse(configJsonStr, parseError);

    if(parseError || !configVal.is_object() )
        return false; // default off; SpdkBdevManager::doAttach() will report the real parse error

    const auto& configObj = configVal.as_object();

    if(!configObj.contains("interrupt_mode") )
        return false; // default off, see note above

    return configObj.at("interrupt_mode").as_bool();
}

/**
 * Get the top-level "mem_size_mb" JSON config value (DPDK/spdk DMA memory budget in MiB, see
 * opts.mem_size). Does its own minimal parse, like isInterruptModeConfigEnabled() above, since
 * this must be known before spdk_env_init().
 *
 * @return configured value, or 8192 (the default) if unset or the config could not be parsed.
 */
static int getMemSizeMbConfig(const std::string& configJsonStr)
{
    boost::system::error_code parseError;
    boost::json::value configVal = boost::json::parse(configJsonStr, parseError);

    if(parseError || !configVal.is_object() )
        return 8192; // default; SpdkBdevManager::doAttach() will report the real parse error

    const auto& configObj = configVal.as_object();

    if(!configObj.contains("mem_size_mb") )
        return 8192; // default

    return static_cast<int>(configObj.at("mem_size_mb").as_int64() );
}

/**
 * Get the top-level "log_file" JSON config value (path to append spdk's own log messages to,
 * instead of the console). Does its own minimal parse, like isInterruptModeConfigEnabled()
 * above, since this must be known before spdk_log_open().
 *
 * @return configured path, or empty string if unset (default: log to console) or the config
 *     could not be parsed.
 */
static std::string getLogFileConfig(const std::string& configJsonStr)
{
    boost::system::error_code parseError;
    boost::json::value configVal = boost::json::parse(configJsonStr, parseError);

    if(parseError || !configVal.is_object() )
        return ""; // default; SpdkBdevManager::doAttach() will report the real parse error

    const auto& configObj = configVal.as_object();

    if(!configObj.contains("log_file") )
        return ""; // default: console

    return std::string(configObj.at("log_file").as_string().c_str() );
}

/**
 * Translate a "log_level" JSON config string into spdk's enum spdk_log_level. Recognizes
 * "disabled", "error", "warn", "notice", "info" and "debug" (case-insensitive), matching the
 * names spdk itself uses for --logflag/--log-level on its own example apps.
 *
 * @param levelStr the string to translate.
 * @param outLevel set to the translated level if the string was recognized; left untouched
 *     otherwise.
 * @return true if levelStr was recognized, false otherwise.
 */
static bool logLevelFromString(const std::string& levelStr, enum spdk_log_level& outLevel)
{
    std::string lowerStr = levelStr;
    std::transform(lowerStr.begin(), lowerStr.end(), lowerStr.begin(),
        [](unsigned char c) { return std::tolower(c); } );

    if(lowerStr == "disabled")
        outLevel = SPDK_LOG_DISABLED;
    else if(lowerStr == "error")
        outLevel = SPDK_LOG_ERROR;
    else if(lowerStr == "warn")
        outLevel = SPDK_LOG_WARN;
    else if(lowerStr == "notice")
        outLevel = SPDK_LOG_NOTICE;
    else if(lowerStr == "info")
        outLevel = SPDK_LOG_INFO;
    else if(lowerStr == "debug")
        outLevel = SPDK_LOG_DEBUG;
    else
        return false;

    return true;
}

/**
 * Get the top-level "log_level" JSON config value (spdk log level threshold, both for the
 * "set" and "print" thresholds, see spdk_log_set_level()/spdk_log_set_print_level() ). Does its
 * own minimal parse, like isInterruptModeConfigEnabled() above, since this must be known before
 * spdk_log_open().
 *
 * Note: elbencho's default (this function returning unset) deliberately leaves spdk's own
 * built-in default log level ("notice", i.e. only notice/warn/error get printed) untouched, so
 * that log verbosity stays exactly as it always has been unless a user explicitly opts in here.
 *
 * @return configured level string (e.g. "debug"), or empty string if unset (default: leave
 *     spdk's own default level untouched) or the config could not be parsed.
 */
static std::string getLogLevelConfig(const std::string& configJsonStr)
{
    boost::system::error_code parseError;
    boost::json::value configVal = boost::json::parse(configJsonStr, parseError);

    if(parseError || !configVal.is_object() )
        return ""; // default; SpdkBdevManager::doAttach() will report the real parse error

    const auto& configObj = configVal.as_object();

    if(!configObj.contains("log_level") )
        return ""; // default: leave spdk's own default level untouched

    return std::string(configObj.at("log_level").as_string().c_str() );
}

/**
 * Get the top-level "log_flags" JSON config value (list of spdk debug log flag names to enable,
 * e.g. "bdev_nvme", "nvme", passed to spdk_log_set_flag() ). Does its own minimal parse, like
 * isInterruptModeConfigEnabled() above, since this must be known before spdk_log_open().
 *
 * Note: unset (the default) enables no flags at all, i.e. behaves exactly as before this option
 * existed. Also note that flags gated behind spdk's SPDK_DEBUGLOG() macro only produce output if
 * spdk itself was built with debug logging enabled (spdk's "./configure --enable-debug", not the
 * default for elbencho's bundled spdk build) - "log_level": "debug" plus these flags still won't
 * show that particular subset of messages otherwise.
 *
 * @return configured flag names, or an empty vector if unset (default: no flags enabled) or the
 *     config could not be parsed.
 */
static std::vector<std::string> getLogFlagsConfig(const std::string& configJsonStr)
{
    boost::system::error_code parseError;
    boost::json::value configVal = boost::json::parse(configJsonStr, parseError);

    if(parseError || !configVal.is_object() )
        return {}; // default; SpdkBdevManager::doAttach() will report the real parse error

    const auto& configObj = configVal.as_object();

    if(!configObj.contains("log_flags") )
        return {}; // default: no flags enabled

    std::vector<std::string> flags;

    for(const auto& flagVal : configObj.at("log_flags").as_array() )
        flags.push_back(std::string(flagVal.as_string().c_str() ) );

    return flags;
}

/**
 * Initialize the SPDK environment and the process-wide frameworks needed for bdev_nvme (threads,
 * iobuf, accel, bdev). This sets up memory (no hugepages) and CPU affinity (lcore_map).
 *
 * Note: spdk_env_init() narrows the calling OS thread's own CPU affinity down to a single core
 * as a side effect (see dpdk's rte_eal_init() ); this restores the calling thread's original
 * affinity right after that call returns, so that this thread (typically elbencho's coordinator
 * thread) and anything it spawns afterwards (e.g. LocalWorker threads) keep running across all
 * originally available cores rather than getting stuck on just one.
 *
 * Note: This must be called once per process before any threads are started.
 *
 * @param configJsonStr the SPDK JSON config, used here only to read the top-level
 *     "interrupt_mode", "mem_size_mb", "log_file", "log_level" and "log_flags" keys, since all
 *     of them must take effect before spdk_env_init()/spdk_log_open(); the rest of the config is
 *     parsed later by SpdkBdevManager.
 * @return 0 on success, negative SPDK error code otherwise.
 */
int SpdkNvmeClient::globalInit(const std::string& configJsonStr)
{
    std::lock_guard<std::recursive_mutex> lock(initMutex);

    if(globalInitCalled)
        return 0; // nothing to do

    /* optional "log_file": append (not truncate) so multiple elbencho instances can share one
        file; falls back to stderr (the default) if unset or if opening it fails */
    std::string logFilePath = getLogFileConfig(configJsonStr);

    if(!logFilePath.empty() )
    {
        spdkLogFileHandle = fopen(logFilePath.c_str(), "a");

        if(!spdkLogFileHandle)
            ERRLOGGER(Log_NORMAL, "[SPDK] Warning: Failed to open log_file '" << logFilePath <<
                "': " << strerror(errno) << "; logging to console instead." << std::endl);
    }

    // install our log filter (see logFilterCb() ) before any spdk code can log anything
    spdk_log_open(logFilterCb);

    /* optional "log_level": raise (or lower) spdk's own log verbosity from its built-in default
        ("notice"); left untouched (i.e. unchanged default behavior) if unset */
    std::string logLevelStr = getLogLevelConfig(configJsonStr);

    if(!logLevelStr.empty() )
    {
        enum spdk_log_level logLevel;

        if(logLevelFromString(logLevelStr, logLevel) )
        {
            spdk_log_set_level(logLevel);
            spdk_log_set_print_level(logLevel);
        }
        else
            ERRLOGGER(Log_NORMAL, "[SPDK] Warning: Unknown log_level '" << logLevelStr <<
                "'; ignoring." << std::endl);
    }

    /* optional "log_flags": enable individual spdk debug log flags (e.g. "bdev_nvme", "nvme");
        none enabled by default, i.e. unchanged default behavior if unset */
    for(const std::string& logFlag : getLogFlagsConfig(configJsonStr) )
    {
        if(spdk_log_set_flag(logFlag.c_str() ) != 0)
            ERRLOGGER(Log_NORMAL, "[SPDK] Warning: Unknown log_flags entry '" << logFlag <<
                "'; ignoring." << std::endl);
    }

    /* interrupt mode must be enabled before any spdk_thread gets created below, so that all of
        them (including SpdkBdevManager's app/I/O threads, created much later) get interrupt
        facilities */
    if(isInterruptModeConfigEnabled(configJsonStr) )
        spdk_interrupt_mode_enable();

    struct spdk_env_opts opts;

    spdk_env_opts_init(&opts);

    opts.opts_size = sizeof(opts); // for ABI compatibility

    // unique env name to prevent conflicts with other processes on the same host
    std::string envName = "elb_spdk_" + std::to_string(getpid() );
    opts.name = envName.c_str();

    // memory settings to avoid hugepages
    opts.no_huge = true;
    opts.mem_size = getMemSizeMbConfig(configJsonStr); // in MiB, optional "mem_size_mb" config
    opts.base_virtaddr = 0; // avoid base_virtaddr collisions
    opts.iova_mode = NULL; // use VA mode to avoid hugepages

    // use "lcore_map" to define the allowed cores
    static std::string coreList = getCoreListFromAffinity();

    opts.core_mask = NULL;
    opts.lcore_map = coreList.c_str();

    LOGGER(Log_VERBOSE, "[SPDK] Initializing with lcore map: " << coreList << std::endl);

    /* spdk_env_init() (via dpdk's rte_eal_init() ) narrows the *calling* OS thread's own affinity
        down to a single core (its "main lcore"), which would otherwise also stick to every thread
        spawned from this one afterwards (e.g. LocalWorker threads inherit affinity from whichever
        thread creates them); snapshot it here and restore it right after the call below */
    cpu_set_t origAffinity;
    CPU_ZERO(&origAffinity);
    bool gotOrigAffinity = (sched_getaffinity(0, sizeof(origAffinity), &origAffinity) == 0);

    int initRes = spdk_env_init(&opts);

    if(initRes != 0)
        return initRes;

    if(gotOrigAffinity && (sched_setaffinity(0, sizeof(origAffinity), &origAffinity) != 0) )
        ERRLOGGER(Log_NORMAL, "[SPDK] Warning: Failed to restore original CPU affinity after "
            "spdk_env_init(): " << strerror(errno) << std::endl);

    /* spdk's tcp transport otherwise falls back to the OS-level TCP connect timeout (~2 minutes
       via SYN retries) for a path that's down at the IP level (e.g. blackholed/firewalled)
       instead of refused; that's much too slow when a multipath config includes such paths, so
       bound it to roughly the same order of magnitude as fabrics_connect_timeout_us in
       SpdkBdevManager::doAttach() */
    struct spdk_nvme_transport_opts transportOpts;
    spdk_nvme_transport_get_opts(&transportOpts, sizeof(transportOpts) );
    transportOpts.tcp_connect_timeout_ms = 10 * 1000; // 10s
    spdk_nvme_transport_set_opts(&transportOpts, sizeof(transportOpts) );

    // bring up the process-wide frameworks that bdev_nvme needs on top of the DPDK env

    int threadLibRes = spdk_thread_lib_init(NULL, 0);

    if(threadLibRes != 0)
        return threadLibRes;

    /* iobuf/accel/bdev init all call spdk_io_device_register() synchronously as part of the
       init call itself (not deferred to their async completion callback), which requires an
       spdk_thread to be bound to the calling OS thread via spdk_set_thread(); create that
       thread and bind it here first.
       Note: this also becomes spdk's global "app thread" (the first spdk_thread ever created in
       the process), which the bdev layer keeps sending messages to internally (e.g. via
       spdk_bdev_module_examine_done() ) for as long as any bdev exists - so, unlike a typical
       one-off bootstrap thread, this one must stay alive until globalUninit(), not be destroyed
       right after framework init */
    bootstrapThread = spdk_thread_create("elbencho_spdk_bootstrap", NULL);
    spdk_set_thread(bootstrapThread);

    if(spdk_interrupt_mode_is_enabled() )
        spdk_thread_set_interrupt_mode(true);

    int iobufRes = spdk_iobuf_initialize();

    if(iobufRes != 0)
        return iobufRes;

    int accelRes = spdk_accel_initialize();

    if(accelRes != 0)
        return accelRes;

    /* spdk_bdev_initialize() itself is asynchronous, but registers/inits the bdev_nvme module
        synchronously as part of the call, which is all SpdkBdevManager needs from it */
    struct
    {
        bool done{false};
        int rc{0};
    } bdevInitState;

    spdk_bdev_initialize([](void* cbArg, int rc)
    {
        auto* state = static_cast<decltype(bdevInitState)*>(cbArg);
        state->rc = rc;
        state->done = true;
    }, &bdevInitState);

    while(!bdevInitState.done)
        spdk_thread_poll(bootstrapThread, 0, 0);

    spdk_set_thread(NULL); // keep bootstrapThread itself alive, see comment above

    if(bdevInitState.rc != 0)
        return bdevInitState.rc;

    globalInitCalled = true;

    return 0;
}

/**
 * Finalize the process-wide bdev_nvme frameworks and the SPDK environment, releasing the DPDK
 * resources.
 *
 * Note: This must be called once per process after all threads have finished and before the
 * application exits.
 */
void SpdkNvmeClient::globalUninit()
{
    std::lock_guard<std::recursive_mutex> lock(initMutex);

    if(!globalInitCalled)
        return; // nothing to do if init wasn't called

    /* bdev_finish()/accel_finish()/iobuf_finish() are asynchronous and need an spdk_thread to
        both run their own steps and deliver their completion, same as spdk_bdev_initialize() above;
        reuse the same bootstrapThread from globalInit() (it is spdk's global "app thread" by now,
        see comment there), rather than creating a second one */
    spdk_set_thread(bootstrapThread);

    bool bdevDone = false;
    spdk_bdev_finish([](void* cbArg) { *static_cast<bool*>(cbArg) = true; }, &bdevDone);

    while(!bdevDone)
        spdk_thread_poll(bootstrapThread, 0, 0);

    bool accelDone = false;
    spdk_accel_finish([](void* cbArg) { *static_cast<bool*>(cbArg) = true; }, &accelDone);

    while(!accelDone)
        spdk_thread_poll(bootstrapThread, 0, 0);

    bool iobufDone = false;
    spdk_iobuf_finish([](void* cbArg) { *static_cast<bool*>(cbArg) = true; }, &iobufDone);

    while(!iobufDone)
        spdk_thread_poll(bootstrapThread, 0, 0);

    spdk_thread_exit(bootstrapThread);

    while(!spdk_thread_is_exited(bootstrapThread) )
        spdk_thread_poll(bootstrapThread, 0, 0);

    spdk_thread_destroy(bootstrapThread);
    bootstrapThread = NULL;
    spdk_set_thread(NULL);

    spdk_thread_lib_fini();

    spdk_env_fini();

    spdk_log_close();

    if(spdkLogFileHandle)
    {
        fclose(spdkLogFileHandle);
        spdkLogFileHandle = NULL;
    }

    globalInitCalled = false;
}

/**
 * Custom spdk log sink, installed via spdk_log_open() in globalInit(), to filter out one specific
 * benign-but-noisy error message without otherwise changing spdk's log output or log level
 * thresholds. Writes to spdkLogFileHandle (optional "log_file" config) instead of stderr if set.
 *
 * Background: shutting down a qpair whose socket got closed concurrently (e.g. during
 * SpdkBdevManager teardown) can make a subsequent nvme_tcp_qpair_process_completions() call try
 * to flush the already-closed socket and log "spdk_sock_flush() failed, rc -9: Bad file
 * descriptor" from nvme_tcp.c. spdk itself anticipates and tolerates this exact case, so the
 * message is just harmless log noise, not a real error.
 *
 * A second, unrelated case: connecting to a path whose destination IP hasn't been contacted
 * before on this host can lose its first TCP segment while the kernel resolves the ARP entry,
 * making spdk abort and retry that qpair's connect internally. The abort path then tries to
 * re-quiesce a qpair that's already quiescing and logs "The recv state QUIESCING is same with
 * the state to be set" from nvme_tcp.c. The retry itself still succeeds, so this is also just
 * harmless noise.
 */
void SpdkNvmeClient::logFilterCb(int level, const char* file, const int line, const char* func,
    const char* format, va_list args)
{
    char msgBuf[1024];
    vsnprintf(msgBuf, sizeof(msgBuf), format, args);

    // filter out the benign "socket already closed during qpair teardown" message
    if(file && (std::string(file) == "nvme_tcp.c") &&
        (std::string(msgBuf).find("spdk_sock_flush() failed") != std::string::npos) )
        return;

    // filter out the benign "qpair connect retry after first-contact ARP resolution" message
    if(file && (std::string(file) == "nvme_tcp.c") &&
        (std::string(msgBuf).find("QUIESCING is same with the state to be set") !=
            std::string::npos) )
        return;

    // everything else: reproduce spdk's own default formatting/thresholds (see spdk_vlog() )

    static const char* levelNames[] = {"ERROR", "WARNING", "NOTICE", "INFO", "DEBUG"};

    if( (level > spdk_log_get_print_level() ) && (level > spdk_log_get_level() ) )
        return;

    if(level <= spdk_log_get_print_level() )
    {
        std::string timestamp = getLogTimestampPrefix();
        FILE* outStream = spdkLogFileHandle ? spdkLogFileHandle : stderr;

        if(file)
            fprintf(outStream, "%s%s:%4d:%s: *%s*: %s", timestamp.c_str(), file, line, func,
                levelNames[level], msgBuf);
        else
            fprintf(outStream, "%s%s", timestamp.c_str(), msgBuf);

        if(spdkLogFileHandle)
            fflush(spdkLogFileHandle); // ensure log lines land promptly, e.g. for live tailing
    }

    if(level <= spdk_log_get_level() )
    {
        int severity = spdk_log_to_syslog_level(static_cast<enum spdk_log_level>(level) );

        if(severity >= 0)
        {
            if(file)
                syslog(severity, "%s:%4d:%s: *%s*: %s", file, line, func, levelNames[level],
                    msgBuf);
            else
                syslog(severity, "%s", msgBuf);
        }
    }
}

SpdkNvmeClient::SpdkNvmeClient()
{
}

SpdkNvmeClient::~SpdkNvmeClient()
{
    disconnect();
}

/**
 * Attach this instance to the process-wide shared SpdkBdevManager (creating/extending its
 * connection pool if this is the first instance to attach) and pick one of its dedicated I/O
 * threads for this instance's own I/O.
 *
 * @return true if at least one namespace is usable, false otherwise.
 */
bool SpdkNvmeClient::init(const std::string& configJsonStr)
{
    if(isAttached)
        return true; // already attached

    if(!SpdkBdevManager::instance().attach(configJsonStr) )
        return false;

    isAttached = true;

    ioThreadIdx = SpdkBdevManager::instance().assignIoThreadIdx();
    ioThread = SpdkBdevManager::instance().getIoThread(ioThreadIdx);

    return true;
}

/**
 * Detach this instance from the shared SpdkBdevManager. Once the last attached instance detaches,
 * every NVMe-oF connection gets dropped (see SpdkBdevManager::doDetach() ).
 */
void SpdkNvmeClient::disconnect()
{
    if(!isAttached)
        return;

    // drain this instance's own in-flight I/Os first, so none of them outlives the manager
    std::vector<IoContext*> completed;

    while(!pendingIo.empty() )
    {
        completed.clear();
        waitForCompletions(completed, -1 /*block indefinitely*/);
    }

    SpdkBdevManager::instance().detach();

    isAttached = false;
    ioThread = NULL;
}

/**
 * Get the numeric ID of a namespace from its human-friendly name.
 *
 * @return namespace ID or -1 if not found.
 */
int32_t SpdkNvmeClient::getNamespaceId(const std::string& name) const
{
    return SpdkBdevManager::instance().getNamespaceId(name);
}

/**
 * Get the human-friendly name of a namespace from its numeric ID.
 *
 * @return name string or empty string if not found.
 */
std::string SpdkNvmeClient::getNamespaceName(uint32_t nsId) const
{
    return SpdkBdevManager::instance().getNamespaceName(nsId);
}

/**
 * Get the list of all attached namespace IDs.
 */
std::vector<uint32_t> SpdkNvmeClient::getNamespaceIds() const
{
    return SpdkBdevManager::instance().getNamespaceIds();
}

/**
 * Get the size of a namespace in bytes.
 *
 * @return size in bytes or 0 if the namespace was not found.
 */
uint64_t SpdkNvmeClient::getNamespaceSize(uint32_t nsId) const
{
    auto* nsHandle = SpdkBdevManager::instance().getNamespace(nsId);

    return nsHandle ? nsHandle->sizeBytes : 0;
}

/**
 * Get the sector size of a namespace in bytes.
 *
 * @return sector size in bytes or 0 if the namespace was not found.
 */
uint32_t SpdkNvmeClient::getNamespaceSectorSize(uint32_t nsId) const
{
    auto* nsHandle = SpdkBdevManager::instance().getNamespace(nsId);

    return nsHandle ? nsHandle->sectorSize : 0;
}

/**
 * Get the real UUID of a namespace, as provided by the target.
 *
 * @return UUID string, or empty string if the namespace was not found or the target provides no
 *     real UUID (see getNamespaceNguid() for its alternative in that case).
 */
std::string SpdkNvmeClient::getNamespaceUuid(uint32_t nsId) const
{
    auto* nsHandle = SpdkBdevManager::instance().getNamespace(nsId);

    return nsHandle ? nsHandle->uuid : "";
}

/**
 * Get the NGUID of a namespace, as provided by the target.
 *
 * @return NGUID string, or empty string if the namespace was not found or the target provides no
 *     NGUID.
 */
std::string SpdkNvmeClient::getNamespaceNguid(uint32_t nsId) const
{
    auto* nsHandle = SpdkBdevManager::instance().getNamespace(nsId);

    return nsHandle ? nsHandle->nguid : "";
}

/**
 * Get the controller model name of a namespace.
 *
 * @return model string or empty string if the namespace was not found.
 */
std::string SpdkNvmeClient::getNamespaceModel(uint32_t nsId) const
{
    auto* nsHandle = SpdkBdevManager::instance().getNamespace(nsId);

    return nsHandle ? nsHandle->model : "";
}

/**
 * Dispatch one already-allocated IoContext's submission onto this instance's assigned shared I/O
 * thread and track it in pendingIo for poll() to later pick up its completion.
 */
void SpdkNvmeClient::submitOnIoThread(IoContext* ioCtx, std::function<void(IoContext*)> submitFn)
{
    pendingIo.push_back(ioCtx);

    struct MsgCtx
    {
        IoContext* ioCtx;
        std::function<void(IoContext*)> submitFn;
    };

    auto* msgCtx = new MsgCtx{ioCtx, std::move(submitFn)};

    spdk_thread_send_msg(ioThread, [](void* arg)
    {
        std::unique_ptr<MsgCtx> ctx(static_cast<MsgCtx*>(arg) );
        ctx->submitFn(ctx->ioCtx);
    }, msgCtx);
}

/**
 * Fill in and submit a caller-owned IoContext for a read.
 *
 * @return 0 on successful submission, -1 for an unknown namespace.
 */
int SpdkNvmeClient::submitRead(IoContext* ioCtx, uint32_t nsId, uint64_t lba, uint32_t lbaCount,
    void* buffer)
{
    auto* nsHandle = SpdkBdevManager::instance().getNamespace(nsId);

    if(!nsHandle)
        return -1;

    ioCtx->nsHandle = nsHandle;
    ioCtx->lba = lba;
    ioCtx->lbaCount = lbaCount;
    ioCtx->buffer = buffer;
    ioCtx->owner = this;
    ioCtx->done.store(false, std::memory_order_relaxed);

    size_t chanIdx = ioThreadIdx;

    submitOnIoThread(ioCtx, [chanIdx](IoContext* ctx)
    {
        int submitRes = spdk_bdev_read_blocks(ctx->nsHandle->desc,
            ctx->nsHandle->channels[chanIdx], ctx->buffer, ctx->lba, ctx->lbaCount, ioCompleteCb,
            ctx);

        if(submitRes != 0)
        {
            ERRLOGGER(Log_NORMAL, "[SPDK] Read submission failed. LBA: " << ctx->lba <<
                "; Error: " << spdk_strerror(-submitRes) << std::endl);

            ctx->ioSuccess = false;
            ctx->done.store(true, std::memory_order_release);

            std::lock_guard<std::mutex> lock(ctx->owner->completionMutex);
            ctx->owner->completionCv.notify_one();
        }
    });

    return 0;
}

/**
 * Fill in and submit a caller-owned IoContext for a write.
 *
 * @return 0 on successful submission, -1 for an unknown namespace.
 */
int SpdkNvmeClient::submitWrite(IoContext* ioCtx, uint32_t nsId, uint64_t lba, uint32_t lbaCount,
    void* buffer)
{
    auto* nsHandle = SpdkBdevManager::instance().getNamespace(nsId);

    if(!nsHandle)
        return -1;

    ioCtx->nsHandle = nsHandle;
    ioCtx->lba = lba;
    ioCtx->lbaCount = lbaCount;
    ioCtx->buffer = buffer;
    ioCtx->owner = this;
    ioCtx->done.store(false, std::memory_order_relaxed);

    size_t chanIdx = ioThreadIdx;

    submitOnIoThread(ioCtx, [chanIdx](IoContext* ctx)
    {
        int submitRes = spdk_bdev_write_blocks(ctx->nsHandle->desc,
            ctx->nsHandle->channels[chanIdx], ctx->buffer, ctx->lba, ctx->lbaCount, ioCompleteCb,
            ctx);

        if(submitRes != 0)
        {
            ERRLOGGER(Log_NORMAL, "[SPDK] Write submission failed. LBA: " << ctx->lba <<
                "; Error: " << spdk_strerror(-submitRes) << std::endl);

            ctx->ioSuccess = false;
            ctx->done.store(true, std::memory_order_release);

            std::lock_guard<std::mutex> lock(ctx->owner->completionMutex);
            ctx->owner->completionCv.notify_one();
        }
    });

    return 0;
}

/**
 * Fill in and submit a caller-owned IoContext for a trim (unmap).
 *
 * @return 0 on successful submission, -1 for an unknown namespace.
 */
int SpdkNvmeClient::submitTrim(IoContext* ioCtx, uint32_t nsId, uint64_t lba, uint32_t lbaCount)
{
    auto* nsHandle = SpdkBdevManager::instance().getNamespace(nsId);

    if(!nsHandle)
        return -1;

    ioCtx->nsHandle = nsHandle;
    ioCtx->lba = lba;
    ioCtx->lbaCount = lbaCount;
    ioCtx->buffer = NULL;
    ioCtx->owner = this;
    ioCtx->done.store(false, std::memory_order_relaxed);

    size_t chanIdx = ioThreadIdx;

    submitOnIoThread(ioCtx, [chanIdx](IoContext* ctx)
    {
        int submitRes = spdk_bdev_unmap(ctx->nsHandle->desc, ctx->nsHandle->channels[chanIdx],
            ctx->lba, ctx->lbaCount, ioCompleteCb, ctx);

        if(submitRes != 0)
        {
            ERRLOGGER(Log_NORMAL, "[SPDK] Trim submission failed. LBA: " << ctx->lba <<
                "; Error: " << spdk_strerror(-submitRes) << std::endl);

            ctx->ioSuccess = false;
            ctx->done.store(true, std::memory_order_release);

            std::lock_guard<std::mutex> lock(ctx->owner->completionMutex);
            ctx->owner->completionCv.notify_one();
        }
    });

    return 0;
}

/**
 * Submit an asynchronous read into a caller-preallocated IoContext (e.g. one slot of a pool sized
 * by ioDepth). The caller (not this class) owns ioCtx's lifetime and is responsible for picking
 * up the completion (e.g. via waitForCompletions() ); only ioCtx->userData is left untouched
 * here, so the caller can use it (e.g. for a pool slot index) across resubmissions of the same
 * context.
 *
 * @return 0 on successful submission, -1 for an unknown namespace.
 */
int SpdkNvmeClient::read(IoContext* ioCtx, uint32_t nsId, uint64_t lba, uint32_t lbaCount,
    void* buffer)
{
    return submitRead(ioCtx, nsId, lba, lbaCount, buffer);
}

/**
 * Submit an asynchronous write into a caller-preallocated IoContext. See read(IoContext*, ...)
 * above for details.
 *
 * @return 0 on successful submission, -1 for an unknown namespace.
 */
int SpdkNvmeClient::write(IoContext* ioCtx, uint32_t nsId, uint64_t lba, uint32_t lbaCount,
    void* buffer)
{
    return submitWrite(ioCtx, nsId, lba, lbaCount, buffer);
}

/**
 * Submit an asynchronous trim (unmap) into a caller-preallocated IoContext. See
 * read(IoContext*, ...) above for details.
 *
 * @return 0 on successful submission, -1 for an unknown namespace.
 */
int SpdkNvmeClient::trim(IoContext* ioCtx, uint32_t nsId, uint64_t lba, uint32_t lbaCount)
{
    return submitTrim(ioCtx, nsId, lba, lbaCount);
}

/**
 * @return true if at least one entry in pendingIo is done (used as the wait predicate by
 *     waitForCompletions() ).
 */
bool SpdkNvmeClient::anyPendingIoDone() const
{
    for(const IoContext* ioCtx : pendingIo)
        if(ioCtx->done.load(std::memory_order_acquire) )
            return true;

    return false;
}

/**
 * Check this instance's own in-flight I/Os for completions and run the corresponding user
 * callbacks. The actual qpair/channel polling happens continuously in the background on
 * SpdkBdevManager's dedicated I/O threads, independent of how often (or whether) this gets
 * called; this just picks up results that are already there. Never blocks (see
 * waitForCompletions() for a blocking, non-busy-polling alternative).
 *
 * @return number of completions processed.
 */
int SpdkNvmeClient::poll()
{
    std::vector<IoContext*> completed; // unused by callers of poll(), only for waitForCompletions()

    return waitForCompletions(completed, 0);
}

/**
 * Block (without busy-polling) until at least one of this instance's in-flight I/Os completes, or
 * timeoutMs elapses, then drain every currently-done entry into outCompleted (out-of-order safe:
 * order of outCompleted does not necessarily match submission order). Drained entries remain
 * owned by the caller, as all IoContexts are caller-preallocated (see read(IoContext*, ...) ).
 *
 * @param timeoutMs how long to block for at most; 0 to just check without blocking (like the
 *     traditional poll() ), negative to block indefinitely until a completion arrives.
 * @return number of completions processed (0 on timeout with nothing ready).
 */
int SpdkNvmeClient::waitForCompletions(std::vector<IoContext*>& outCompleted, int timeoutMs)
{
    std::unique_lock<std::mutex> lock(completionMutex);

    if(timeoutMs < 0)
        completionCv.wait(lock, [this] { return anyPendingIoDone(); } );
    else if(timeoutMs > 0)
        completionCv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
            [this] { return anyPendingIoDone(); } );

    lock.unlock(); // pendingIo itself is only ever touched by this instance's own thread

    int numCompleted = 0;

    for(size_t i = 0; i < pendingIo.size(); /* no incr here, see below */)
    {
        IoContext* ioCtx = pendingIo[i];

        if(!ioCtx->done.load(std::memory_order_acquire) )
        {
            i++;
            continue;
        }

        pendingIo.erase(pendingIo.begin() + i); // don't increment i, next element shifted here

        outCompleted.push_back(ioCtx);

        numCompleted++;
    }

    return numCompleted;
}

/**
 * Allocate zeroed DMA-capable (pinned) memory for use as I/O buffer.
 *
 * @return pointer to the new buffer or NULL on error.
 */
void* SpdkNvmeClient::allocDmaBuf(size_t size)
{
    return spdk_dma_zmalloc(size, 0, NULL);
}

/**
 * Free DMA-capable memory that was allocated through allocDmaBuf().
 */
void SpdkNvmeClient::freeDmaBuf(void* buf)
{
    spdk_dma_free(buf);
}

/**
 * SPDK callback after a bdev I/O command completed. This logs an error message for failed I/Os
 * and marks the IoContext done, so that this instance's own poll() picks it up and runs the
 * user-provided completion callback. Runs on the shared I/O thread that the I/O was submitted on.
 */
void SpdkNvmeClient::ioCompleteCb(struct spdk_bdev_io* bdevIo, bool success, void* cbArg)
{
    IoContext* ioCtx = static_cast<IoContext*>(cbArg);

    if(!success)
    {
        uint32_t cdw0;
        int sct, sc;
        spdk_bdev_io_get_nvme_status(bdevIo, &cdw0, &sct, &sc);

        ERRLOGGER(Log_NORMAL, "[SPDK] I/O failed!" << std::endl <<
            "       Namespace: " << ioCtx->nsHandle->fullName <<
                " (ID " << ioCtx->nsHandle->id << ")" << std::endl <<
            "       LBA:       " << ioCtx->lba <<
                " (Count: " << ioCtx->lbaCount << ")" << std::endl <<
            "       Error:     " << getNvmeErrorString(sct, sc) << std::endl <<
            "       Raw Code:  SCT=" << sct << " SC=" << sc << std::endl);
    }

    spdk_bdev_free_io(bdevIo);

    ioCtx->ioSuccess = success;
    ioCtx->done.store(true, std::memory_order_release);

    // wake up the owning instance's thread if it's blocked in waitForCompletions()
    std::lock_guard<std::mutex> lock(ioCtx->owner->completionMutex);
    ioCtx->owner->completionCv.notify_one();
}

/**
 * Translate an NVMe status code type and status code to a human-readable string.
 */
std::string SpdkNvmeClient::getNvmeErrorString(uint8_t sct, uint8_t sc)
{
    switch(sct)
    {
        case SPDK_NVME_SCT_GENERIC:
            switch(sc)
            {
                case SPDK_NVME_SC_SUCCESS: return "Success";
                case SPDK_NVME_SC_INVALID_OPCODE: return "Invalid Opcode";
                case SPDK_NVME_SC_INVALID_FIELD: return "Invalid Field";
                case SPDK_NVME_SC_COMMAND_ID_CONFLICT: return "Command ID Conflict";
                case SPDK_NVME_SC_DATA_TRANSFER_ERROR: return "Data Transfer Error";
                case SPDK_NVME_SC_ABORTED_POWER_LOSS: return "Aborted - Power Loss";
                case SPDK_NVME_SC_INTERNAL_DEVICE_ERROR: return "Internal Device Error";
                case SPDK_NVME_SC_ABORTED_BY_REQUEST: return "Aborted by Request";
                case SPDK_NVME_SC_ABORTED_SQ_DELETION: return "Aborted - SQ Deletion";
                case SPDK_NVME_SC_ABORTED_FAILED_FUSED: return "Aborted - Failed Fused";
                case SPDK_NVME_SC_ABORTED_MISSING_FUSED: return "Aborted - Missing Fused";
                case SPDK_NVME_SC_INVALID_NAMESPACE_OR_FORMAT: return "Invalid Namespace/Format";
                case SPDK_NVME_SC_LBA_OUT_OF_RANGE: return "LBA Out of Range";
                case SPDK_NVME_SC_CAPACITY_EXCEEDED: return "Capacity Exceeded";
                case SPDK_NVME_SC_NAMESPACE_NOT_READY: return "Namespace Not Ready";
            } break;

        case SPDK_NVME_SCT_COMMAND_SPECIFIC:
            switch(sc)
            {
                case SPDK_NVME_SC_COMPLETION_QUEUE_INVALID: return "Completion Queue Invalid";
            } break;

        case SPDK_NVME_SCT_MEDIA_ERROR:
            switch(sc)
            {
                case SPDK_NVME_SC_WRITE_FAULTS: return "Write Faults";
                case SPDK_NVME_SC_UNRECOVERED_READ_ERROR: return "Unrecovered Read Error";
                case SPDK_NVME_SC_GUARD_CHECK_ERROR: return "End-to-End Guard Check Error";
                case SPDK_NVME_SC_APPLICATION_TAG_CHECK_ERROR: return "Application Tag Check Error";
                case SPDK_NVME_SC_REFERENCE_TAG_CHECK_ERROR: return "Reference Tag Check Error";
                case SPDK_NVME_SC_COMPARE_FAILURE: return "Compare Failure";
                case SPDK_NVME_SC_ACCESS_DENIED: return "Access Denied";
            } break;
    }

    std::stringstream stream;
    stream << "Unknown Error (SCT: 0x" << std::hex << (int)sct << "; SC: 0x" << (int)sc << ")";

    return stream.str();
}

#endif // SPDK_SUPPORT
