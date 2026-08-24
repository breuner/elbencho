# **NVMe-oF Block Device Benchmarking via SPDK**

elbencho can benchmark remote NVMe-oF block devices (over TCP or RDMA) through [SPDK](https://spdk.io/), the Storage Performance Development Kit. SPDK provides a user-space NVMe-oF initiator, which enables elbencho to talk directly to remote NVMe-oF targets. SPDK uses active polling for I/O request completions, which results in a corresponding increase in CPU usage. Using elbencho with SPDK is an alternative to using elbencho with the NVMe-oF stack of the Linux kernel, which gets configured via the `nvme` CLI tool to create regular `/dev/nvme...` entries.

## **Build elbencho with SPDK Support**

SPDK support is not enabled by default, but can easily be enabled via the `SPDK_SUPPORT=1` make option:

```bash
git clone https://github.com/breuner/elbencho.git
cd elbencho
make SPDK_SUPPORT=1 -j $(nproc)
```

elbencho automatically downloads and builds a copy of [SPDK's official GitHub repo](https://github.com/spdk/spdk) as part of this build, so no separate SPDK installation is required.

SPDK itself has a number of build dependencies (e.g. certain dev packages). If the initial SPDK build fails because of missing dependencies, run SPDK's own dependency installer script with root privileges, then clean up and retry the elbencho build:

```bash
sudo ./external/spdk/scripts/pkgdep.sh
make clean-all
make SPDK_SUPPORT=1 -j $(nproc)
```

*Note*: As an alternative to building elbencho with SPDK support, consider using an elbencho container from [Docker Hub](https://hub.docker.com/r/breuner/elbencho) with SPDK NVMe-oF support included.

## **Quick Start**

SPDK mode is selected via `--spdkconf` (inline JSON string) or `--spdkconffile` (path to a JSON config file), instead of giving normal file/block device paths.

If you call elbencho in SPDK mode without any paths, it will only run discovery/connect and print the result on the console, then exit:

```bash
elbencho --spdkconffile myconfig.json
```

```
SPDK namespace discovery result...
Namespace: fast_db:nvme0:n1; ID: 0; Size: 100GiB (107374182400B); Format: 512B; Model: Example NVMe Ctrl; UUID: 4c4c4544-005a-3810-8059-c3c04f334434
Namespace: fast_db:nvme0:n2; ID: 1; Size: 200GiB (214748364800B); Format: 512B; Model: Example NVMe Ctrl; NGUID: c624d5fcdfd247c08c8726965459ad5c
```

Once you know which namespace(s) you want to benchmark, add them as path arguments, just like you would add local block device paths for a normal elbencho run:

```bash
elbencho -r -b 4k -t 16 --iodepth 16 --rand --spdkconffile myconfig.json 4c4c4544-005a-3810-8059-c3c04f334434
```

Namespaces can be selected by the numeric `ID`, the human-friendly `Namespace` name, `UUID` or `NGUID` shown in the discovery result above.

The `Namespace` name is built as `<subsystem-name>:<controller-name>:n<suffix>`. `<subsystem-name>` is the subsystem entry's `name` if given, otherwise it defaults to the subsystem NQN, shortened to the part after its last `:` (e.g. `nqn.2014-08.org.nvmexpress:uuid:1234` becomes `1234`). `<controller-name>` is `controller_name` if given, otherwise a per-config `ctrlrN` counter.

Regular expressions can be used to select based on namespaces names, e.g. `elbencho "mysubsys:.*"` to select every namespace of the given subsystem for a benchmark run.

*Note*: UUID or NGUID are the recommended way to select namespaces, because the numeric ID can change depending on the order of discovery results (e.g. across different targets, subsystem paths becoming available in a different order, etc.), while UUID/NGUID uniquely and stably identify a namespace.

## **Minimal Config File Example**

This is the minimum SPDK JSON config to connect to a single subsystem via TCP:

[spdk/spdk-config-minimal-example.json](spdk/spdk-config-minimal-example.json)

```json
{
    "subsystems": [
        {
            "traddr": "192.168.1.100",
            "trsvcid": "4420",
            "trtype": "tcp"
        }
    ]
}
```

If `host_nqn` is omitted, elbencho tries to read the host NQN from `/etc/nvme/hostnqn`.

If `nqn` is omitted for a subsystem entry, elbencho queries the discovery service at the given `traddr`/`trsvcid` and probes every subsystem & path that it reports.

## **Full Config File Example**

This example shows all available config options, including multipathing, CPU core pinning for SPDK's internal threads and discovery-based subsystem resolution:

[spdk/spdk-config-full-example.json](spdk/spdk-config-full-example.json)

```json
{
    "// host_nqn": "optional, host NQN to use for connect/discovery. Default: unset, i.e. read from '/etc/nvme/hostnqn' if that file exists, otherwise use spdk's own generated default.",
    "host_nqn": "nqn.2014-08.org.nvmexpress:uuid:elbencho",

    "// io_threads": "optional, number of dedicated SPDK I/O threads shared by every elbencho worker thread that attaches this config (default: 1). NVMe-oF connections scale with io_threads * usable_paths, NOT with the number of elbencho worker threads, since all of them submit I/O through this small, fixed-size thread pool.",
    "io_threads": 1,

    "// multipath_policy": "optional, applies to every subsystem below. 'roundrobin' (default): spread I/O across all healthy paths. 'failover': stick with one path per namespace, switch only if it fails.",
    "multipath_policy": "roundrobin",

    "// cpu_cores": "optional, list of CPU core numbers for the SPDK app/I/O reactor threads (round-robin assigned, one core per thread). Only affects those dedicated SPDK reactor threads, never elbencho worker threads. Can be combined with cpu_core_range (merged, de-duplicated). Default: unset, i.e. use every CPU core available to this process, so multiple io_threads spread across different cores by default.",
    "cpu_cores": [0, 2, 4, 6],

    "// cpu_core_range": "optional, 'start-end' range of CPU core numbers, expanded and merged with cpu_cores above. Example: cores 0 to 7 inclusive.",
    "cpu_core_range": "0-7",

    "// interrupt_mode": "optional, enables SPDK interrupt mode (epoll-based blocking wait) for the app/I/O reactor threads instead of busy-polling. Default: false. CAVEAT: spdk's bdev_nvme module refuses to create any bdev for a non-PCIe transport while this is enabled, so it only works if every subsystem below is reached via PCIe (never true for NVMe-oF TCP/RDMA, which is elbencho's normal use case) - leave this disabled unless you know it applies to your setup.",
    "interrupt_mode": false,

    "// mem_size_mb": "optional, size in MiB of the process-wide DMA memory pool shared by elbencho's own I/O buffers and spdk's internal pools. Default: 8192. Raise this if DMA buffer allocation fails for a high worker thread count / iodepth / block size combination.",
    "mem_size_mb": 8192,

    "// log_file": "optional, path to append spdk's own log messages to instead of printing them to the console (appended, not truncated, so multiple elbencho instances can share one file). Default: unset, i.e. console. Does not affect elbencho's own '[SPDK] ...' console messages.",
    "log_file": "/var/log/elbencho_spdk.log",

    "// log_level": "optional, spdk's own log level threshold (both for logging and for printing): 'disabled', 'error', 'warn', 'notice' (spdk's own default), 'info' or 'debug'. Default: unset, i.e. leave spdk's built-in default ('notice') untouched. Raise this to 'debug' together with 'log_flags' below when diagnosing e.g. why an expected namespace doesn't show up. Does not affect elbencho's own '--log'/'-v' verbosity.",
    "log_level": "notice",

    "// log_flags": "optional, list of individual spdk debug log flag names to enable (e.g. 'bdev_nvme', 'nvme', 'nvme_rdma'; run 'strings $(which elbencho) | grep -A2 SPDK_LOG_' or check spdk's own SPDK_LOG_REGISTER_COMPONENT() call sites for available names). Default: unset, i.e. no flags enabled. Only takes effect together with 'log_level': 'debug' or 'info' above, since these flags gate spdk's SPDK_DEBUGLOG()/SPDK_INFOLOG() calls, which are themselves still subject to the level threshold. Note: messages behind SPDK_DEBUGLOG() only appear at all if spdk itself was built with debug logging enabled (spdk's own './configure --enable-debug', not the default for elbencho's bundled spdk build) - SPDK_INFOLOG() messages are unaffected by that build option.",
    "log_flags": ["bdev_nvme", "nvme"],

    "// filter": "opional, filter discovery results based on controller model name.",
    "filter": {
        "models": ["Example NVMe Ctrl"]
    },

    "subsystems": [
        {
            "// comment": "explicit subsystem with a single address",
            "name": "fast_db",
            "controller_name": "primary",
            "traddr": "192.168.1.100",
            "trsvcid": "4420",
            "trtype": "tcp",
            "nqn": "nqn.2016-06.io.spdk:cnode1"
        },
        {
            "// comment": "explicit subsystem with multiple explicit addresses (list) plus a contiguous IPv4 range; both are merged and de-duplicated. also opts into any additional paths for the same 'nqn' that the target's discovery service reports.",
            "name": "multipath_db",
            "controller_name": "primary",
            "traddr": ["10.0.0.11", "10.0.0.12"],
            "traddr_range": "10.0.1.11-10.0.1.14",
            "trsvcid": "4420",
            "trtype": "tcp",
            "nqn": "nqn.2016-06.io.spdk:cnode2",
            "use_discovered_paths": true
        },
        {
            "// comment": "discovery mode: 'nqn' is omitted, so elbencho queries the discovery service at 'traddr' (using 'host_nqn' above, unlike a plain nvme-cli/spdk discovery connect) and probes every subsystem/path it reports. 'name' is also omitted here, so each discovered subsystem gets its own name derived from its own NQN (see 'Quick Start' above), rather than a shared name with a disambiguating suffix.",
            "traddr": "10.0.0.20",
            "trsvcid": "4420",
            "trtype": "tcp"
        }
    ]
}
```

