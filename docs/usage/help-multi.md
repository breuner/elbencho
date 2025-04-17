## elbencho --help-multi

> **_NOTE:_**  This page has been auto-generated from built-in help text  of the `elbencho` executable.

<pre><code>
Multi-file / multi-directory testing.

Usage: ./elbencho [OPTIONS] DIRECTORY [MORE_DIRECTORIES]

Basic Options:
  -d [ --mkdirs ]       Create directories. (Already existing dirs are not 
                        treated as error.)
  -w [ --write ]        Write files. Create them if they don't exist.
  -r [ --read ]         Read files.
  --stat                Read file status attributes (file size, owner etc).
  -F [ --delfiles ]     Delete files.
  -D [ --deldirs ]      Delete directories.
  -t [ --threads ] arg  Number of I/O worker threads. (Default: 1)
  -n [ --dirs ] arg     Number of directories per I/O worker thread. This can 
                        be 0 to disable creation of any subdirs, in which case 
                        all workers share the given dir. (Default: 1)
  -N [ --files ] arg    Number of files per thread per directory. (Default: 1) 
                        Example: "-t2 -n3 -N4" will use 2x3x4=24 files.
  -s [ --size ] arg     File size. (Default: 0)
  -b [ --block ] arg    Number of bytes to read/write in a single operation. 
                        (Default: 1M)

Frequently Used Options:
  --direct              Use direct IO.
  --iodepth arg         Depth of I/O queue per thread for asynchronous 
                        read/write. Setting this to 2 or higher turns on async 
                        I/O. (Default: 1)
  --lat                 Show minimum, average and maximum latency for 
                        read/write operations and entries. In read and write 
                        phases, entry latency includes file open, read/write 
                        and close.

Miscellaneous Options:
  --zones arg           Comma-separated list of NUMA zones to bind this process
                        to. If multiple zones are given, then worker threads 
                        are bound round-robin to the zones. (Hint: See 'lscpu' 
                        for available NUMA zones.)
  --latpercent          Show latency percentiles.
  --lathisto            Show latency histogram.
  --nodelerr            Ignore not existing files/dirs in deletion phase 
                        instead of treating this as error.

Examples:
  Test 2 threads, each creating 3 directories with 4 1MiB files inside:
    $ elbencho -w -d -t 2 -n 3 -N 4 -s 1m -b 1m /data/testdir

  Same as above with long option names:
    $ elbencho --write --mkdirs --threads 2 --dirs 3 --files 4 --size 1m \
        --block 1m /data/testdir

  Test 2 threads, each reading 4 1MB files from 3 directories in 128KiB blocks:
    $ elbencho -r -t 2 -n 3 -N 4 -s 1m -b 128k /data/testdir

  As above, but also copy data into memory of first 2 GPUs via CUDA:
    $ elbencho -r -t 2 -n 3 -N 4 -s 1m -b 128k \
        --gpuids 0,1 /data/testdir

  As above, but read data into memory of first 2 GPUs via GPUDirect Storage:
    $ elbencho -r -t 2 -n 3 -N 4 -s 1m -b 128k \
        --gpuids 0,1 --gds /data/testdir

  Delete files and directories created by example above:
    $ elbencho -F -D -t 2 -n 3 -N 4 /data/testdir
</code></pre>
