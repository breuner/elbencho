# MPI-IO Benchmarking

elbencho's MPI-IO mode (`--mpiio`) reads and writes file contents through the MPI-IO API
(`MPI_File_open`, `MPI_File_write_at`/`MPI_File_read_at`, or `MPI_File_write_at_all`/`MPI_File_read_at_all` with `--mpicoll` for collective IO). Everything else works as in the other file modes: the shared file
and directory modes, the sequential, `--strided` and `--rand` access patterns, `--timelimit`,
`--verify`, the live statistics and the result files. Directory creation, stat and delete phases
stay POSIX, only the file data path goes through MPI.

`elbencho --help-mpiio` shows the options with examples.

## Build

```bash
make -j $(nproc) MPI_SUPPORT=1
```

The build compiles and links through the MPI compiler wrapper (`mpicxx`; `MPICXX=<path>` picks
another one, and a `CXX=...` given by the user is kept as it is). The MPI library has to provide
`MPI_THREAD_MULTIPLE`, which Open MPI, MPICH and their derivatives do. `elbencho --version` lists
`mpiio` under the included features of such a build.

## Quick Start

```bash
mpirun -np 5 elbencho --mpiio -w -r -t 8 -b 1m -s 32g /mnt/shared/file
```

Rank 0 is the coordinator, every other rank runs an elbencho service instance that does the I/O
with `-t` threads. So `-np 5` means 4 I/O ranks, and the command behaves like a distributed run
with 4 hosts: each service rank starts on an OS-chosen port, sends its address to rank 0, which
runs the benchmark against them as `--hosts`, and all ranks end together. The result files are
written by rank 0, an `--opslog` by the service ranks (where the I/O happens).

How the ranks are placed on the hosts is up to mpirun (`--map-by node`, `-ppn 1`, hostfiles, ...).
One service rank per host with many threads is elbencho's usual service model, but several I/O ranks
per host work as well. For a Slurm batch job, see [docs/slurm-examples.md](slurm-examples.md).

Without mpirun, `elbencho --mpiio ...` is a standalone run with a single I/O rank. `--mpiio` has to
be on the command line (not only in a config file), because MPI gets initialized before the
options are parsed.

## Threads and Collective I/O

Each worker thread is a separate MPI-IO participant. With independent I/O every thread opens its
files on `MPI_COMM_SELF`, so no call ever waits for another thread or rank. With `--mpicoll` the
threads with the same index on all I/O ranks form one group with its own communicator: a run with
4 I/O ranks and `-t 8` has 8 groups of 4 participants, and each group opens the shared file
collectively on its own communicator and makes its collective data calls there.

**Note on MPI libraries:** the groups of a rank make their collective calls concurrently from
different threads. Open MPI's OMPIO handles that. MPICH's ROMIO (also used by Intel MPI, MVAPICH
and Cray MPICH) serializes all MPI-IO calls of a process behind one lock, so a thread that waits
inside a collective call blocks the other threads of its rank from entering theirs, while the
peers of those threads wait for them on other ranks: the run hangs. This was confirmed with MPICH
5.0.2 (independent I/O with threads is fine there, as are collective runs with one thread per rank),
and the coordinator prints a note when `--mpicoll` is used with more than one thread per service.
With such a MPI library, run collective I/O with `-t 1` and more ranks per host (`mpirun -ppn N`).
MPICH's fine-grained threading in CH4 (per-VCI critical sections, `MPIR_CVAR_CH4_NUM_VCIS`, async
progress) does not change this: ROMIO holds its own single mutex for the whole duration of each
MPI-IO call, independent of the CH4 locking (verified with MPICH 5.0.2).

### How threads map to file blocks

The worker threads of all I/O ranks are numbered consecutively (thread index
`i = rank * threads_per_rank + thread`, rank 0 being the first I/O rank), and the shared file is a
sequence of blocks of `-b` bytes. The example has 2 I/O ranks with `-t 2`, so 4 threads in total
(`T = 4`), and a file of 12 blocks (`n = 3` blocks per thread):

```
thread index:   i = rank * 2 + thread      r0t0 = 0, r0t1 = 1, r1t0 = 2, r1t1 = 3

contiguous (default):  thread i gets blocks [i * n, i * n + n - 1], one range per thread

  block:   0  1  2  3  4  5  6  7  8  9 10 11
  thread:  0  0  0  1  1  1  2  2  2  3  3  3

--strided:             thread i gets blocks i, i + T, i + 2T, ... (interleaved over all threads)

  block:   0  1  2  3  4  5  6  7  8  9 10 11
  thread:  0  1  2  3  0  1  2  3  0  1  2  3

--mpicoll:             thread t of every rank forms one collective group of R = 2 members

  group 0 = {r0t0, r1t0} = threads {0, 2}
  group 1 = {r0t1, r1t1} = threads {1, 3}

  round k: every group makes one collective call, in which each member accesses its k-th block

  contiguous:   group 0 accesses blocks {0, 6}, {1, 7}, {2, 8};  group 1 accesses {3, 9}, {4, 10}, {5, 11}

--mpicoll --strided:   group t owns the region of R * n blocks starting at block t * R * n, and
                       only its members interleave within that region: in round k, member m
                       accesses block t * R * n + k * R + m

  block:   0     1     2     3     4     5     6     7     8     9    10    11
  thread:  r0t0  r1t0  r0t0  r1t0  r0t0  r1t0  r0t1  r1t1  r0t1  r1t1  r0t1  r1t1
           |------- group 0 region --------|  |------- group 1 region --------|

  group 0 accesses blocks {0, 1}, {2, 3}, {4, 5};  group 1 accesses {6, 7}, {8, 9}, {10, 11}
```

