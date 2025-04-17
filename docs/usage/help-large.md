## elbencho --help-large

> **_NOTE:_**  This page has been auto-generated from built-in help text  of the `elbencho` executable.

<pre><code>
Block device & large shared file testing.

Usage: ./elbencho [OPTIONS] PATH [MORE_PATHS]

Basic Options:
  -w [ --write ]        Write to given block device(s) or file(s).
  -r [ --read ]         Read from given block device(s) or file(s).
  -s [ --size ] arg     Block device or file size to use. (Default: 0)
  -b [ --block ] arg    Number of bytes to read/write in a single operation. 
                        (Default: 1M)
  -t [ --threads ] arg  Number of I/O worker threads. (Default: 1)

Frequently Used Options:
  --direct              Use direct IO to avoid buffering/caching.
  --iodepth arg         Depth of I/O queue per thread for asynchronous 
                        read/write. Setting this to 2 or higher turns on async 
                        I/O. (Default: 1)
  --rand                Read/write at random offsets.
  --randamount arg      Number of bytes to write/read when using random 
                        offsets. (Default: Set to aggregate file size)
  --norandalign         Do not align offsets to block size for random IO.
  --lat                 Show minimum, average and maximum latency for 
                        read/write operations.

Miscellaneous Options:
  --zones arg           Comma-separated list of NUMA zones to bind this process
                        to. If multiple zones are given, then worker threads 
                        are bound round-robin to the zones. (Hint: See 'lscpu' 
                        for available NUMA zones.)
  --latpercent          Show latency percentiles.
  --lathisto            Show latency histogram.
  --allelapsed          Show elapsed time to completion of each I/O worker 
                        thread.

Examples:
  Sequentially write 4 large files and test random read IOPS for max 20 seconds:
    $ elbencho -w -b 4M -t 16 --direct -s 20g /mnt/myfs/file{1..4}
    $ elbencho -r -b 4k -t 16 --iodepth 16 --direct --rand --timelimit 20 \
        /mnt/myfs/file{1..4}

  Test 4KiB multi-threaded write IOPS of devices /dev/nvme0n1 & /dev/nvme1n1:
    $ elbencho -w -b 4K -t 16 --iodepth 16 --direct --rand \
        /dev/nvme0n1 /dev/nvme1n1

  Test 4KiB block random read latency of device /dev/nvme0n1:
    $ elbencho -r -b 4K --lat --direct --rand /dev/nvme0n1

  Stream data from large file into memory of first 2 GPUs via CUDA:
    $ elbencho -r -b 1M -t 8 --gpuids 0,1 \
        /mnt/myfs/file1

  Stream data from large file into memory of first 2 GPUs via GPUDirect Storage:
    $ elbencho -r -b 1M -t 8 --gpuids 0,1 --gds \
        /mnt/myfs/file1

</code></pre>
