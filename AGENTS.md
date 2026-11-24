# AGENTS.md

Guidance for AI coding agents (and human contributors) working on this repository. It describes
how elbencho is built and tested, how the code is organized, the conventions that changes have to
follow, and what a pull request on GitHub must contain. Read it before changing anything.

## What this is

elbencho is a distributed storage benchmark tool (C++17) for files, objects (S3) & block devices,
with optional GPU (CUDA/GDS) support. It replaces tools like fio/mdtest/ior with a single unified
benchmark that can run locally or across many hosts in a coordinator/service topology.

## Build

```bash
make -j $(nproc)                 # standard build, output at bin/elbencho
make help                        # list all optional build features & targets
make clean-all                   # also removes the downloaded externals (changing a build feature alone needs no clean, see below)
make deb                         # build a .deb package (via prepare-buildroot)
make rpm                         # build an .rpm package
```

Optional build-time features are enabled via `make <FEATURE>=1 ...`, e.g.:
- `S3_SUPPORT=1` — S3 object storage support (downloads/builds AWS SDK CPP unless `AWS_LIB_DIR`/`AWS_INCLUDE_DIR` point to a prebuilt SDK). `S3_AWSCRT=1` switches the S3 backend to AWS Common Runtime.
- `SPDK_SUPPORT=1` — NVMe-oF initiator support via SPDK (downloads a large external repo); see [docs/spdk.md](docs/spdk.md).
- Netbench mode is always built, with four engines: tcp (plain sockets, no dependency, always built), ibverbs (`IBVERBS_SUPPORT`, auto-detected libibverbs with `ibv_query_gid_ex`, Linux only), rdmacm (`RDMACM_SUPPORT`, auto-detected librdmacm, needs ibverbs) and libfabric (`LIBFABRIC_SUPPORT=1`, off by default; uses a system libfabric (pkg-config, min 1.11) or builds v2.7.0 into `external/` when none is found or `BUILD_LIBFABRIC=1` is given; of the build helpers only the Ubuntu 26.04 and CUDA Dockerfiles enable it, together with the ibverbs and rdmacm engines explicitly; static executables have the tcp engine only); see [docs/netbench.md](docs/netbench.md).
- `CUDA_SUPPORT` / `CUFILE_SUPPORT` — auto-detected from `/usr/local/cuda*`, can be forced 0/1.
- `HDFS_SUPPORT=1` — Hadoop HDFS support (requires `HADOOP_HOME`/`JAVA_HOME`).
- `MPI_SUPPORT=1` — the MPI launch (`--mpilaunch`: rank 0 coordinator, other ranks services, any mode) and
  the MPI-IO benchmark mode (`--mpiio`, `--mpicoll`); compiles and links through the MPI compiler wrapper
  (`MPICXX`, default `mpicxx`) unless `CXX` is given; see [docs/mpi.md](docs/mpi.md).
- `BUILD_STATIC=1` — static binary (tested on Alpine); `USE_MIMALLOC=1` — use mimalloc allocator.
- `ALTHTTPSVC_SUPPORT=1` — build the alternative HTTP service backend (see Architecture below).
- `CYGWIN_SUPPORT=1` — Windows build via Cygwin (auto-detected there); see [docs/windows.md](docs/windows.md).

External dependencies fetched by the build (AWS SDK, SPDK, mimalloc, ftxui, etc.) are pulled by
`external/prepare-external.sh` into `external/`; patches for vendored deps live in `external/patches/`.