So with `--mpicoll --strided` the members of a group access one adjacent run of blocks in every
round, which collective buffering merges into one large file access, and the rounds of a group
follow each other without gaps, so every aggregator produces one unbroken sequential stream and the
file system's read-ahead on that node fetches what the same aggregator asks for next. The file as a
whole sees one such stream per group. This differs from plain `--strided`, which interleaves over
all threads so that the threads of one host access adjacent blocks; in a collective call that would
put the members of a group `threads_per_rank` blocks apart. With contiguous ranges the members of a
group access blocks that lie `n` blocks apart in every round (a segmented pattern). `--rand`
gives each thread random offsets within its contiguous range, with the same number of blocks for
every thread.

`--mpicoll` makes the data calls collective within these groups. Collective calls complete only
when every participant of the group has made its call, which has three consequences:

* **Equal block counts.** Every thread of a group has to make exactly one collective call per block
  and the same number of them. elbencho therefore reduces the file size to a multiple of block size
  times the total thread count (`-t` times I/O ranks) when necessary and says so. `--rand` already
  gives every thread the same amount, `--strided` the same number of interleaved blocks.
* **One shared file.** All collective calls of a group have to hit the same file, so collective mode
  takes a single file as benchmark path. Directory mode, a read/write mix (`--rwmixpct`,
  `--rwmixthr`), `--verify`/`--readinline`, a block size mix and `--norandalign` are rejected with
  `--mpicoll`, because they would make the threads of a group issue different call sequences. All
  of them work with independent MPI-IO. `--infloop` works: the threads of a group decide together
  after every repeat whether to start another one (one small collective call per repeat), so
  either all of them repeat or none does.
* **Ending early.** A thread that stops before its blocks are done, because of `--timelimit`,
  ctrl+c or an error, completes its planned number of calls with empty collective calls (zero
  bytes), so that the other threads of its group never wait forever (with `--infloop`, the group
  then stops at the end of the current repeat). Each empty call still costs
  the MPI library's collective synchronization (in the order of 0.1-0.3 ms), so a `--timelimit`
  that cuts a large file short ends with a tail of that many empty calls: 50,000 remaining blocks
  per thread take about ten seconds. The tail is not part of the measured elapsed time or
  throughput. The I/O path itself has no extra communication: the call type is fixed per phase,
  and nothing is exchanged between the participants besides what the MPI library does for the
  collective call.

## Hints

`--mpihints key=value[,key=value...]` sets an `MPI_Info` for every `MPI_File_open`, e.g.
`--mpihints romio_cb_write=enable,striping_unit=1048576`. Which hints have an effect depends on the
MPI-IO implementation (ROMIO in MPICH and its derivatives, OMPIO or ROMIO in Open MPI) and on the
file system; unknown hints are ignored by the library. There is no `--direct` in MPI-IO mode, the
hints are the way to influence such behavior.

## Persistent Services

Service instances can also run persistently, like in the normal distributed mode, and then any host
runs benchmarks against them without mpirun:

```bash
mpirun -np 4 -ppn 1 elbencho --service --mpiio --foreground --port 1611   # one rank per host
elbencho --mpiio --mpicoll --hosts host1,host2,host3,host4 -w -r -t 8 -b 1m -s 32g /mnt/shared/file
elbencho --hosts host1,host2,host3,host4 --quit
```

With `--service` on the command line, all ranks are I/O ranks (no coordinator rank), and the
services pick up the MPI-IO options of each run from the coordinator. The ranks share one command
line, so with a fixed `--port` this needs one rank per host. For several ranks per host, let the
services pick their ports and register themselves in a hosts file on a shared file system, which
the coordinator then uses:

```bash
rm -f /mnt/shared/elbencho-hosts   # the services append to the file, so start empty
mpirun -np 16 -ppn 4 elbencho --service --mpiio --foreground --port 0 --hostsfile /mnt/shared/elbencho-hosts
# wait until the file has one line per service, then run from any host that reaches them:
elbencho --mpiio --mpicoll --hostsfile /mnt/shared/elbencho-hosts -w -r -t 1 -b 1m -s 32g /mnt/shared/file
elbencho --hostsfile /mnt/shared/elbencho-hosts --quit
```

Each service appends `host:port` under a whole-file lock after it started listening. The host is
the one the launcher started the service with (hydra's `PMI_HOSTNAME` or
`MPIR_CVAR_CH3_INTERFACE_HOSTNAME`, Slurm's `SLURMD_NODENAME`, Open MPI's `PMIX_HOSTNAME`), so an
IP address given to mpirun stays an IP address; without such a variable it is the host name. This
works for services in any mode, `--mpiio` is only needed for MPI-IO runs.

## Notes

* Collective calls match across the threads of a group as long as all ranks get the same thread
  count, which the coordinator ensures. Each group's communicator is created on first use and kept
  for the lifetime of the process. Independent I/O uses no group communicator at all.
* The services' console output (per-phase result tables, run notes) is suppressed under mpirun,
  because it would appear once per rank. Errors still reach the coordinator and stderr.
* Ctrl+c in the mpirun terminal ends the run without a result table, because the launcher kills
  the ranks (MPICH's hydra) or signals all of them, which also stops the services. To end a run
  early with a result table, send SIGINT to the coordinator rank alone: its host and pid are in
  the first output line (`kill -INT <pid>` on that host). The coordinator then interrupts the
  services, prints the results, writes the result files and ends all ranks. `--timelimit` is the
  launcher-independent way to end a run early.
* A service rank announces the IPv4 address of its interface that shares a subnet with one of
  rank 0's addresses (preferring a different host over a same-host match, so that a private subnet
  which exists on every node, like docker's, does not get picked), or the address that routes to
  rank 0 otherwise. Only when rank 0 has no global IPv4 address at all does it use the host name.
