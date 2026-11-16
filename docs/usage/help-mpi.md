## elbencho --help-mpi

> **_NOTE:_**  This page has been auto-generated from built-in help text of the `elbencho` executable.

<pre><code>
Launch via an MPI launcher (any benchmark mode) and I/O through the MPI-IO API.

Usage:
  Launch via mpirun/srun with one rank more than the number of I/O ranks:
    $ mpirun -np <N+1> elbencho --mpilaunch [OPTIONS] PATH
  Rank 0 is the coordinator, all other ranks run an elbencho service and do the
  I/O with "-t" threads each, like the hosts of a distributed run. This works in
  any benchmark mode (POSIX, S3, ...). "--mpiio" selects the MPI-IO API for the
  file data and implies "--mpilaunch" under a launcher; without a launcher it
  is a standalone run. Both options have to be on the command line, not in a
  config file.

Basic Options:
  --mpilaunch           Rank 0 coordinates, all other ranks are service 
                        instances.
  --mpiio               Read and write file contents through 
                        MPI_File_write_at/read_at. Each worker thread is a 
                        separate MPI-IO participant; directory, stat and delete
                        phases stay POSIX.
  --mpicoll             Collective I/O through MPI_File_write_at_all/read_at_al
                        l. The threads with the same index on all I/O ranks 
                        form one collective group on its own communicator. 
                        Requires a single shared file; the file size is reduced
                        to a multiple of block size times total thread count if
                        necessary, so that every thread makes the same number 
                        of calls. A "--timelimit" ends the phase with empty 
                        collective calls for the remaining blocks.
  --mpihints arg        Comma-separated key=value hints for MPI_File_open, e.g.
                        "romio_cb_write=enable,striping_unit=1048576".
  --strided             Interleave the blocks of the threads instead of 
                        contiguous ranges. With --mpicoll each collective group
                        owns a contiguous region of the file and only its 
                        members interleave within that region, so each 
                        collective call covers one adjacent run of blocks (see 
                        docs/mpi.md).

Examples:
  POSIX shared file, 4 I/O ranks with 8 threads each:
    $ mpirun -np 5 elbencho --mpilaunch -w -r -t 8 -b 1m -s 32g /mnt/shared/file

  The same through MPI-IO with collective calls and interleaved blocks:
    $ mpirun -np 5 elbencho --mpiio --mpicoll --strided -w -r -t 8 \
        -b 1m -s 32g /mnt/shared/file

  Persistent services: start "--service --mpilaunch --port 0 --hostsfile FILE"
  ranks via mpirun, then run benchmarks against them from any host with
  "--hostsfile FILE".

See docs/mpi.md for details.
</code></pre>
