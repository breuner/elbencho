# Windows Support

elbencho runs on Windows as a Cygwin build. Cygwin provides the general compatibility layer
(sockets for the service mode, config files, log files, directory operations), but the benchmark
data itself does not go through it: files and block devices are opened, read and written through
the native Win32 API (`CreateFile`, `ReadFile`/`WriteFile`). This happens automatically, there is
nothing to select. The pre-built Windows package from the
[Releases](https://github.com/breuner/elbencho/releases) page runs in a normal command prompt
without a Cygwin installation.

## Paths

Paths are given in Cygwin notation, e.g. `/cygdrive/c/Users/Administrator/bench` for
`C:\Users\Administrator\bench`.

Block devices can be given either as Cygwin device names or as native Windows device names:

| Windows disk number (`Get-Disk`) | Cygwin name | Native name |
|---|---|---|
| 0 | `/dev/sda` | `\\.\PhysicalDrive0` |
| 1 | `/dev/sdb` | `\\.\PhysicalDrive1` |
| 2 | `/dev/sdc` | `\\.\PhysicalDrive2` |
| ... | ... | ... |

The native name needs quoting in a shell (`'\\.\PhysicalDrive2'`), or can be written with forward
slashes instead (`//./PhysicalDrive2`). As on Linux, elbencho refuses write access to a disk only
if the operating system does; a disk that holds mounted volumes cannot be written to.

```bash
elbencho -w -r -t 16 -b 1M --iodepth 16 --direct --rand --timelimit 30 /dev/sdc /dev/sdd
elbencho -r -t 16 -b 4k --iodepth 32 --direct --rand --timelimit 30 --lat '\\.\PhysicalDrive2'
```

## How the options map to Win32

- `--direct` opens with `FILE_FLAG_NO_BUFFERING` (the equivalent of `O_DIRECT`). Block sizes and
  offsets have to be multiples of the sector size (typically 512 bytes or 4 KiB) and the I/O
  buffers are page-aligned, as on Linux.
- `--iodepth` greater than 1 uses asynchronous (overlapped) I/O with an I/O completion port per
  worker thread, in place of libaio on Linux. Block sizes have to be smaller than 4 GiB.
- Every worker thread always opens its own handle for each file or block device, so
  `--nofdsharing` makes no difference on Windows. (A handle opened for synchronous I/O would
  serialize the I/Os of all threads.)
- `--trunctosize`, `--preallocfile` and `--statinline` use `SetFileInformationByHandle` and
  `GetFileInformationByHandle`.
- Random writes to a new or extended file are slower than on Linux, because NTFS zero-fills the
  gap between the end of the data written so far and the write offset. Write such files
  sequentially once (or preallocate them) before running random write tests on them.

## Not available on Windows

These options are rejected with a corresponding message, because they need a Cygwin file
descriptor or a Linux kernel interface: `--mmap`, `--flock`, `--fadv`, `--madv`, `--dropcache`.
`--sync` falls back to Cygwin's `sync()`. Of the optional build features, these are not
available: libaio (not needed, see above), libnuma, core binding, syncfs, backtraces and thread
names; S3 and the RDMA based netbench engines are not part of the Windows package. The fullscreen
live statistics are not supported in the Windows console.

## Building on Windows

Install Cygwin with these packages: `bash gcc-g++ git libboost-devel libssl-devel make zip` (plus
`jq perl procps-ng` for the test suite). The Makefile detects Cygwin and configures the build
accordingly:

```bash
git clone https://github.com/breuner/elbencho ~/elbencho
cd ~/elbencho
make -j $(nproc)
bin/elbencho.exe --help
```

`build_helpers/cygwin/create_package.sh` builds the release zip, which contains the executable and
the Cygwin DLLs it needs.

## Running the test suite on Windows

The default test set (`tests/run-tests.sh`) runs on Windows too. Process creation is slow in
Cygwin, so running several test scripts in parallel helps:

```bash
tests/run-tests.sh -j 10
```

Tests for features that are not available on Windows skip themselves: `--cores` binding, the
ibverbs/rdmacm/libfabric netbench engines, the IPv6 service test (the service listens on IPv4 only
on Windows) and the two tests that need the Linux `127.0.0.0/8` loopback range.
