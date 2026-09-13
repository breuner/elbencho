# SPDK tests

Black-box tests for elbencho's SPDK NVMe-oF initiator support. Enable them with
`../run-tests.sh -p` (or `-a`).

These tests bring their own target: `nvmf_tgt` and `rpc.py` are built together
with elbencho whenever `SPDK_SUPPORT=1` is used, so nothing has to be downloaded
or set up by hand. A test skips itself if the binary was built without SPDK
support or if the SPDK tools are missing.

## The target that each test starts

Every test script starts its **own** `nvmf_tgt` on a random free TCP port, with
its own RPC socket and its own backing files below its own `tests/tmp/<test>/`
dir. So the tests are independent, can run in parallel, and do not interfere
with an unrelated `nvmf_tgt` that uses the SPDK defaults.

It runs unprivileged: `--no-huge` avoids hugepages and `--no-pci` avoids any
device access. `--interrupt-mode` keeps it from busy-polling a CPU core, so no
external CPU limiting is needed.

The namespaces it exposes, via file-backed aio bdevs with a 4096 byte block
size:

| bdev | size | subsystem | namespace id | elbencho name |
| :--- | :--- | :--- | :--- | :--- |
| `aio0` | 128 MiB | `nqn.2016-06.io.spdk:cnode1` | 1 | `sysa:ctrl0:ns1` |
| `aio1` | 192 MiB | `nqn.2016-06.io.spdk:cnode1` | 2 | `sysa:ctrl0:ns2` |
| `aio2` | 256 MiB | `nqn.2016-06.io.spdk:cnode2` | 1 | `sysb:ctrl0:ns1` |
| `aio3` | 320 MiB | `nqn.2016-06.io.spdk:cnode2` | 2 | `sysb:ctrl0:ns2` |

The backing files are sparse, so they cost almost no disk space. Namespace UUIDs
are pinned to `a0000000-0000-4000-8000-00000000000<1-4>`, and tests select a
namespace by UUID via `spdk_ns_uuid <index>`. Do not select by numeric id:
elbencho assigns its own flat numeric ids in attach order, so they are not
stable. The names and the UUIDs are.

The sizes start at 128 MiB on purpose. elbencho prints namespace sizes with a
maximum length of 6 characters, so 64 MiB would be shown as `65536K` rather than
as a clean `64M`, which would make the expected discovery output awkward.

## Resource usage

Per test script: one `nvmf_tgt` with a 512 MiB memory pool plus one elbencho
with another 512 MiB. Both are lazily committed, so they cost address space
rather than resident memory. The relevant limit is CPU: elbencho's SPDK I/O
thread polls without sleeping, so each running SPDK test occupies about one
core, and the two-services test about three. Prefer `-j 4` or lower.

The memory pool sizes are constants at the top of `../lib/spdk.sh`. SPDK itself
reserves roughly 196 MiB for its internal buffer pools, so the usable floor is
about 320 MiB, plus elbencho's own I/O buffers:

```
mem_size_mb >= 320 + (numThreads * iodepth * largest block size)
```

If it is ever too small, elbencho says so explicitly and names the setting.

## What the tests cover

| Script | Covers |
| :--- | :--- |
| `discovery.t` | Namespace discovery against the target's own view, all four namespace selector forms, and the SPDK specific argument checks |
| `syncio-range.t` | Synchronous I/O, and containment of all accesses within `--offset`/`--size`, verified three independent ways |
| `asyncio-blockmix.t` | Asynchronous I/O via `--iodepth`, equivalence with the synchronous engine, and mixed block sizes |
| `rwmix.t` | `--rwmixpct` with exact operation counts in both engines, and `--rwmixthr` with exact reader/writer thread assignment |
| `verify.t` | `--verify` in both engines, including the cases that must fail: wrong salt, and right salt at the wrong offset |
| `journal-namespaces.t` | Journaled data verification (`--journaldir`) over several namespaces at once, its `--offset`/`--size` window, survival of a target restart, and an injected corruption of a backing file |
| `services-shared-ns.t` | Two elbencho service instances working on the same namespace at the same time, shared and with `--nosvcshare` |
| `target-lost-kill.t` | The target process is killed mid-run, for asynchronous write, synchronous write and read |
| `target-lost-remove-ns.t` | The benchmarked namespace is removed mid-run while the target stays up |

The two `target-lost-*` scripts deliberately do **not** run elbencho under a
timeout. elbencho handles `SIGTERM` cleanly, so terminating it looks exactly like
a graceful shutdown, and the only way to tell "elbencho noticed the broken
target" apart from "elbencho hung and the test gave up" is to never signal it
and check whether it exits by itself. If it does hang, the test fails and dumps
a `gdb` backtrace of where it is stuck.
