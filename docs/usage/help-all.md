## elbencho --help-all

> **_NOTE:_**  This page has been auto-generated from built-in help text of the `elbencho` executable.

<pre><code>
Overview of all available options.

Usage: ./elbencho [OPTIONS] PATH [MORE_PATHS]

All options in alphabetical order:
  --allelapsed            Show elapsed time to completion of each I/O worker 
                          thread.
  -b [ --block ] arg      Number of bytes to read/write in a single operation. 
                          Each thread needs to keep one block in RAM (or 
                          multiple blocks if "--iodepth" is used), so be 
                          careful with large block sizes. For S3, this defines 
                          the multipart upload size and the ranged read size. 
                          Multipart uploads will automatically be used if 
                          object size is larger than this block size. (Default:
                          1M; supports base2 suffixes, e.g. "128K")
  --backward              Do backwards sequential reads/writes.
  --blockvaralgo arg      Random number algorithm for "--blockvarpct". Values: 
                          "fast" for high speed but weaker randomness; 
                          "balanced" for good balance of speed and randomness; 
                          "strong" for high CPU cost but strong randomness. 
                          When GPUs are given then the CUDA default ("XORWOW") 
                          random generator will be used and this value is 
                          ignored. (Default: fast)
  --blockvarpct arg       Block variance percentage. Defines the percentage of 
                          each block that will be refilled with random data 
                          between writes. This can be used to defeat 
                          compression/deduplication. (Default: 100; Range: 
                          0-100)
  -c [ --configfile ] arg Path to benchmark configuration file. All command 
                          line options starting with double dashes can be used 
                          as "OPTIONNAME=VALUE" in the config file. Multiple 
                          options are newline-separated. Lines starting with 
                          "#" are ignored.
  --clients arg           Comma-separated list of service hosts to use as 
                          clients in netbench mode. (Format: hostname[:port])
  --clientsfile arg       Path to file containing line-separated service hosts 
                          to use as clients in netbench mode. (Format: 
                          hostname[:port])
  --cores arg             Comma-separated list of CPU cores to bind this 
                          process to. If multiple cores are given, then worker 
                          threads are bound round-robin to the cores. The 
                          special value "all" is short for the list of all 
                          available CPU cores. (Hint: See 'lscpu' for available
                          CPU cores.)
  --cpu                   Show CPU utilization in phase stats results.
  --csvfile arg           Path to file for end results in csv format. This way,
                          results can be imported e.g. into MS Excel. If the 
                          file exists, results will be appended. (See also 
                          "--livecsv" for progress results in csv format.)
  --cufile                Use cuFile API for reads/writes to/from GPU memory, 
                          also known as GPUDirect Storage (GDS).
  --cufiledriveropen      Explicitly initialize cuFile lib and open the 
                          nvida-fs driver.
  --cuhostbufreg          Pin host memory buffers and register with CUDA for 
                          faster transfer to/from GPU.
  -D [ --deldirs ]        Delete directories.
  -d [ --mkdirs ]         Create directories. (Already existing dirs are not 
                          treated as error.)
  --direct                Use direct IO (also known as O_DIRECT) to avoid file 
                          contents caching. Note: For network or cluster 
                          filesystems, it depends on the actual filesystem 
                          whether this option is only effective for the 
                          client-side cache or also for the server-side cache. 
                          Also, some filesystems might ignore this completely 
                          or might have a RAID controller cache which operates 
                          independent of this setting.
  --dirsharing            If benchmark path is a directory, all threads create 
                          their files in the same dirs instead of using 
                          different dirs for each thread. In this case, "-n" 
                          defines the total number of dirs for all threads 
                          instead of the number of dirs per thread.
  --dirstats              Show directory completion statistics in file 
                          write/read phase. A directory counts as completed if 
                          all files in the directory have been written/read. 
                          Only effective if benchmark path is a directory.
  --dropcache             Drop linux file system page cache, dentry cache and 
                          inode cache before/after each benchmark phase. 
                          Requires root privileges. This should be used 
                          together with "--sync", because only data on stable 
                          storage can be dropped from cache. Note for 
                          distributed file systems that this only drops caches 
                          on the clients where elbencho runs, but there might 
                          still be cached data on the server.
  --dryrun                Don't run any benchmark phase, just print the number 
                          of expected entries and dataset size per benchmark 
                          phase.
  -F [ --delfiles ]       Delete files.
  --fadv arg              Provide file access hints via fadvise(). This value 
                          is a comma-separated list of the following flags: 
                          seq, rand, willneed, dontneed, noreuse.
  --flock arg             Use POSIX file locks around each file read/write 
                          operation. Possible values: "range" to lock the 
                          specific range of each IO operation, "full" to lock 
                          the entire file for each IO operation.
  --foreground            When running as service, stay in foreground and 
                          connected to console instead of detaching from 
                          console and daemonizing into background.
  --gds                   Use Nvidia GPUDirect Storage API. Enables "--direct",
                          "--cufile", "--gdsbufreg".
  --gdsbufreg             Register GPU buffers for GPUDirect Storage (GDS) when
                          using cuFile API.
  --gpuids arg            Comma-separated list of CUDA GPU IDs to use for 
                          buffer allocation. If no other option for GPU buffers
                          is given then read/write timings will include copy 
                          to/from GPU buffers. GPU IDs will be assigned round 
                          robin to different threads. When this is given in 
                          service mode then the given list will override any 
                          list given by the master, which can be used to bind 
                          specific service instances to specific GPUs. The 
                          special value "all" is short for the list of all 
                          available GPUs. (Hint: CUDA GPU IDs are 0-based; see 
                          'nvidia-smi' for available GPU IDs.)
  --gpuperservice         Assign GPUs round robin to service instances (i.e. 
                          one GPU per service) instead of default round robin 
                          to threads (i.e. multiple GPUs per service, if 
                          multiple given).
  --hosts arg             List of hosts in service mode (separated by comma, 
                          space, or newline) for coordinated benchmark. When 
                          this argument is used, this program instance runs in 
                          master mode to coordinate the given service mode 
                          hosts. The given number of threads, dirs and files is
                          per-service then. (Format: hostname[:port])
  --hostsfile arg         Path to file containing line-separated service hosts 
                          to use for benchmark. Lines starting with "#" will be
                          ignored. (Format: hostname[:port])
  -i [ --iterations ] arg Number of iterations to run the benchmark. (Default: 
                          1)
  --infloop               Let I/O threads run in an infinite loop, i.e. they 
                          restart from the beginning when the reach the end of 
                          the specified workload. Terminate this via ctrl+c or 
                          by using "--timelimit"
  --interrupt             Interrupt current benchmark phase on given service 
                          mode hosts.
  --iodepth arg           Depth of I/O queue per thread for asynchronous I/O. 
                          Setting this to 2 or higher turns on async I/O. 
                          (Default: 1)
  --jsonfile arg          Path to file for end results in json format. If the 
                          file exists, results will be appended. (See also 
                          "--livejson" for progress results in json format.) 
                          (EXPERIMENTAL: Output format can still change.)
  --label arg             Custom label to identify benchmark run in result 
                          files.
  --lat                   Show minimum, average and maximum latency for 
                          read/write operations and entries. In read and write 
                          phases, entry latency includes file open, read/write 
                          and close.
  --lathisto              Show latency histogram.
  --latpercent            Show latency percentiles.
  --latpercent9s arg      Number of decimal nines to show in latency 
                          percentiles. 0 for 99%, 1 for 99.9%, 2 for 99.99% and
                          so on. (Default: 0)
  --limitread arg         Per-thread read limit in bytes per second.
  --limitwrite arg        Per-thread write limit in bytes per second. (In 
                          combination with "--rwmixpct" this defines the limit 
                          for read+write.)
  --live1                 Use brief live statistics format, i.e. a single line 
                          instead of full screen stats. The line gets updated 
                          in-place.
  --live1n                Use brief live statistics format, i.e. a single line 
                          instead of full screen stats. A new line is written 
                          to stderr for each update.
  --livecsv arg           Path to file for live progress results in csv format.
                          If the file exists, results will be appended. This 
                          must not be the same file that is given as 
                          "--csvfile". The special value "stdout" as filename 
                          will make the CSV get printed to stdout (and all 
                          other console prints will get sent to stderr).
  --livecsvex             Use extended live results csv file. By default, only 
                          aggregate results of all worker threads will be 
                          added. This option also adds results of individual 
                          threads in standalone mode or results of individual 
                          services in distributed mode.
  --liveint arg           Update interval for console and csv file live 
                          statistics in milliseconds. (Default: 2000)
  --log arg               Log level. (Default: 0; Verbose: 1; Debug: 2)
  --madv arg              When using mmap, provide access hints via madvise(). 
                          This value is a comma-separated list of the following
                          flags: seq, rand, willneed, dontneed, hugepage, 
                          nohugepage.
  --mmap                  Do file IO through memory mapping. Mmap writes cannot
                          extend a file beyond its current size; if you try 
                          this, you will cause a Bus Error (SIGBUS). Thus, you 
                          typically use mmap writes together with 
                          "--trunctosize" or "--preallocfile". For random read 
                          tests, consider using direct IO and adding an madvise
                          for random access to disable prefetching. But caching
                          is in the nature of how mmap IO works, so direct IO 
                          won't disable caching in this case. Note also that 
                          memory maps count towards the total virtual address 
                          limit of a process and platform. A typical limit is 
                          128TB, seen as 48 bits virtual address size in 
                          "/proc/cpuinfo".
  -N [ --files ] arg      Number of files per thread per directory. (Default: 
                          1) Example: "-t2 -n3 -N4" will use 2x3x4=24 files.
  -n [ --dirs ] arg       Number of directories per thread. This can be 0 to 
                          disable creation of any subdirs, in which case all 
                          workers share the given dir. (Default: 1)
  --netbench              Run network benchmarking. To simulate the typical 
                          storage access request/response pattern, each client 
                          thread will send blocksized chunks ("-b") to one of 
                          the servers and wait for a reponse of length 
                          "--respsize" bytes before transmitting the next 
                          block. Client threads will get connected round-robin 
                          to the given servers. Blocksize larger than response 
                          size simulates writes from clients to servers, 
                          blocksize smaller than response size simulates reads.
                          Client threads use filesize as limit for amount of 
                          data to send. See "--servers" & "--clients" for how 
                          to define servers and clients. The used network port 
                          for data transfer connections will be "--port" plus 
                          1000. (Netbench mode defaults to zero block 
                          variance.)
  --netdevs arg           Comma-separated list of network device names (e.g. 
                          "eth0") for round-robin binding of outgoing 
                          (client-side) connections in network benchmark mode. 
                          Requires root privileges.
  --no0usecerr            Do not warn if worker thread completion time is less 
                          than 1 microsecond.
  --nocsvlabels           Do not print headline with labels to csv file.
  --nodelerr              Ignore not existing files/dirs in deletion phase 
                          instead of treating this as error.
  --nodiocheck            Don't check direct IO alignment and minimum block 
                          size. Many platforms require IOs to be aligned to 
                          certain minimum block sizes for direct IO.
  --nofdsharing           If benchmark path is a file or block device, let each
                          worker thread open the givenfile/bdev separately 
                          instead of sharing the same file descriptor among all
                          threads.
  --nolive                Disable live statistics on console.
  --nopathexp             Disable expansion of number lists and ranges in 
                          square brackets for given paths.
  --norandalign           Do not align offsets to multiples of given block size
                          for random IO.
  --nosvcshare            Benchmark paths are not shared between service 
                          instances. Thus, each service instance will work on 
                          its own full dataset instead of a fraction of the 
                          data set.
  --numhosts arg          Number of hosts to use from given hosts list or hosts
                          file. (Default: use all given hosts)
  --opslog arg            Absolute path to logfile for all I/O operations 
                          (open, read, ...). In service mode, the service 
                          instances will log their operations locally to the 
                          given path. Log is in JSON format. (Default: 
                          disabled)
  --opsloglock            Use file locking to synchronize appends to 
                          "--opslog".
  --phasedelay arg        Delay between different benchmark phases in seconds. 
                          (Default: 0)
  --port arg              TCP port of background service. (Default: 1611)
  --preallocfile          Preallocate file disk space in a write phase via 
                          posix_fallocate().
  --quit                  Quit services on given service mode hosts.
  -r [ --read ]           Read files.
  --rand                  Read/write at random offsets.
  --randalgo arg          Random number algorithm for "--rand". Values: "fast" 
                          for high speed but weaker randomness; 
                          "balanced_single" for good balance of speed and 
                          randomness; "strong" for high CPU cost but strong 
                          randomness. (Default: a special algo for maximum 
                          single pass block coverage in write phase for aligned
                          IO and "balanced_single" for reads and unaligned IO)
  --randamount arg        Number of bytes to write/read when using random 
                          offsets. Only effective when benchmark path is a file
                          or block device. (Default: Set to file size)
  --rankoffset arg        Rank offset for worker threads. (Default: 0)
  --readinline            When benchmark path is a directory, read files 
                          immediately after write while they are still open.
  --recvbuf arg           In netbench mode, this sets the receive buffer size 
                          of sockets in bytes. (Supports base2 suffixes, e.g. 
                          "2M")
  --respsize arg          Netbench mode server response size in bytes. Servers 
                          will send this amount of data as response to each 
                          received block from a client. (Default: 1; supports 
                          base2 suffixes, e.g. "2M")
  --resfile arg           Path to file for human-readable results, similar to 
                          console output. If the file exists, new results will 
                          be appended.
  --rotatehosts arg       Number by which to rotate hosts between phases to 
                          avoid caching effects. (Default: 0)
  --rwmixpct arg          Percentage of blocks that should be read in a write 
                          phase. (Default: 0; Max: 100)
  --rwmixthr arg          Number of threads that should do reads in a write 
                          phase for mixed read/write. The number given here 
                          defines the subset of readers out of the total number
                          of threads per host ("-t"). This assumes that the 
                          full dataset has been precreated via normal write. In
                          S3 mode, this only works in combination with "-n" and
                          "-N". Read/write rate balance can be defined via 
                          "--rwmixthrpct".
  --rwmixthrpct arg       Percentage of reads in a write phase when using 
                          "--rwmixthr". This implies frequent sleep and wakeup 
                          of reader and writer threads to maintain the given 
                          balance ratio and thus might impact the achievable 
                          maximum performance of a host in some scenarios. This
                          needs to be used together with "--infloop" to prevent
                          starvation of the I/O threads that run at the lower 
                          rate. Consider adding "--timelimit") for termination.
                          (Value range: 1..99)
  -s [ --size ] arg       File size. (Default: 0; supports base2 suffixes, e.g.
                          "2M")
  --s3aclget              Get S3 object ACLs.
  --s3aclgrantee arg      S3 object ACL grantee. This can be used with special 
                          values to set a canned ACL, in which case the grantee
                          type and permissions arguments will be ignored: 
                          private, public-read, public-read-write, 
                          authenticated-read
  --s3aclgtype arg        S3 object ACL grantee type. Possible values: id, 
                          email, uri, group
  --s3aclgrants arg       S3 object ACL grantee permissions. Comma-separated 
                          list of these values: none, full, read, write, racp, 
                          wacp
  --s3aclput              Put S3 object ACLs. This requires definition of 
                          grantee, grantee type and permissions.
  --s3aclputinl           Set S3 object ACL inlined in object upload. This 
                          requires definition of grantee and permissions. 
                          Grantee type is specified as part of the grantee 
                          name: "emailAddress=example@myorg.org" or "id=123" or
                          "uri=...".
  --s3aclverify           Verify S3 object and bucket ACLs based on given 
                          grantee, grantee type and permissions. This only 
                          effective in the corresponding get object or bucket 
                          ACL phase.
  --s3baclget             Get S3 bucket ACLs.
  --s3baclput             Put S3 bucket ACLs. This requires definition of 
                          grantee, grantee type and permissions.
  --s3btag                Activate bucket tagging operations.
  --s3btagverify          Verify the correctness of S3 bucket tagging results. 
                          (Requires "--s3btag")
  --s3bversion            Activate bucket versioning operations.
  --s3bversionverify      Verify the correctness of S3 bucket versioning 
                          settings. (Requires "--s3bversion")
  --s3checksumalgo arg    S3 checksum algorithm to use (CRC32, CRC32C, SHA1, 
                          SHA256). This sets the x-amz-sdk-checksum-algorithm 
                          header for S3 operations. (EXPERIMENTAL)
  --s3endpoints arg       Comma-separated list of S3 endpoints. When this 
                          argument is used, the given benchmark paths are used 
                          as bucket names. Also see "--s3key" & "--s3secret". 
                          (Format: [http(s)://]hostname[:port])
  --s3fastget             Send downloaded objects directly to /dev/null instead
                          of a memory buffer. This option is incompatible with 
                          any buffer post-processing options like data 
                          verification or GPU data transfer.
  --s3fastput             Reduce CPU overhead for uploads. Enables "--s3sign=2 
                          (never)", "--s3nocompress".
  --s3ignoreerrors        Ignore any S3 upload/download errors. Useful for 
                          stress-testing.
  --s3key arg             S3 access key. (This can also be set via the 
                          AWS_ACCESS_KEY_ID env variable.)
  --s3listobj arg         List objects. The given number is the maximum number 
                          of objects to retrieve. Use "--s3objprefix" to start 
                          listing with the given prefix. (Multiple threads will
                          only be effective if multiple buckets are given.)
  --s3listobjpar          List objects in parallel. Requires a dataset created 
                          via "-n" and "-N" options and parallelizes by using 
                          different S3 listing prefixes for each thread.
  --s3listverify          Verify the correctness of S3 server object listing 
                          results in combination with "--s3listobjpar". This 
                          requires the dataset to be created with the same 
                          values for "-n" and "-N".
  --s3log arg             Log level of AWS S3 SDK. See "--s3logprefix" for 
                          filename. (Default: 0=disabled; Max: 6)
  --s3logprefix arg       Path and filename prefix of AWS S3 SDK log file. 
                          "DATE.log" will get appended to the given filename. 
                          (Default: "aws_sdk_" in current working directory)
  --s3multidel arg        Delete multiple objects in a single DeleteObjects 
                          request. This loops on retrieving a chunk of objects 
                          from a listing request and then deleting the 
                          retrieved set of objects in a single request. This 
                          makes no assumption about the retrieved object names 
                          and deletes arbitrary object names in the given 
                          bucket(s). The given number is the maximum number of 
                          objects to retrieve and delete in a single request. 
                          1000 is a typical max value. Use "--s3objprefix" to 
                          list/delete only objects with the given prefix. 
                          (Multiple threads will only be effecive if multiple 
                          buckets are given.)
  --s3multiignore404      Ignore 404 HTTP error code for multipart upload 
                          completions, which can happen if the 
                          CompleteMultipartUpload request has to be retried, 
                          e.g. because of a connection failure. Depending on 
                          how the S3 backend handles this, it might return a 
                          404 because the upload was already completed 
                          beforehand. Enabling this will ignore 404 HTTP errors
                          in CompleteMultiPartUpload responses.
  --s3nocompress          Disable S3 request compression.
  --s3nompcheck           Don't check for S3 multi-part uploads exceeding 
                          10,000 parts.
  --s3nompucompl          Don't send completion message after uploading all 
                          parts of a multi-part upload.
  --s3objprefix arg       S3 object prefix. This will be prepended to all 
                          object names when the benchmark path is a bucket. (A 
                          sequence of 3 to 16 "%%%" chars will be replaced by a
                          random hex string of the same length.)
  --s3olockcfg            Activate object lock configuration creation.
  --s3olockcfgverify      Verify the correctness of object lock configurations.
  --s3otag                Activate S3 object tagging.
  --s3otagverify          Verify the correctness of created S3 object tags.
  --s3randobj             Read at random offsets and randomly select a new 
                          object for each S3 block read. Only effective in read
                          phase and in combination with "-n" & "-N". Read limit
                          for all threads is defined by "--randamount".
  --s3sse                 Server-side encryption of S3 objects using SSE-S3. 
                          (EXPERIMENTAL)
  --s3sseckey arg         Base64-encoded AES-256 encryption key for S3 SSE-C.
  --s3ssekmskey arg       Key for S3 SSE-KMS. (EXPERIMENTAL)
  --s3region arg          S3 region.
  --s3secret arg          S3 access secret. (This can also be set via the 
                          AWS_SECRET_ACCESS_KEY env variable.)
  --s3sessiontoken arg    S3 session token. (Optional. This can also be set via
                          the AWS_SESSION_TOKEN env variable.)
  --s3sign arg            S3 payload signing policy. 0=RequestDependent, 
                          1=Always, 2=Never. Changing this to 'Never' has no 
                          effect with current S3 SDK as described in Github 
                          issue 3297. (Default: 0)
  --s3statdirs            Do bucket Stats.
  --sendbuf arg           In netbench mode, this sets the send buffer size of 
                          sockets in bytes. (Supports base2 suffixes, e.g. 
                          "2M")
  --servers arg           Comma-separated list of service hosts to use as 
                          servers in netbench mode. (Format: hostname[:port])
  --serversfile arg       Path to file containing line-separated service hosts 
                          to use as servers in netbench mode. (Format: 
                          hostname[:port])
  --service               Run as service for distributed mode, waiting for 
                          requests from master.
  --sharesize arg         In custom tree mode, this defines the file size as of
                          which files are no longer exclusively assigned to a 
                          thread. This means multiple threads read/write 
                          different parts of files that exceed the given size. 
                          (Default: 0, which means 32 x blocksize)
  --stat                  Run file stat benchmark phase.
  --statinline            When benchmark path is a directory, stat files 
                          immediately after open in a write or read phase.
  --start arg             Start time of first benchmark in UTC seconds since 
                          the epoch. Intended to synchronize start of 
                          benchmarks on different hosts, assuming they use 
                          synchronized clocks. (Hint: Try 'date +%s' to get 
                          seconds since the epoch.)
  --strided               Use strided read/write access pattern. Only available
                          if given benchmark paths are files or block devices.
  --svcelapsed            Show elapsed time to completion of each service 
                          instance ordered by slowest thread.
  --svcpwfile arg         Path to a text file containing a single line of text 
                          as shared secret between service instances and 
                          master. This is to prevent unauthorized requests to 
                          service instances.
  --svcupint arg          Update retrieval interval for service hosts in 
                          milliseconds. (Default: 500)
  --sync                  Sync Linux kernel page cache to stable storage 
                          before/after each phase.
  -t [ --threads ] arg    Number of I/O worker threads. (Default: 1)
  --timelimit arg         Time limit in seconds for each benchmark phase. If 
                          the limit is exceeded for a phase then no further 
                          phases will run. (Default: 0 for disabled)
  --treefile arg          The path to a treefile containing a list of dirs and 
                          filenames to use. This is called "custom tree mode" 
                          and enables testing with mixed file sizes. The 
                          general benchmark path needs to be a directory. Paths
                          contained in treefile are used relative to the 
                          general benchmark directory. The elbencho-scan-path 
                          tool is a way to create a treefile based on an 
                          existing data set. Otherwise, options are similar to 
                          "--help-multi" with the exception of file size and 
                          number of dirs/files, as these are defined in the 
                          treefile. (Note: The file list will be split across 
                          worker threads, but dir create/delete is not fully 
                          parallel, so don't use this for dir create/delete 
                          performance testing.)
  --treerand              In custom tree mode: Randomize file order. Default is
                          order by file size.
  --treeroundup arg       When loading a treefile, round up all contained file 
                          sizes to a multiple of the given size. This is useful
                          for "--direct" with its alignment requirements on 
                          many platforms. (Default: 0 for disabled)
  --treescan arg          Path to scan on startup. The discovered entries will 
                          be stored in a treefile and the resulting treefile 
                          will be used for the run. Path can be a directory 
                          ("/mnt/mystorage") or an S3 bucket with optional 
                          prefix ("s3://mybucket/myprefix"). The location of 
                          the generated treefile can be changed from the 
                          default in "/var/tmp" by setting "--treefile". The 
                          scan runs single-threaded. In case of a distributed 
                          run with services, the master instance (i.e. the host
                          from which the test gets submitted) will run the 
                          scan. Only regular files will be used, symlinks and 
                          other special files will be ignored. S3 prefix from 
                          scan will not be stored in the treefile, so use 
                          "--s3objprefix" toset/change prefix for benchmark 
                          runs.
  --trunc                 Truncate files to 0 size when opening for writing.
  --trunctosize           Truncate files to given "--size" via ftruncate() when
                          opening for writing. If the file previously was 
                          larger then the remainder is discarded.
  --verify arg            Enable data integrity check. Writes sum of given 
                          64bit salt plus current 64bit offset as file or block
                          device content, which can afterwards be verified in a
                          read phase using the same salt (e.g. "1"). Different 
                          salt values can be used to ensure different contents 
                          when running multiple consecutive write and read 
                          verifications. (Default: 0 for disabled)
  --verifydirect          Verify data integrity by reading each block directly 
                          after writing. Use together with "--verify", 
                          "--write".
  --version               Show version and included optional build features.
  -w [ --write ]          Write files. Create them if they don't exist.
  --zones arg             Comma-separated list of NUMA zones to bind this 
                          process to. If multiple zones are given, then worker 
                          threads are bound round-robin to the zones. The 
                          special value "all" is short for the list of all 
                          available NUMA zones. (Hint: See 'lscpu' for 
                          available NUMA zones.)

</code></pre>
