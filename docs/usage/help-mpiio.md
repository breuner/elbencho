## elbencho --help-mpiio

> **_NOTE:_**  This page has been auto-generated from built-in help text of the `elbencho` executable.

<pre><code>
I/O through the MPI-IO API, independent or collective.

Usage:
  Launch via mpirun with one rank more than the number of I/O ranks. Rank 0 is the
  coordinator, all other ranks run an elbencho service and do the actual I/O, each
  with "-t" threads:
    $ mpirun -np <N+1> elbencho --mpiio [OPTIONS] PATH
  Without mpirun, this is a standalone run with a single I/O rank.

Each worker thread is a separate MPI-IO participant: the threads with the same index
on all I/O ranks form one group with its own communicator and open the file
collectively, so a run with 4 I/O ranks and "-t 8" has 8 groups of 4 participants.
Only the file data path uses MPI-IO; directory, stat and delete phases stay POSIX.

Basic Options:
  --mpiio               Read and write file contents through 
                        MPI_File_write_at/read_at.
  --mpicoll             Collective I/O through MPI_File_write_at_all/read_at_al
                        l. Requires a single shared file as benchmark path. 
                        Every thread of a collective group makes the same 
                        number of calls, so the file size is reduced to a 
                        multiple of block size times total thread count if 
                        necessary. A "--timelimit" ends the phase after the 
                        limit with empty collective calls for the remaining 
                        blocks, so that no thread waits forever for its group.
  --mpihints arg        Comma-separated key=value hints for MPI_File_open, e.g.
                        "romio_cb_write=enable,striping_unit=1048576".
  -w [ --write ]        Write phase. In file mode the shared file is created if
                        it does not exist.
  -r [ --read ]         Read phase.
  -s [ --size ] arg     Shared file size, or size per file in directory mode. 
                        (Default: 0)
  -b [ --block ] arg    Bytes per MPI-IO call. (Default: 1M)
  -t [ --threads ] arg  Number of threads per I/O rank. (Default: 1)
  --strided             Interleave the blocks of the threads instead of giving 
                        each thread a contiguous range of the shared file, i.e.
                        thread N writes blocks N, N+T, N+2T, ... for T total 
                        threads. With --mpicoll each collective group owns a 
                        contiguous region of the file and only its members 
                        interleave within that region, so each collective call 
                        covers one adjacent run of blocks and a group's calls 
                        follow each other without gaps (see docs/mpiio.md).
  --rand                Random offsets within each thread's range of the shared
                        file.

Examples:
  Shared file, 4 I/O ranks with 8 threads each, independent writes and reads:
    $ mpirun -np 5 elbencho --mpiio -w -r -t 8 -b 1m -s 32g /mnt/shared/file

  The same with collective I/O and interleaved blocks:
    $ mpirun -np 5 elbencho --mpiio --mpicoll --strided -w -r -t 8 \
        -b 1m -s 32g /mnt/shared/file

  One file per thread (file-per-process pattern), 100 files of 1 MiB per thread:
    $ mpirun -np 5 elbencho --mpiio -d -w -r -F -D -t 8 -n 1 -N 100 \
        -s 1m -b 1m /mnt/shared/dir

  Persistent services: start one "--service --mpiio" rank per host via mpirun, then run
  benchmarks against them from any host with "--mpiio --hosts ..." like in distributed mode.

See "--help-large" for the shared file options and "--help-multi" for the
directory mode options, which all apply here as well.
</code></pre>
