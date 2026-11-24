# MPI-IO mode: developer notes

Design notes for the code in this directory and the MPI launcher in
`source/toolkits/mpi/`. The user-level description (launching via MPI, collective I/O, persistent
services) is in [docs/mpi.md](../../../docs/mpi.md); the conventions in the repo root
[AGENTS.md](../../../AGENTS.md) apply here as well. Everything here is built only with
`MPI_SUPPORT=1`.

## Files

- `MpiIoProgArgs`: the MPI-IO options (definition, defaults, checks incl. the file size
  reduction to equal block counts for collective I/O, `--help-mpiio`, service transfer); owned by
  `ProgArgs` as `mpiIoArgs`.
- `MpiIoMode`: the worker-side code, a friend member of `LocalWorker` (`LocalWorker::mpiIo`).
- `source/toolkits/mpi/MpiLauncher`: mode-independent mapping of MPI ranks to coordinator and
  service roles for `--mpilaunch`; `Main.cpp` calls `MpiLauncher::init()` before the options are
  parsed and holds a `MpiLauncher::Guard` that finalizes MPI on every exit path.

## Data path

Only the file data path goes through MPI. `MpiIoMode` opens the files (`MPI_File_open`,
collectively on the thread's communicator in file mode, `MPI_COMM_SELF` in dir mode) and
provides `readAt()`/`writeAt()` behind `LocalWorker`'s positional read/write function pointers,
with the independent or collective (`_at_all`) MPI call chosen once per phase. The POSIX code
keeps doing the shared file preparation (`prepareBenchPathFDsVec()`, which treats MPI-IO like
POSIX via `getIsPosixFsMode()`), the dir/stat/delete phases and the offset generation.

Every worker thread is one MPI-IO participant: thread index t of all I/O ranks shares the t-th
`MPI_Comm_dup` of the I/O ranks' communicator. That is a grow-only process-wide cache, never
freed, so the collective dups stay matched across runs and errors.

Collective phases need equal call counts, so `--mpicoll` requires a single shared file and a
fixed block size, and a thread that ends early (time limit, error) completes its planned call
count with zero-byte collective calls in `finishCollectivePhase()`, called from `finishPhase()`
and from the error path of `run()`, before the collective close. There is no extra communication
in the I/O path.

## Launcher

`MpiLauncher` maps ranks to roles when `--mpilaunch` is on the command line under `mpirun`
(implied by `--mpiio` with more than one rank): rank 0 becomes the coordinator (it broadcasts its
IPv4 addresses, each service announces the own address that shares a subnet with one of them or
routes to it, and the gathered `addr:port` list is appended as `--hosts`), all other ranks become
`--service --foreground --port 0` instances with stdout silenced and result file paths dropped,
which announce their port from the HTTP backends' listening point and stop when rank 0 reaches
the final barrier (a polled `MPI_Ibarrier`, so no rank spins in MPI during the run). A single
rank runs standalone, `--service` ranks are persistent services for a separately run coordinator
(any mode; MPI initialized for MPI-IO runs), and `--netbench` is rejected under the launcher
because its services would have to be split into servers and clients. Either option initializes
MPI before the options are parsed, so both must be on the command line.

A service with `--port 0` names its upload dir and daemon log after the actual port
(`setServicePort()`) or its pid, so several such services on one host do not share them.
