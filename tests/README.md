# elbencho test suite

Black-box tests that run the `elbencho` binary and verify its results. The
[prove](https://perldoc.perl.org/prove) TAP harness executes the tests, the
tests themselves are plain bash scripts.

Nothing in here is part of a release build or package. Only `tests/tmp/`, the
dir for temporary files, is excluded from git via the `.gitignore` in here.

## Running the tests

```bash
tests/run-tests.sh              # all posix & distributed mode tests
tests/run-tests.sh -a           # everything, including S3
tests/run-tests.sh -s -j 4      # add the S3 tests, 4 test scripts in parallel
tests/run-tests.sh -r           # add the S3-over-RDMA plugin tests
tests/run-tests.sh -v tests/tests_posix/smallfiles-dirmode.t   # a single test, verbose
tests/run-tests.sh -c tests/tests_posix/smallfiles-dirmode.t   # ... and log its commands
tests/run-tests.sh -e /usr/bin/elbencho                  # test an installed binary
tests/run-tests.sh -h           # all available options
```

`-c` writes every command that a test executes, with its exit code, its runtime
and its complete console output, to `tests/tmp/commands/<test script>.log`. That
is the way to confirm that a test really does what it claims also when it does
not report a failure: the same console output is in the `.out` files below the
temp dir of the test, but that dir is gone when the test ends, and the command
lines themselves are not recorded anywhere else. The transcripts also cover the
minio server, spdk's `nvmf_tgt` and `rpc.py` and the `aws` cli calls, plus the
logs of the background processes, and each command line is quoted so that it can
be pasted into a shell as it is. The dir is emptied at the start of a run, so it
always holds the transcripts of the most recent one.

By default only the tests for backends that are always compiled in are run.
Tests for optional backends are enabled explicitly (`-s` for S3, `-p` for SPDK,
`-r` for the S3-over-RDMA plugins) and skip themselves if the given binary was
built without the feature. The netbench tests are part of the default set.

The S3 server used by the S3 tests is [libreFS](https://github.com/libreFS/libreFS),
a community fork of the minio server, which was discontinued as open source. It
gets downloaded on demand and stored as `tests/tmp/minio`, because it keeps
minio's command line interface and is used as a drop-in replacement. An already
existing file at that path is kept as it is, so delete it to pick up a newer or
a different server. `ELBENCHO_TEST_MINIO` points at a different binary, and the
S3 tests skip themselves while none is available.

Note that this server is licensed under the AGPL-3.0. It is only downloaded for
running the tests and is neither linked against elbencho nor redistributed with
it, so it does not affect elbencho's own licensing.

The S3 tests can also run against an already running S3 server instead of
private minio instances, e.g. an RDMA-capable one. Point `ELBENCHO_TEST_S3_ENDPOINT`
at it and set the credentials; nothing gets downloaded or started then:

```bash
ELBENCHO_TEST_S3_ENDPOINT=http://s3.example.test:9000 S3_KEY=mykey S3_SECRET=mysecret \
    tests/run-tests.sh -s -r
```

The tests create and delete buckets named `elbencho-test-<test script name>` on
that server.

Requirements: `prove`, `jq`, `timeout`, for the S3 tests the `aws` cli tool, and
for the SPDK tests `python3` (spdk's `rpc.py`). The S3 server is downloaded
automatically into the temporary dir when the S3 tests are enabled for the first
time. The SPDK tests need no download: `nvmf_tgt` and `rpc.py` are built
together with elbencho when `SPDK_SUPPORT=1` is used. The S3-over-RDMA tests
need `make` and a C++17 compiler for their server, see below.

Each running SPDK test occupies roughly one CPU core, because elbencho's SPDK
I/O thread polls without sleeping. Prefer `-j 4` or lower together with `-p`.

## S3-over-RDMA plugin tests

`-r` runs the tests of the `s3rdma_*` plugins from `contrib/plugins/` against
[tools/s3rdma/server](../tools/s3rdma/server/README.md), a minimal S3 server that
moves object data over RDMA and stores the objects as plain files. The whole
group skips when elbencho was built without an `s3rdma_*` plugin. Otherwise
`run-tests.sh` builds the server, the
[cuObject client shim](../tools/s3rdma/cuobjclient-shim/README.md) and
[tools/s3rdma/client](../tools/s3rdma/client/README.md) once before the test
scripts start (so parallel runs never build; the build uses the job count of
`-j`), in a copy of their sources below the temporary dir (`<tmp dir>/s3rdma/`),
so that the tests write nothing outside of it; `make -C tools/s3rdma` builds them
in place for interactive use. Each test
script starts its own private server instance in its temporary dir, like the
minio tests do. A failed build is reported as a note, not as an error, and the
tests skip themselves while a tool is missing.

The plugins reach the server through NVIDIA's cuObject client API, and the tests
run them with one of two libraries behind that API:

* NVIDIA's `libcuobjclient` (`cuobj`): RDMA with DC transport. This needs a NIC
  with DC support (ConnectX-5 or newer), the NVIDIA GPU driver on the client,
  `libcufile_rdma.so` (auto-detected, or `ELBENCHO_TEST_CUFILE_LIB_DIR`) and
  `libcuobjserver` for the server build. The cuObject client config is written
  per test (`cuobj.json` in the test's temp dir) with the detected NIC address
  and without `allow_compat_mode`, so that nothing silently falls back;
  `ELBENCHO_TEST_CUFILE_COMPAT_MODE=1` adds it for hosts where cuFile refuses to
  initialize otherwise. On hosts without the `nvidia_peermem` module (the open
  kernel module uses dma-buf instead), cuFile would disable userspace RDMA, so
  the config then sets `rdma_peer_type` to `dmabuf`; `ELBENCHO_TEST_CUOBJ_PEER_TYPE`
  overrides this choice.
* The cuObject client shim from `tools/s3rdma/` (`rc`): the same API over
  Reliable Connections on plain `libibverbs`, which works on any RoCE NIC and
  without a GPU. The tests put it on `LD_LIBRARY_PATH`, which also redirects an
  elbencho that was linked against NVIDIA's library to the shim (and the Cloudian
  SDK fork, which loads the library at runtime). `ELBENCHO_TEST_CUOBJ_SHIM_LIB_DIR`
  points at a shim built elsewhere.

By default a test uses NVIDIA's library where it can work (`s3rdma-server
--rdma-check` tries the DC transport on the NIC, and the GPU needs 2 GiB of free
memory for the CUDA contexts of the cuObject clients, because cuFile aborts the
process otherwise) and otherwise falls back to the shim, with a note on stderr
that says why. `ELBENCHO_TEST_S3RDMA_TRANSPORT=cuobj|rc`
pins one of the two. Both need an active RDMA link with an IPv4 address
(auto-detected, or `ELBENCHO_TEST_RDMA_ADDR`), a locked memory limit of at
least 64 MiB (`ulimit -l`; systemd services get 8 MiB by default, so a test
runner started as a service skips these tests unless its unit raises
`LimitMEMLOCK`), and elbencho built with an `s3rdma_*` plugin
(`ELB_PLUGIN_S3RDMA_MINIO=1` or `ELB_PLUGIN_S3RDMA_CLOUDIAN=1`); the first one
found is used, or the one named in `ELBENCHO_TEST_S3RDMA_PLUGIN`.

That the data really moved over RDMA is checked independently of the plugin and
the library: the server only stores what it received via RDMA (the control
requests have no body), its log is checked for one RDMA transfer per object or
part, and the NIC's `rx_read_requests` / `rx_write_requests` hardware counters
have to grow. The NIC and the GPU may be in use by other programs while the tests
run: the counters are only required to grow, each test counts the transfers in
the log of its private server, every server instance gets its own cuObject port
(one process per port) and the buffers stay small, a few MiB per thread on both
sides.

The test scripts in `tests_plugin_s3rdma/`:

* `putget-rdma.t`: single-part PUT and GET over RDMA with an `s3rdma_*` plugin,
  with data verification.
* `gpu-rdma.t`: the same from and to GPU memory (`--gpuids`; with the
  `s3rdma_cloudian` plugin both host-staged and GPU-direct via `--gds`). Needs
  NVIDIA's cuObject library, elbencho built with CUDA support and a GPU (the
  first one that `nvidia-smi` lists, or `ELBENCHO_TEST_GPU_ID`); the GPU-direct
  modes also need a working GPUDirect RDMA path between the GPU and the NIC.
* `mpu-rdma.t`: multipart upload and ranged download over RDMA with an
  `s3rdma_*` plugin, including shared uploads of one object by several threads
  (`--sharesize`).
* `service-rdma.t`: distributed mode. The coordinator activates the plugin on a
  local service instance per run, and a run without the plugin against the same
  service has to move the data in the HTTP body again.
* `plugin-argchecks.t`: activation via `--plugins` and the argument checks of the
  active plugin. Needs neither a server nor RDMA hardware.
* `plainhttp-mpu.t`: self-test of the server over plain HTTP (multipart upload,
  ranged reads, listing, declining an RDMA token). Only needs the server
  executable and runs on hosts without RDMA hardware as well.
* `client-shim-rc.t`: the cuObject-style protocol over Reliable Connections with
  `tools/s3rdma/client` linked against the shim, independent of elbencho's
  plugins.
* `hipobj-rc-v2.t`: the server's third protocol, AMD hipObject's `hipobj-rc-v2`
  with RC transport. No elbencho plugin speaks it yet, so the test drives it with
  hipObject's own RDMA test client, which the server's `Makefile` builds from the
  downloaded hipObject sources when `libibverbs` and OpenSSL development files
  are present (`libibverbs-dev`, `libssl-dev`). It always uses the private server.

## Temporary files

Everything a test creates lives in `tests/tmp/<test script name>/`, which the
test removes when it is done - also when it failed. Use `-k` to keep the dir of
a failed test for inspection, and `-T DIR` to place the temporary files
somewhere else, e.g. on the file system that is to be tested:

```bash
tests/run-tests.sh -k -T /mnt/myfs/elbencho-tests
```

The command transcripts of `-c` are the exception: they live in
`tests/tmp/commands/` next to the dirs of the test scripts, because they have to
survive the test that wrote them.

After an aborted run, `rm -rf tests/tmp` is all that is needed to clean up. That
also removes the downloaded S3 server and the command transcripts.

## Layout

Dirs holding test cases are prefixed with `tests_`; everything else holds supporting files.

| Path | Contents |
| :--- | :--- |
| `run-tests.sh` | Wrapper that checks the prerequisites and calls `prove`. |
| `lib/testlib.sh` | TAP output, assertions, temp dir & watchdog handling, command tracing, free port selection, result file, file system and process inspection. |
| `lib/minio.sh` | Download-once S3 server (libreFS, a minio fork), one private instance per test, or an external S3 server via `ELBENCHO_TEST_S3_ENDPOINT`. Also the aws cli helpers shared by all S3 tests. |
| `tests_posix/` | Local file & dir benchmarks. Always run. |
| `tests_distributed/` | Benchmarks through a local elbencho service instance. Always run. |
| `tests_netbench/` | Network benchmarks (`--netbench`) between local elbencho service instances, over the tcp engine (the default), the libfabric engine and, where an RDMA interface exists, the ibverbs and rdmacm engines and the libfabric verbs provider. Run when the binary was built with netbench support. |
| `lib/netbench.sh` | Starts the service instances for the netbench tests, inspects their logs and computes the expected transfer sizes. |
| `tests_s3/` | S3 object benchmarks against minio. Enabled via `-s`. |
| `tests_spdk/` | SPDK NVMe-oF benchmarks. Each script starts its own private target. Enabled via `-p`. See [tests_spdk/README.md](tests_spdk/README.md). |
| `lib/spdk.sh` | Starts and provisions a private `nvmf_tgt`, plus target side and SPDK specific result helpers. |
| `tests_plugin_s3rdma/` | S3-over-RDMA tests via the `s3rdma_*` plugins and the test client against `tools/s3rdma/server`. Enabled via `-r`. See [S3-over-RDMA plugin tests](#s3-over-rdma-plugin-tests). |
| `lib/plugin_s3rdma.sh` | Builds nothing itself, but starts a private `s3rdma-server` with the RDMA transport under test, configures the cuObject client or the RC shim, runs the test clients and inspects the server's files, log and the NIC's RDMA counters. |
| `lib/journal.sh` | Journal dir and sidecar inspection for the `--journaldir` tests, plus target corruption helpers. |
| `lib/interactive.sh` | Lets the two server helpers above run outside a test, for the scripts in `tools/`. |
| `tools/` | Ad-hoc servers for interactive use, not test cases. See [Interactive servers](#interactive-servers). |

## Coverage of tools/test-examples.sh

[tools/test-examples.sh](../tools/test-examples.sh) is the older manual smoke test. It runs a
handful of documented example command lines and only checks their exit codes, and
its block device cases need root privileges. Everything it exercises is covered
here, so it does not have to be run anymore:

| Case in tools/test-examples.sh | Covered by |
| :--- | :--- |
| `test_loopdev_read_lat` (`--lat --cpu --direct --rand` on a block device) | `tests_posix/latency-cpu-stats.t` for the statistics, `tests_posix/asyncio-filemode.t` for direct random IO, `tests_spdk/` for block device mode itself |
| `test_loopdev_write_iops` (`--iodepth --direct --rand`, two devices) | `tests_posix/asyncio-filemode.t`, `tests_posix/multipath-filemode.t` |
| `test_loopdev_read_stream` (`--iodepth --direct`, streaming read) | `tests_posix/asyncio-filemode.t` |
| `test_multifile_create` / `_read` / `_delete` | `tests_posix/smallfiles-dirmode.t`, `tests_posix/largefiles-dirmode.t`, plus `tests_posix/latency-cpu-stats.t` for its `--lat` |
| `start_distributed_services` (`--service` without `--foreground`, `--zone`, `--core`) | `tests_distributed/service-daemonized.t`, `tests_distributed/service-cpu-binding.t` |
| `test_distributed_master` (`--hosts localhost:[P1-P2]`, two services, all phases in one run) | `tests_distributed/services-shared-dir.t` |
| `stop_distributed_services` (`--quit`) | `tests_distributed/service-daemonized.t`, `tests_distributed/services-shared-dir.t` |
| `test_random_io_create` (`--rand --direct --iodepth --preallocfile`, `du` checks) | `tests_posix/asyncio-filemode.t`, `tests_posix/prealloc-truncate-filemode.t` |
| `test_random_io_read` / `_delete` (uneven thread and file counts) | `tests_posix/multipath-filemode.t` |

The loopback block devices themselves are deliberately not reproduced, because
`losetup` requires root privileges. File mode covers the same IO options, and
block device mode as such stays covered by `tests_spdk/`, which reports its
namespaces as `blockdev` benchmark paths.

## Interactive servers

`tools/` holds two scripts that are not test cases. They start the same servers
that the automated tests use, but keep them running until ctrl+c, so they can be
used for poking around by hand:

```bash
tests/tools/start-nvmeof-target.sh      # SPDK NVMe-oF target, 2 subsystems x 2 namespaces
tests/tools/start-minio.sh              # minio S3 server with a bucket
tests/tools/start-nvmeof-target.sh -h   # all options
```

Both print the connection details and a few ready-to-paste elbencho command
lines when they come up, and both **keep their data** when stopped, so a dataset
written today can be read back and verified tomorrow, also after a reboot. Use
`-C` / `--clean` to start over with empty storage instead.

Their storage lives in `tests/tmp/tools-*/`, next to the test dirs, so
`rm -rf tests/tmp` removes it along with everything else. Test scripts must
therefore never be named `tools-*.t`, because the temp dir is derived from the
script name and would collide.

Notes:

* Ports are fixed by default (4420 for the NVMe-oF target, 9000 for minio) so
  that a saved command line keeps working. A port that is already in use is
  reported as an error rather than silently replaced; use `-p PORT` then.
* The NVMe-oF target needs a **concrete** listen address and defaults to the
  auto-detected address of this host, so that it is reachable from other hosts.
  A wildcard address cannot be used: NVMe-oF only accepts clients that connect
  to an address which is registered as a listener of the subsystem, so `0.0.0.0`
  would reject every client. Use `-a 127.0.0.1` to keep it local.
* Both servers are **unprotected**: the NVMe-oF target allows any host to read
  and write its namespaces, and the S3 server uses the well known test
  credentials from `lib/minio.sh`. They warn about this when they are reachable
  from other hosts. Only use them on a trusted network.
* Only one instance can use a storage dir at a time, enforced with a lock file.
  Use `-n NAME` or `-T DIR` for a second, independent instance.
* The target runs on a single core without huge pages, so it is meant for
  functional testing rather than for performance measurements.
* Looking for leftover server processes needs care, because the two servers need
  opposite matchers. For the NVMe-oF target, match the **command line**
  (`pgrep -a -f nvmf_tgt`), because SPDK renames its main thread to `reactor_0`
  so a name match finds nothing even while a target runs. For elbencho service
  instances it is the other way round: `pgrep -f 'elbencho --service'` also
  matches the shell that is running the check, so match the **process name**
  instead, e.g. `ps -eo comm,args | awk '$1 ~ /^elbencho/'`.

## Adding a test

Test scripts are named `<what it tests>.t`, are executable and start like this:

```bash
#!/bin/bash

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1

test_init
tap_plan 3

run_elbencho write -w -t 2 -s 1m -b 1m "$TEST_DIR/file"
assert_ok $? "write phase succeeds"
assert_eq "$(file_size "$TEST_DIR/file")" "1048576" "the file has the expected size"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "1048576" \
    "write phase reports the expected number of bytes"
```

Rules to keep tests independent, fast and safe to run in parallel:

* Call `test_init` before the first check. It creates `$TEST_DIR`, arms the
  cleanup trap and starts the watchdog that kills the script if it hangs.
* Write everything below `$TEST_DIR`, never anywhere else.
* Use `run_elbencho <tag> <args>` instead of calling the binary directly. It
  adds `--nolive --no0usecerr`, puts the result files in
  `$TEST_DIR/<tag>.{txt,csv,json,out}`, exports their paths as `$ELB_JSON`,
  `$ELB_CSV`, `$ELB_RES`, `$ELB_OUT` and enforces the per-command timeout.
* Keep the runtime in the range of a few seconds. Datasets of a few dozen MiB
  are plenty to verify counters.
* Never hardcode a TCP port. Use `find_free_port` plus `wait_for_port`, and
  retry the start if the service did not come up.
* Redirect stdout and stderr of every background process to a file and register
  it with `register_pid`. `prove` reads the test's stdout until EOF, so a
  background process that inherits it would make the whole run hang.
* Skip instead of failing when a precondition is missing:
  `require_build_feature s3`, `require_async_io` (libaio, or the native async
  I/O of the Windows build), `require_cmd aws`, `fs_supports_directio`,
  `is_cygwin` for what Windows cannot do.
* Keep the number in `tap_plan` in sync with the number of checks. `prove`
  reports a mismatch as a failure. Emit the plan only after everything that
  could still skip the whole file, because a `1..0 # SKIP` line after the plan
  is not valid TAP - that is why `require_minio` and `require_spdk` are called
  before `test_init`.
* Do not name a test script `tools-*.t` or `commands.t`: the temp dir is derived
  from the script name, and `test_init` starts by removing it, so such a script
  would delete the storage of the interactive servers or every command
  transcript of the running suite.
* A new place that starts elbencho or a helper server without going through
  `run_elbencho` has to call `trace_cmd` (and `trace_add_log` for a background
  process), so that `-c` keeps showing everything. Two rules for that call: it
  must not be inserted between a command and a `$?` that belongs to it - save
  the exit code in a variable first - and it must not be placed inside a poll
  loop without `local TRACE_QUIET=1`, or it writes the same line hundreds of
  times.

## Notes on verifying elbencho results

* The json result file (`--jsonfile`) is in json-lines format with one object
  per benchmark phase. Every scalar is written as a quoted string, and counters
  whose total is zero are omitted, which is why `json_value` falls back to `"0"`.
* Counters worth asserting are `entries` and `bytes` under `last_done`. There is
  no entry counter when the benchmark path is a file or block device, and no byte
  counter in dir or delete phases.
* The dirs counter of the mkdirs phase counts only the `d<num>` dirs, not the
  `r<rank>` parent dir of each thread. So the number of dirs on the file system
  is higher than the reported number of entries.
* Throughput, IOPS and latency values are only checked for being non-zero,
  never against a fixed value.
* The command transcript of `-c` is a file and not a TAP diagnostic, because
  several of the traced helpers run inside a command substitution of their
  callers (`count=$(aws_s3api list-objects-v2 ...)`, `spdk_rpc ... | jq`), where
  a line written to stdout would end up in the caller's variable or in jq's
  input instead of in the output of the test. Writing it to a dup of stdout
  would escape the command substitution, but such a file descriptor is inherited
  by every background server and would keep `prove` waiting for end of file.
* Do not use `date +%s%3N` anywhere in this suite. The uutils reimplementation
  of coreutils, which some distributions now ship as `/usr/bin/date`, ignores
  the field width and returns nanoseconds, so a duration computed from it is a
  million times too large. Use `$EPOCHREALTIME` as `trace_now_ms` does, and note
  that its decimal separator depends on the locale.
* The ops log helpers report `MALFORMED_OPSLOG` when they cannot parse the log,
  so a check that fails with that value means the log itself is broken and not
  that a count is wrong. That happens when a log line is written with more than
  one `write()` call, because only a single `write()` to a file opened with
  `O_APPEND` cannot interleave with the writes of the other worker threads.
  `OpsLogger::logOpJSON()` therefore formats each line first and submits it with
  one `write()`, which is why the tests here do not need `--opsloglock` even
  with many threads. If this ever fails again, count the `write()` calls per log
  line with `strace -f -y -e trace=write` before looking anywhere else.
* In the operations log (`--opslog`), `entry_name` holds the path for operations
  that work on a name (`openat`, `mkdirat`, `unlinkat`, all S3 operations), but
  the numeric file descriptor for read and write operations. Use
  `opslog_distinct_ranks` for the former and `opslog_max_distinct_ranks` for the
  latter to verify that an entry was shared between threads.
* Live statistics rows (`--livecsv`) are only written on `--liveint` ticks, so a
  test that runs for milliseconds may legitimately produce a header line only.
  Only the header and the shape of existing rows are verified.
* Netbench mode is different from every other mode in several ways worth knowing
  before touching `tests_netbench/`: it needs **no benchmark path**, it always
  runs the write phase (so no `-w` and no other phase option), it reports its
  phase as `NET/<engine>` (`$NET_PHASE` in `lib/netbench.sh`, e.g. `NET/tcp`)
  with a `path_type` of `net`, and it populates only byte
  counters - `entries` is absent, so `json_value` returns `0` for it. Its block
  size is `--sendsize` (the same option as `-b`), and `-s` defaults to a huge
  value, so a test has to give `-s` or the run only ends with the time limit.
* The netbench data endpoints use ports chosen by the OS and announced through
  the service HTTP API, so the tests only allocate service ports, with
  `find_free_port` like the distributed tests. `--netport` on a server service
  fixes the base port instead, see `fixed-port.t`.
* A netbench result counts the clients only (the servers see the same bytes
  from the other side): the clients send
  `clients * client threads * rounds * sendsize` bytes (`bytes` in the json
  result) and receive `clients * client threads * rounds * recvsize` bytes
  (`rwmix_read` `bytes`), where `client threads` is `--tclients` (default `-t`)
  and `rounds` is the size divided by the send size (or by the receive size for
  a receive-only stream). Keep the size an exact multiple of the round size,
  because elbencho divides them as integers. `lib/netbench.sh` has
  `netbench_expected_send_bytes` and `netbench_expected_recv_bytes` for this.
* Which client thread talks to which server endpoint is only visible in the
  service logs (`--log 1`): a server logs `Accepted stream from client rank N:
  <client address> -> <endpoint>`, a client logs `Established connection to:
  <server service>; endpoint: <server endpoint>; via: <own endpoint>`. An
  endpoint is described as `<engine words>: ADDRESS (DOMAIN)` by every engine
  (`tcp socket: 127.0.0.1:1234 (lo)`, `ibverbs rc: 10.0.0.1 (mlx5_0 port 1
  gid 3)`, `tcp FI_PROTO_XNET: 127.0.0.1:1234 (lo)`), which the
  `netbench_log_*_since` helpers rely on when they extract the counts and
  addresses from the lines written after a mark taken before the run
  (`netbench_log_conn_pairs_since` gives the endpoint/own address pairs).
* Streams are placed by the coordinator so that client and server interfaces
  share a subnet (`--netcrosssubnet` relaxes that). On loopback, the IPv4 and
  IPv6 addresses of `lo` (`127.0.0.0/24` given as subnet, `::1`) serve as two
  separate subnets, and an address like `127.0.1.2` (covered by `lo` as
  `127.0.0.0/8`, but not inside `127.0.0.0/24`) as a third one, see
  `subnet-placement.t`. `--netdevsubnets` (subnets from the `--netdevs` entries)
  is checked there through the interfaces the coordinator logs with `--log 1`,
  because loopback cannot show a `/32` address without root. The service start
  checks of `--netdevs` (error when no entry exists, warning when some do not)
  are in `netdevs-subnet.t`.
* Netbench mode is always built, but the engines are separate build features
  (`libfabric`, which needs a `LIBFABRIC_SUPPORT=1` build, `ibverbs`, `rdmacm`)
  for `require_build_feature`. `rdma-verbs.t`, `engine-ibverbs.t` and
  `engine-rdmacm.t` additionally
  need an RDMA-capable interface with an IPv4 address (`rdma_first_ipv4`) and
  loop the traffic through the local adapter. `rdma-verbs.t` also needs a locked
  memory limit of at least 512 MiB (`ulimit -l`; `require_memlock 512` skips
  otherwise, as systemd services such as a Jenkins agent get 8 MiB by default),
  because the libfabric verbs provider pins about 65 MiB per endpoint at
  libfabric's defaults and the test has four endpoints. The ibverbs and rdmacm
  engines only pin their own small buffers, so their tests run within a few MiB.
  Both engine tests also provoke receiver-not-ready retries with a send-only run
  of 4 KiB blocks, eight in flight and two receive buffers (`--netrecvpct 25`),
  and check the adapter's RNR NAK counter when sysfs has one
  (`rdma_rnr_nak_count`, Mellanox `hw_counters`), else they skip that check.
  Small blocks matter: with 64 KiB blocks the server reposts while the client
  fills its next buffer, so the count is small and vanishes under load.
* `--verify <salt>` makes elbencho disable block variance implicitly, so
  `--blockvarpct 0` does not have to be given. Reading with a different salt has
  to fail, which is what proves the check is effective. The written pattern
  depends on the absolute offset, so reading the right salt at the wrong offset
  has to fail as well.
* Journaled data verification (`--journaldir`) disables block variance the same
  way, so a journaled write needs no `--blockvarpct 0` of its own either. The
  journaled tests deliberately leave it out, which means every one of their
  write cases also covers that implicit disable: if it stopped happening, the
  read-back verification would fail. `--verify` on the other hand is rejected
  together with `--journaldir`, so the journaled tests never combine those two.
* `--journalsync` implicitly enables `--direct`, because a committed content
  generation is only meaningful if the target write it describes is durable once
  it completes. So a `--journalsync` case needs an `fs_supports_directio` guard,
  and `json_config … direct_io` is how a test confirms the implicit enable.
* A journal is matched to a target by exact string equality between the
  `target` field of its json sidecar and the benchmark path *as it was typed on
  the command line*. The same target under a different spelling - `./f` versus
  `f`, a symlink, or an SPDK namespace selected by name instead of by uuid - is
  a different target and gets a second journal with its own random seed. Two
  journals for the same bytes means one of them reports a bogus verification
  failure, so a test must keep the selector identical across the runs that are
  meant to resume, and `lib/journal.sh` therefore looks journals up by target
  string rather than by file name. Editing that one field is also the supported
  way to point a journal at a renamed target, see `journal_remap_target`.
* The note for a resumed journal carries a `Verifiable: <pct>%` value, the share
  of the journal's blocks that hold a content generation. It is exactly
  predictable - journal half of a range and it reads `50.0` - so tests assert it
  rather than just its presence, via `journal_resumed_percent`. A newly created
  journal gets no percentage, being 0.0% by definition.
* A journal block that was never written is skipped in a pure read phase: no
  read is issued for it and it counts as verified. It is counted as submitted
  but **not** as bytes done, so a journaled read of a partially written target
  legitimately reports fewer bytes than its range and logs fewer operations
  than it has blocks. That only adds up exactly while the IO block size equals
  `--journalblock`; with a larger IO block, an IO that covers initialized *and*
  uninitialized journal blocks is read and counted as a whole.
* In a read/write mix the same block is initialized instead of skipped, i.e.
  the read is converted into a write. So a mix phase against a *fresh* journal
  logs no read at all and reports nothing under `rwmix_read`, and only the
  second run against the same journal produces the read counts that the
  `(workerRank + opNum) % 100 < pct` formula predicts. Both runs are exact, but
  a *partially* initialized journal with more than one thread is not: whether a
  read converts then depends on which thread got to the block first.
* `--journaldir` writes its files into the given dir and nowhere else, and its
  locking is per process. But elbencho parses *every* `*.journal.json` in that
  dir on startup, including ones belonging to targets the run does not use, so
  a deliberately broken sidecar poisons every later run in the same dir. Tests
  that create broken sidecars therefore use one journal dir per case.
* A run that is cut short - by `--timelimit`, by ctrl+c or by an IO error -
  leaves the journal blocks of the writes that were in flight marked as never
  written. After that, neither byte counts nor read/write mix counts are
  predictable, so rewrite the range before asserting on them. Note that an
  unthrottled local write gets through far more than a second of work before the
  shortest possible `--timelimit 1` fires, so use `--limitwrite` when a test
  needs a *partially* written range, and pre-create the target at full size,
  because elbencho rejects a read phase whose offset plus size exceeds it.
* The journal binary file is `ceil(size / journalblock) * 2` bits rounded up to
  whole bytes, and it is preallocated with `posix_fallocate` on creation, so it
  reports allocated blocks even though nothing has been written to it yet.
  `journal_expected_binary_size` computes the expected size.
* Boost's json writer quotes every value, so all sidecar fields come back as
  strings: compare against `"4096"`, never with `jq '.journalBlockSize == 4096'`.
* A journal verification failure can be printed twice, once by the worker and
  once from the error history that the coordinator dumps at the end. Match the
  message with `assert_match`, do not count its occurrences.
* In SPDK mode the benchmark paths are namespace selectors, and the result files
  report the path type as `blockdev`, i.e. there is no entry counter, only byte
  counters. The operations log uses the human-friendly namespace name as its
  entry name, so `opslog_distinct_ranks` works on it directly, unlike in file
  mode where the entry name is a file descriptor.
* A mixed read/write phase is not called `WRITE` in the result files:
  `--rwmixpct 30` reports the phase as `RWMIX30` and `--rwmixthr 2` as `MIX-T2`.
  The read side of such a phase lives in a `rwmix_read` subtree, reachable via
  `json_value_rwmix`.
* A block size mix is drawn randomly without a seed, so the number of blocks per
  size is not predictable and must not be asserted. The total amount of data is
  predictable, and the last block of a thread's range may be clipped to fit, so
  the set of block sizes seen can contain one extra value per thread.
* Latency values only appear in the json result file with `--lat`, below
  `last_done.latency` in an `entries` and an `IO` subtree, reachable via
  `json_latency`. There is no entry latency where no entry is opened per
  operation, i.e. in file and block device mode, and no IO latency in a phase
  that transfers no data. `--lathisto` and `--lathistogrpd` add a `histogram`
  subtree instead, whose bucket counts add up to the number of operations, but
  no minimum, average and maximum - the two option groups are disjoint.
* `--cpu`, `--latpercent` and `--lathistogrpd` only control the results output.
  The CPU utilization is written to the json and csv result file either way, so
  what `--cpu` changes is the `CPU util %` row of the txt result file. Use
  `res_row_count` for those rows, which is also the only way to check the
  percentiles row of `--latpercent`.
* An `--iodepth` above 1 switches from `pread`/`pwrite` to the libaio based IO
  engine, which logs its operations as `aioread`/`aiowrite` in the operations
  log. Counting those is how a test can tell that the engine really changed
  instead of the option having been accepted and ignored.
* In file mode all given paths form one block space that the threads split
  between themselves. Random IO gives each thread a whole number of blocks and
  ignores the remainder, so the transferred amount is
  `(totalblocks / threads) * threads * blocksize`, which is less than the full
  dataset when the block count is not a multiple of the thread count.
  Sequential IO transfers everything.
* A service that is started without `--foreground` forks into the background, so
  the starting process returns immediately and its exit code says nothing about
  the service. The service writes its own log file to `$TMP`, so pointing `$TMP`
  at the test dir keeps that file inside it; the first line of that log holds the
  pid, but it is written after the starting process has already returned, hence
  `wait_for_file_line`. `--quit` is reported as success even when nothing is
  listening on the given port, so only `wait_for_pid_gone` proves that a
  daemonized service really ended.
* Neither `--cores` nor `--zones` appears in any result file. The effective
  affinity of the process is the only evidence that they took effect, which is
  what `cpus_allowed_list` reads. A NUMA zone binding cannot be verified this
  way on a host with a single zone, because the resulting mask is then the same
  as without any binding.
* Threads, dirs and files are per service, so the totals of a distributed run
  are multiplied by the number of hosts: `hosts * threads * dirs` entries in the
  dir phases and `hosts * threads * dirs * files` in the file phases. For a
  shared benchmark path the coordinator gives each service an offset for its
  worker thread ranks, so the file names of the services do not collide. That is
  worth checking on the file system and not only in the counters, because a
  collision would still report the expected number of entries.