The resolved feature configuration is written to `bin/.build-features` (only when it changes), and
the objects and the externals preparation depend on that file: switching a feature rebuilds what
depends on it, a build with unchanged features does nothing, and `make install`, `make deb` and
`make rpm` take an existing executable as it is (they build only when it is missing), so they need
none of the feature flags. `make clean-all` is only needed to build the downloaded externals
differently (e.g. after changing `BUILD_STATIC`); the AWS SDK is the exception, as
`prepare-external.sh` records tag and `S3_AWSCRT` of the installed SDK in
`external/aws-sdk-cpp_install/.source` and re-clones/rebuilds it by itself when they change (e.g.
for a plugin's SDK fork).

Docker build files live in `build_helpers/docker/`; only the `.local` variants (building from a local
clone rather than pulling from GitHub) are kept — non-`.local` variants were intentionally removed.

### Portability: the code has to compile on macOS and Windows (Cygwin) too

elbencho is built on Linux, on macOS and on Windows via Cygwin, so C++17 changes must compile on
all of them. It is fine for a *feature* to be unavailable on a platform that cannot support it —
guard it with `#if defined(__APPLE__)` / `#if !defined(__APPLE__)` (or `#ifdef CYGWIN_SUPPORT`)
and, where a user could ask for it explicitly, fail with a clear message saying so. Existing
examples: `posix_fallocate()` does not exist on macOS (`--preallocfile` throws, the write journal
just skips preallocation), and there is no `O_DIRECT` (`--direct` is rejected). The `Darwin`
section of the `Makefile` additionally force-disables `COREBIND_SUPPORT`, `LIBAIO_SUPPORT`,
`LIBNUMA_SUPPORT`, `SYNCFS_SUPPORT` and `THREADNAME_SUPPORT`, so anything behind those defines is
Linux-only anyway. The Cygwin build disables a similar set, and the options that need a Linux
kernel interface are rejected there; [docs/windows.md](docs/windows.md) lists them.

The macOS toolchain is **Apple clang with libc++** (`g++` is clang there, targeting `arm64-apple-darwin`,
from the Command Line Tools), which rejects a few things gcc/libstdc++ accepts. The two that keep
coming up:

- **`size_t` and `uint64_t` are distinct types** on macOS (`unsigned long` vs. `unsigned long long`),
  while they are the same type on x86-64 Linux. So `std::min(someUint64, someSizeT)` compiles on
  Linux but fails template deduction on macOS. Give such calls an explicit type
  (`std::min<uint64_t>(...)`) or cast an argument.
- **libc++ pulls in far fewer headers transitively** than libstdc++, so include what you use —
  `<algorithm>` for `std::min`/`std::max`, `<iomanip>` for `std::setprecision`, `<cstring>` for
  `memcpy`/`strerror`, and so on.

Neither of these is reproducible from a Linux box (a `-m32` build or libc++ would catch the first,
but neither is usually installed), so they need care by review rather than by compiling. The
Windows build is covered by the release workflows (see "Pull requests on GitHub" below), the macOS
build is not covered by any automation and remains a requirement nevertheless.

## Tests

There is no unit test suite. Automated testing is black-box: the tests execute the `elbencho`
binary and verify its results. This is the current way to add and run tests.

```bash
tests/run-tests.sh              # posix & distributed mode tests (the default set)
tests/run-tests.sh -a -j 4      # all groups incl. S3, 4 test scripts in parallel
tests/run-tests.sh -v tests/tests_posix/smallfiles-dirmode.t   # single test, verbose
tests/run-tests.sh -c tests/tests_posix/smallfiles-dirmode.t   # log the executed commands
tests/run-tests.sh -h           # all options (-e binary, -T tmpdir, -k keep failed, -s S3, -p SPDK)
```

[tests/run-tests.sh](tests/run-tests.sh) is a bash wrapper around the `prove` TAP harness; the tests
themselves are bash scripts named `*.t`, grouped in `tests/tests_posix/`, `tests/tests_distributed/`,
`tests/tests_netbench/` (`--netbench` between local service instances),
`tests/tests_mpi/` (the MPI launch and `--mpiio`, standalone and under a local `mpirun`; skips without
`MPI_SUPPORT=1` or without an `mpirun` in the PATH; helpers in `tests/lib/mpi.sh`),
`tests/tests_s3/` (libreFS, a minio fork, downloaded on demand), `tests/tests_spdk/` (each script starts its own
private `nvmf_tgt`, built with elbencho when `SPDK_SUPPORT=1`; needs python3 for spdk's `rpc.py`) and
`tests/tests_plugin_s3rdma/` (`-r`: the S3-over-RDMA plugins against the test server in `tools/s3rdma/`,
through NVIDIA's cuObject client library where the NIC supports it, else through the cuObject client
shim in `tools/s3rdma/cuobjclient-shim`; `make -C tools/s3rdma` builds these tools, which are separate
little projects; see the "S3-over-RDMA plugin tests" section of `tests/README.md`).
Dirs holding test cases carry the `tests_` prefix; `lib/`, `tmp/` and `tools/` hold supporting files -
`tests/tools/` has ad-hoc servers for interactive use (an NVMe-oF target and an S3 server that
stay up until ctrl+c and keep their dataset). Shared helpers live in
[tests/lib/testlib.sh](tests/lib/testlib.sh) (TAP output, assertions, per-test temp dir, watchdog
timeouts, free port selection, command tracing for `-c`, `--jsonfile`/`--opslog`/`--livecsv`
inspection),
[tests/lib/minio.sh](tests/lib/minio.sh) and [tests/lib/spdk.sh](tests/lib/spdk.sh).
**[tests/README.md](tests/README.md) is the reference for
how to write a test** — read it before adding one; it documents the required structure and the
non-obvious details of verifying elbencho result files.

Do not add tests on your own initiative. When a change would be worth covering, *suggest* a test
where it is genuinely relevant and only write it when the user explicitly asks for it. Same rule for
extending existing test scripts. (Running the existing tests to verify a change is a different
matter and is expected, see "Pull requests on GitHub" below.)

The older [tools/test-examples.sh](tools/test-examples.sh) still exists as a manual smoke test of
documented example commands against a given directory (and loopback block devices; requires root).
It only checks exit codes. Everything it exercises is now also covered by the tests above, see the
coverage map in [tests/README.md](tests/README.md), so there is no need to add cases from it again.
It supports `-b` (skip block device tests), `-d` (skip distributed/service-mode tests), `-m` (skip
multi-file tests):

```bash
sudo tools/test-examples.sh /path/to/basedir
```

After building, `bin/elbencho --help` and `bin/elbencho --version` are the fastest way to check that a
change behaves as expected and which optional features are compiled in.

## Architecture

**Entry point & flow**: [source/Main.cpp](source/Main.cpp) parses args into `ProgArgs`
([source/ProgArgs.cpp](source/ProgArgs.cpp), by far the largest source file — all CLI options, config
file parsing, and validation live here) and hands off to `Coordinator::main()`
([source/Coordinator.cpp](source/Coordinator.cpp)). The Coordinator drives the overall benchmark
lifecycle: waiting for a user-defined start time, waiting for remote services to be ready, running each
benchmark phase (mkdir/mkfile/write/read/rmfile/rmdir/etc.), sync/drop-caches between phases, and
aggregating final `Statistics`.

**Local vs. distributed execution**: elbencho can run standalone or in a distributed "service" mode
(`--hosts` + `--service`). In distributed mode, one elbencho process is the coordinator/client and
others run as services; the coordinator's `RemoteWorker` ([source/workers/RemoteWorker.cpp](source/workers/RemoteWorker.cpp))
talks to remote elbencho service instances over HTTP (see `HTTPService*` below), while `LocalWorker`
([source/workers/LocalWorker.cpp](source/workers/LocalWorker.cpp), the largest worker file — contains
the actual I/O engines) does the real local file/object/block I/O. Both derive from the common `Worker`
base ([source/workers/Worker.cpp](source/workers/Worker.cpp)), and `WorkerManager`
([source/workers/WorkerManager.cpp](source/workers/WorkerManager.cpp)) owns the thread pool of workers
and coordinates phase transitions across them.

**HTTP service layer**: the REST API used for distributed coordination and live stats has two
interchangeable backends behind a shared interface (`HTTPService.h`): `HTTPServiceSWS` (Simple-Web-Server)
is the default and always built, `HTTPServiceUWS` (uWebSockets) is an alternative that is only compiled
in with `ALTHTTPSVC_SUPPORT=1` and then selected at runtime by the service option `--althttpsvc`. Both
print `... now listening. Port: N` at start (`--port 0` gives an OS-chosen port).
If the wire protocol changes between coordinator and services, suggest to update the `HTTP_PROTOCOLVERSION`
in file `source/Common.h`, but do not update it unless explicitly requested.

**S3 bench mode & plugins**: The S3 mode is separated from the core in `source/modes/s3/`:
`S3Mode` holds the worker-side S3 code (a friend of `LocalWorker` that works directly on its
buffers and stats, dispatched per phase from `LocalWorker::run()`), `S3ProgArgs` the S3 options
(owned by `ProgArgs` as `s3Args`), `S3UploadStore` the shared multipart upload state, `toolkits/`
the S3 helpers (SDK init and client creation, credentials, ACLs, the SDK log and retry adapters)
and `S3Transport.h` the compile-time seam for how object data moves (`S3DefaultTransport`
= HTTP body; a plugin can replace it via the `ELB_S3_TRANSPORT_HEADER` define, no virtual calls in
the I/O path). The design notes are in [source/modes/s3/AGENTS.md](source/modes/s3/AGENTS.md).
The remaining bench modes (POSIX, HDFS, SPDK) still live in `LocalWorker`/`ProgArgs` and can follow
the same pattern. Contributed plugins live in `contrib/plugins/<name>/` (own `plugin.mk`, sources,
README), implement the `Plugin` interface from `source/plugins/` (options, checks, service transfer
of their options, all outside the I/O path), get compiled in via `ELB_PLUGIN_<NAME>=1` and
activated at runtime via `--plugins`. Plugin options are deliberately not added to the bash
completion file and plugin docs stay in the plugin dir; see
[contrib/plugins/README.md](contrib/plugins/README.md).

**Netbench mode**: `source/modes/netbench/` follows the same pattern: `NetBenchProgArgs` (options,
checks, `--help-netbench`, service transfer; owned by `ProgArgs` as `netBenchArgs`) and
`NetBenchMode` (worker-side code, a friend member of `LocalWorker`). The data path is an engine
behind the abstract `NetBenchEndpoint`, selected per run via `--netengine`: tcp (always built),
ibverbs, rdmacm and libfabric (under their `*_SUPPORT` defines). Before a run the coordinator
asks every service for its interfaces over HTTP and `NetBenchPlacement` assigns every client
stream to a server interface in the same subnet; completion waiting is event-based, not busy
polling. The design notes are in [source/modes/netbench/AGENTS.md](source/modes/netbench/AGENTS.md),
the user-level description in [docs/netbench.md](docs/netbench.md).

**MPI-IO mode**: `source/modes/mpiio/` follows the same pattern again: `MpiIoProgArgs` (options,
checks, `--help-mpiio`, service transfer; owned by `ProgArgs` as `mpiIoArgs`) and `MpiIoMode`
(worker-side, a friend member of `LocalWorker`). Only the file data path goes through MPI, behind
`LocalWorker`'s positional read/write function pointers, with the independent or collective call
chosen once per phase; the POSIX code keeps doing the file preparation, the dir/stat/delete
phases and the offset generation. `MpiLauncher` (`toolkits/mpi/`, mode-independent) maps MPI
ranks to the coordinator and service roles for `--mpilaunch`. The design notes are in
[source/modes/mpiio/AGENTS.md](source/modes/mpiio/AGENTS.md), the user-level description in
[docs/mpi.md](docs/mpi.md).

**Storage backends**: I/O paths dispatch by benchmark target type — local/POSIX files & block devices,
S3 objects (`modes/s3/`, see above), Hadoop HDFS, and NVMe-oF
block devices via SPDK (`toolkits/spdk/`: `SpdkBdevManager`, `SpdkNvmeClient`, `SpdkTk` — see
[docs/spdk.md](docs/spdk.md) for the underlying design). GPU-memory I/O (CUDA / GPUDirect Storage) is
integrated at the `LocalWorker` level via `CuFileHandleData.h`.

**Supporting toolkits** (`source/toolkits/`): `net/` (raw socket helpers used before/alongside the HTTP
service), `offsetgen/` (I/O offset/access-pattern generators for sequential vs. random coverage),
`random/` (pluggable RNG algorithms — MT19937, Xoshiro256, GoldenPrime — selected via `RandAlgoSelectorTk`),
plus standalone helpers (`FileTk`, `HashTk` for data verification, `UnitTk`, `StringTk`, `TranslatorTk`,
`SystemTk`, `NumaTk`, `TerminalTk` for the live TUI, `OpsLogger`). Grouped, stateless helpers are
classes named `<Topic>Tk` with only static methods; pass the state a helper needs as parameters
rather than adding members or globals.

**Results & output** (`source/statistics/`): `Statistics` aggregates the worker counters (live values, the "First Done"
(stonewall) vs. "Last Done" results in `PhaseResults`, the service property trees) and decides
which workers count (`isAggregated()`: in netbench mode only the clients, because the servers see
the same bytes from the other side). `LiveStats` prints the live statistics (single line,
fullscreen, live CSV) and `PhaseResultsPrinter` the phase results (console/results file, CSV,
JSON; the fields are documented in [docs/csv-and-json-result-files.md](docs/csv-and-json-result-files.md),
the stonewall semantics in [docs/result-table-explanation.md](docs/result-table-explanation.md)). The words and
rows that depend on the benchmark mode (plain, rwmix write/read/total, netbench send/recv) come
from `ResultTerms`, chosen once per phase, so the printers contain no mode checks (`LiveOps`,
`LiveLatency` and `LatencyHistogram` are the counter types). The CSV/JSON
files are post-processed by the separate `elbencho-chart` and `elbencho-summarize-json` tools
(built from `tools/`, distinct from the main `elbencho` binary).

## Code conventions

- License is GPL-3.0-only; every file starts with the SPDX header:
  ```cpp
  // SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
  // SPDX-License-Identifier: GPL-3.0-only
  ```
- Old C++ code uses tabs for indentation, but all new or modified code lines in the `source/` subdir
  shall use 4 spaces for indentation.
- Keep the code simple and the comments brief: explain non-obvious intent, trade-offs or
  constraints, never what the code obviously does. Public functions get a short `/** ... */`
  Doxygen block (`@param`, `@return`, `@throw` where relevant); skip it for small obvious ones.
- Header guards follow the path: `#ifndef TOOLKITS_STRINGTK_H_` for `source/toolkits/StringTk.h`,
  `MODES_S3_S3MODE_H_` for `source/modes/s3/S3Mode.h`. System/library includes come first
  (alphabetical), then a blank line, then the project's `"quoted"` includes (alphabetical by path).
- Classes are PascalCase, methods and variables camelCase, macros UPPER_SNAKE_CASE. Use the shared
  aliases from `source/Common.h` (`StringVec`, `IntVec`, `UInt64Vec`, `SizeTVec`, ...) instead of
  raw `std::vector<...>`.
- Errors are thrown as `ProgException`/`WorkerException` with a descriptive message that includes
  the relevant values, e.g. `throw ProgException("Given namespace ID is invalid: " + nsIdStr);`.
- Insert a space before a closing paren whenever it would otherwise directly follow another
  closing paren, e.g. `if(nsName.empty() )`, but not when only one closing paren is present, e.g.
  `foo(a, b);`.
- Code must compile on macOS and Windows (Cygwin) as well as Linux; see the portability notes in
  the Build section above for the platform differences that trip this up.
- Keep the hot I/O path of every benchmark mode free of anything that could spoil the measured
  results: no virtual function calls, allocations, locks, string building, logging or other
  per-I/O overhead there. Decide such things once per phase (function pointers, templates,
  compile-time seams like `S3Transport.h`) and keep checks and option handling outside the loop.
- Service instances (`--service`) can be long-running and get a different benchmark mode, target
  type or option set with every run the coordinator sends them. So per-run state must be set up
  and torn down per phase/run and must never assume the mode or options of a previous run.
- Optional-feature source (e.g. `toolkits/spdk/`) should degrade cleanly to a no-op/absent when the
  corresponding `*_SUPPORT` build flag is off, matching the pattern of existing optional backends
  (S3, HDFS, CUDA): gate it with `#ifdef <FEATURE>_SUPPORT` and throw a clear message in the
  fallback branch when a user asks for a feature that was compiled out.
- `CHANGELOG.md` is updated per user-facing change, grouped under an unreleased "(work in progress)"
  version heading with `### New Features & Enhancements` / `### General Changes` / `### Fixes` subsections.
  Do not update `CHANGELOG.md` unless explicitly requested by user, and do not suggest or offer
  to update it either; the maintainer updates it.
- Do not suggest or offer to make a git commit. The user decides when to commit.
- Do not add new entries to the json result file and do not add new columns to the csv result file
  unless explicitly requested by user.

## Pull requests on GitHub

Release artifacts and the pull request tests are generated by the GitHub Actions workflows in the
separate [elbencho-devops](https://github.com/breuner/elbencho-devops) repository. Its README
describes every build configuration in detail; a change must not break any of them:

- **Pull request tests**: every new head commit of an open pull request gets built in three
  configurations (without S3, with S3, with the AWS CRT S3 client; backtrace, ibverbs and rdmacm
  off, mimalloc on) and runs `tests/run-tests.sh -a -j 4` and `tools/test-examples.sh -b`. The
  result is reported on the pull request.
- **Static Linux executables** for x86_64 and aarch64 (`.tar.gz`, `.rpm`, `.deb`), built with
  `BUILD_STATIC=1` on Alpine (musl libc) via `build_helpers/docker/Dockerfile.alpine-static.local`
  and tested with the full test suite.
- **Windows** (`elbencho-windows.zip`), built with Cygwin and tested with the default test set.
- **Docker images** for `linux/amd64` and `linux/arm64` from `Dockerfile.ubuntu2604.local`,
  `Dockerfile.alpine.local` and `Dockerfile.ubuntu-cuda-multiarch.local` in `build_helpers/docker/`.
- **Package repositories** for RHEL 8, 9 and 10 (built on Rocky Linux) and Ubuntu 24.04 and 26.04,
  each for x86_64 and aarch64, built with `S3_SUPPORT=1`, `SPDK_SUPPORT=1`, `IBVERBS_SUPPORT=1`,
  `RDMACM_SUPPORT=1`, `LIBFABRIC_SUPPORT=1` and `BUILD_LIBFABRIC=1`.

The macOS build is not covered by these workflows, but remains a requirement (see Portability
above), so review changes for it explicitly.

Rules for a pull request:

- **Test the change** for correctness before opening the pull request: build it, run the tests
  that cover the changed area (the whole default set where the change is not local to one mode),
  and exercise the new or changed behavior with the binary. State in the pull request what was
  tested and how, and what could not be tested (e.g. a build feature or platform that was not
  available).
- **Look for downsides** of the implemented approach, not only for correctness: added per-I/O
  overhead, new dependencies, changed defaults, behavior changes for existing users, portability.
  elbencho runs distributed across thousands of hosts, so consider the coordinator/service path in
  particular (more HTTP requests or larger messages per service, work that grows with the number
  of hosts or threads, timeouts, memory per service). Any downside or scalability concern must be
  mentioned explicitly in the pull request, even when it seems acceptable.
- **Do not modify this `AGENTS.md`** unless it is strictly necessary for the change (e.g. the change
  makes a statement in here wrong). If it is, the pull request must explicitly state the
  justification for the modification. The `AGENTS.md` files in the mode source dirs (`source/modes/s3/`,
  `netbench/`, `mpiio/`) hold the design notes of that mode and are kept in sync with
  the code: a change to a mode's design updates its file, and the pull request says so.
- Follow the conventions above: no `CHANGELOG.md` edits, no new result file entries or columns,
  no `HTTP_PROTOCOLVERSION` bump and no new tests unless explicitly requested; suggest them in the
  pull request instead where they would be relevant.
