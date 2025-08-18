# Changelog of elbencho

## v3.0.32 (work in progress)

### New Features & Enhancements
* Added support for S3 async PUT/GET via `--iodepth`.
* Experimental: Added support for macOS.

### General Changes
* Updated S3 to latest AWS SDK CPP v1.11.628.

### Contributors
* Thanks to Christopher Koay for helpful comments and suggestions.

## v3.0.31 (July 20, 2025)

### Fixes
* Added missing calculation of checksum for S3 uploads with custom checksum algorithm via `--s3chksumalgo`.
* Added check to prevent infinite loop usage (`--infloop`) in S3 mode with shared object uploads.

### Contributors
* Thanks to Chuck Cancilla for helpful comments and suggestions.

## v3.0.29 (July 5, 2025)

### New Features & Enhancements
* Added support for temporary credentials using AWS Session Tokens (See `--s3sessiontoken`.)
* Experimental: Added support for S3 checksum algorithm selection. (See `--s3checksumalgo`.)
* Random writes now use a new LCG-based algorithm by default to achieve maximum coverage of block offsets in a single pass. This is a much improved version in comparison to the previous LCG algo that was introduced in v3.0.19 and got removed in v3.0.27 because of corner cases that lead to sequences not being random enough. (`--randalgo balanced_single` can be used to select the old random number generator from before v3.0.19, which is still the default for read offsets.)

### General Changes
* Updated S3 to latest AWS SDK CPP v1.11.580.
* Moved libaio init code to not be called for each new file, but rather only once per thread during preparation phase. This avoids init overhead when using `--iopdepth` in tests with lots of small files.
* Removed "--s3nomd5" option. Not setting MD5 checksum is the default now with the latest AWS SDK CPP, which always signs requests as described here: https://github.com/aws/aws-sdk-cpp/blob/main/docs/MD5ChecksumFallback.md

### Fixes
* Fixed override of S3 credentials with given credential parameters if a AWS profile exists in user home dir.

### Contributors
* Thanks to Avi Drabkin, Michael Shustin for contributions, helpful comments and suggestions.

## v3.0.27 (May 6, 2025)

### New Features & Enhancements
* Added support for S3 server-side encryption using SSE-S3, SSE-C, SSE-KMS. (See `--s3sse`, `--s3sseckey`, `--s3ssekmskey`.)
* Added support for json results output. (See `--jsonfile`.)
* Added support for rate limiting/balancing between readers and writers when using `--rwmixthr`. (See `--rwmixthrpct`.)

### General Changes
* Updated version of embedded HTTP lib to master for IPv6 address support. (RHEL 7.x and derivatives continue to use older version without IPv6 address support for compatibility.)
* Disabled automatic selection of the new full file offsets coverage linear congruential random algo from v3.0.19, because the access pattern was not random enough in some cases.
* Moved markdown docs into new `docs/` dir.
* Added markdown pages based on built-in usage info. (See `docs/usage/`).
* Changed csv results column "ISO date" from phase end date to phase start date for consistency with json output and results text file.
* For read and write mix workloads, console output now also contains lines for "total" (read + write) throughput.

### Fixes
* Modified square brackets parser to enable connection to S3 servers via IPv6 address.
* Patched AWS SDK CPP to not fail DNS name validation for IPv6 address with square brackets. Should later get resolved upstream here: https://github.com/aws/aws-sdk-cpp/issues/3244
* Modified parser for elbencho service instance IP addresses to work with IPv6 addresses in square brackets.

### Contributors
* Thanks to Conor Elrick, Michael Shustin, Sebastian Thorarensen, Ameena Suhani, Janmejay Singh, Paul Hargreaves, Brian Radwanski, Maroun Issa, Dan Reichhardt, Jan Heichler for contributions, helpful comments and suggestions.

## v3.0.25 (Jan 24, 2025)

### General Changes
* S3 secret key will not get printed to result files.

### Fixes
* Fixed `--s3logprefix` not working correctly.

### Contributors
* Thanks to Michael Shustin and Github user russki for contributions, helpful comments and suggestions.

## v3.0.23 (Jan 11, 2025)

### General Changes
* Updated Alpine Linux version for static builds to latest v3.21.

### Fixes
* Fixed S3 multi-delete (`--s3multidel`) not deleting in distributed mode. (Regression from v3.0.17.)
* S3 endpoint was not shown in some error messages.

### Contributors
* Thanks to Wong Tran for helpful comments and suggestions.

## v3.0.21 (Jan 05, 2025)

### New Features & Enhancements
* New option to use file locks around each read/write. (See `--flock`.)
* New option for strided file access. (See `--strided`.)
* New S3 option to skip completion message for multi-part uploads. (See `--s3nompucompl`.)

### General Changes
* Create only a shallow clone of AWS SDK to reduce time to download.
* Added Dockerfile for Ubuntu 24.04.
* Removed Dockerfile for outdated Nvidia Magnum IO container. (Ubuntu CUDA multiarch container provides modern GDS support.)
* Updated mimalloc lib for static builds to latest v2.1.9.
* S3 credentials from environment variables (`AWS_ACCESS_KEY_ID` & `AWS_SECRET_ACCESS_KEY`) now also get forwarded to service instances by the master.

### Fixes
* Fixed compile error on platforms with CUDA, but no cuFile (GDS) support.

### Contributors
* Thanks to Scott Howard, Casey Peel and Github users runiq, russki for contributions, helpful comments and suggestions.

## v3.0.19 (Sep 02, 2024)

### New Features & Enhancements
* New option to scan directory or bucket on startup and use result as custom treefile. (See `--treescan`.)
* New build option to use S3 CRT client of AWS SDK, which uses AWS libs instead of external libs like libcurl. (See `S3_AWSCRT` in `make help`.)
* S3 credentials can now also be provided via environment variables instead of config parameters. (`AWS_ACCESS_KEY_ID` & `AWS_SECRET_ACCESS_KEY`.)
* Random writes now use a new algorithm by default to achieve maximum coverage of file blocks. (Typical random number generators only cover around 70% of file blocks in a single write pass, which is less suitable for file reads after a single random write pass due to the resulting amount of holes.)
* New option to have live statistics in CSV format be printed to console. (See `--livecsv`.)

### General Changes
* Option `--s3objprefix` is now also effective when `--treefile` (and the new `--treescan`) is used.
* Removed unused `--s3transman` option to reduce AWS SDK build options and build time.
* AWS SDK build process now tries to recover if it turns out that a previous build did not complete successfully.
* Stonewall will no longer get triggered by workers without any work assignments in custom tree mode (e.g. because of very small dataset).
* Random IOs are aligned by default now. Corresponding option ` --randalign` has been removed and new option `--norandalign` has been added.
* Live CSV now has a new column for ISO date.

### Fixes
* Fixed a segmentation fault when attempting to validate an empty S3 tag set. (Will throw an appropriate error instead.)

### Contributors
Thanks to Alon Horev and Phil Canman for helpful comments and suggestions.

## v3.0.17 (July 31, 2024)

### New Features & Enhancements
* Added option to ignore 404 errors in MultipartUploadComplete responses. (See `--s3multiignore404`.)

### General Changes
* Use latest Alpine Linux 3.x for Alpine-based docker containers instead of always 3.14.
* Updated CentOS 7 dockerfiles to vault repo due to EOL.
* Use number of parallel jobs from makefile environment variable for builds of external software in `prepare-external.sh`.
* Reseed golden prime based random number generator after 256KB at the latest with a random number generator from a stronger generator.
* Rotate through multiple golden primes for fast random number generator.
* Add hint about initial sequential write or `--trunctosize` in case of short reads.

### Fixes
* Fixed retry mechanism for S3 object download in case server is temporarily unavailable.

### Contributors
Thanks to Chuck Cancilla for helpful comments and suggestions.

## v3.0.15 (June 23, 2024)

### General Changes
* Updated S3 to latest AWS SDK CPP v1.11.335.

### Fixes
* Fixed slight misalignment of latency results column console output. (Regression from v3.0.9)
* Fixed putting S3 ACLs for types other than canned ACLs.

## v3.0.13 (June 16, 2024)

### Fixes
* Fixed memory management for various S3 requests. (Regression from v3.0.9)

## v3.0.11 (June 11, 2024)

### New Features & Enhancements
* Added options to reduce CPU overhead for S3 uploads. (See "--s3fastput", "--s3nocompress", "--s3nomd5".)

### General Changes
* Disabled curl's "Expect: 100-Continue" header for S3 requests to reduce round-trips.
* Updated mimalloc memory allocation library to latest version 2.1.7.

## v3.0.9 (June 04, 2024)

### New Features & Enhancements
* Added options to read and stat files immediately after creation while they are still open. (See "--readinline" and "--statinline".)
* Added option to sleep between test phases. (See "--phasedelay".)
* Added option to rotate hosts list for service instances between phases to avoid caching effects. (See "--rotatehosts".)
* Added support for S3 object and bucket ACL PUT/GET. (See "--s3aclput", "--s3aclputinl", "--s3baclput".)
* Added new ops log to log all IO operations (open, read, ...). (See "--opslog".)
* Added support for shared secret to protect services from unauthorized requests. (See "--svcpwfile".)
* Added new ops for S3 bucket and object tagging and object locking. (See "--s3btag", "--s3otag", "--s3olockcfg"). [STILL WORK-IN-PROGRESS]

### General Changes
* Timestamps in csv files now include milliseconds for higher precision.
* Updated S3 to latest AWS SDK CPP v1.11.319.
* Added new script in `build_helpers/chroot` to build static executable via Alpine chroot.

### Contributors
Thanks to Casey Peel, Michael Shustin, Erez Horev and Github user russki for code contributions. Thanks to Andy Black, Jean-Baptiste Denis and Github user mhanafi1970 for helpful comments and suggestions.

## v3.0.7 (March 21, 2024)

### New Features & Enhancements
* Added new multi-arch (ARM64 & x86_64) docker container with CUDA/GDS support.
* If an S3 object prefix contains a sequence of three or more '%' chars, this sequence will now get replaced by a random hex string of the same length.
* New option to specify a custom path and prefix for AWS S3 SDK log file: "--s3logprefix".

### Contributors
* Thanks to Phil Canman, Erez Binia, Ohad Shamir and Erez Horev for helpful comments and suggestions.

## v3.0.5 (Jan 05, 2024)

### New Features & Enhancements
* Square brackets can now be used to define number lists and ranges in paths, host lists, S3 endpoints.
    * Examples:
        * 4 different files (myfile1, myfile2, ...): `elbencho -w /data/myfile[1-4]`
        * Specify two different hosts (node001, node002): `elbencho --hosts node00[1,2]`
        * Specify 2x5=10 different S3 servers (192.168.1.1, 192.168.2.1, ...): `elbencho --s3endpoints http://192.168.[1,2].[1,3,5-7]`

### General Changes
* Use latest Alpine Linux 3.x for Alpine-based docker containers instead of always 3.14.
* Support building without fullscreen live stats to avoid ncurses dependency. ("make NCURSES_SUPPORT=0").

### Contributors
* Thanks to Eyal Rif and Oz Perry for helpful comments and suggestions.

## v3.0.3 (Nov 11, 2023)

### New Features & Enhancements
* Block variance to defeat compression is now also effective for writes from GPUs.
* Makefile now provides options to manually specify CUDA include and library paths.
* Added support for S3 multi-delete (S3 DeleteObjects) operation. See new option "--s3multidel".

### General Changes
* S3 error messages now also show HTTP error code.
* Use latest Alpine Linux 3.x for static builds instead of always 3.14.
* Core binding arguments now support "all" as value: "--zones all" & "--cores all".
* GPU list argument now supports "all" as value: "--gpuids all".
* Added cygserver.exe to Windows .zip package to speed up Active Directory lookups.

### Fixes
* elbencho-chart tool now also works with csv files created via "--livecsv".

### Contributors
* Thanks to Bob Holmes and Github user bolochavaez for code contributions. Thanks to Prabhjyot Singh Saluja, David Johnson, Erez Zilber, Dima Persov, Omri Zedan, Andy Black for helpful comments and suggestions.

## v3.0.1 (Aug 08, 2023)

### New Features & Enhancements
* Added N-to-M network bandwidth test. See new option "--netbench".
* Added support for memory mapped IO (aka mmap). See new option "--mmap".
* Added support for file access hints via fadvise. See new option "--fadv".
* Added support for mmap access hints via madvise. See new option "--madv".

### General Changes
* Updated S3 to latest AWS SDK CPP v1.11.102.
* Updated mimalloc memory allocation library to latest version 2.1.2.

### Fixes
* Fixed compilation error (missing include directive) when building without S3 support.
* Fixed check for file slice smaller than block size in situations without direct IO.
* Fixed missing random IO flag in config file example.
* Fixed Windows build not listening for TCP/IPv4 connections in service mode.

### Contributors
* Thanks to Glenn K. Lockwood, Avi Drabkin, Michael Bertelson for reporting issues. Thanks to Jan Heichler, Rob Mallory, Maria Gutierrez for helpful comments and suggestions.

## v2.3.1 (Apr 10, 2023)

### New Features & Enhancements
* Added support for Hadoop HDFS through the offical libhdfs. See new option "--hdfs".
* Added average latency to live statistics. Will be shown in "--livecsv" and in fullscreen live stats when "--lat" is given as argument.

### General Changes
* Improved help text for distributed tests ("--help-dist").
* Improved speed of directory creation in custom tree mode by making each worker only create a subset of the dirs.
* Elapsed time is shown more human-friendly in phase results table and in live statistics. (Hours and minutes instead of previously only seconds and milliseconds.)
* Improved error message when existing CSV file fails compatibility check.

### Contributors
* Thanks to Leon Clayton, Andy Black, Roger Goff, John Legato for helpful comments and suggestions.

## v2.2.5 (Dec 04, 2022)

### New Features & Enhancements
* New option "--svcelapsed" shows service instances ordered by completion time of their slowest thread to make it easier to see if some services are always slower/faster than others.

### Contributors
* Thanks to Samuel Fulcomer for helpful comments and suggestions.

## v2.2.3 (Sep 18, 2022)

### New Features & Enhancements
* Block variance algo "balanced" has been replaced with a new vectorized SIMD version to enable fast fill of buffers with higher quality random values ("--blockvaralgo"). The previous version still exists as "balanced_single" and remains the default for "--randalgo".

### General Changes
* Updated S3 to latest AWS SDK CPP v1.9.343. This adds compatibility with Ubuntu 22.04.
* Block variance algo "fast" now gets reseeded every 256KiB at the latest to improve quality of generated random numbers.

### Fixes
* Set alpine 3.14 (instead of latest available 3.x) as exact version for static executable, as 3.15 & 3.16 produced executables that were not usable in service mode when trying to connect.

## v2.2.1 (Sep 08, 2022)

### Important Note
Blocks now get filled with randomized data before each write ("--blockvarpct 100"). This is to prevent unintended effects on storage systems that have compression, deduplication or similar technology enabled. The generation of randomized data can slightly increase CPU utilization and latency for writes, but this increase is presumably neglectable in typical test cases.

### New Features & Enhancements
* Added support for an alternative HTTP service framework as compile-time and runtime option. ("elbencho --service --althttpsvc" enables the alternative framework at runtime when built with "ALTHTTPSVC_SUPPORT=1".)
* Added script to cross-compile static arm64 executable on x86_64 platform. ("build_arm64_static_local.sh")
* New option "--live1n" to print single-line live statistics to a new line on stderr on each update instead of updating in-place like "--live1" does.

### General Changes
* When using S3 option "--s3randobj" then show IOPS in addition to throughput.
* Added new Nvidia CUDA repo keys to MagnumIO docker container.
* When "--rand" is specified in S3 write phase then automatically use "--backward" instead of refusing to run.
* Building elbencho now requires a C++17 compatible compiler.
* Changed block variance percentage default to 100 ("--blockvarpct 100"). Previous default was 0.

### Contributors
* Thanks to Peter Grossoehme, Rob Mallory, Darren Miller and Sascha Kuebart for helpful comments and suggestions.

### Fixes
* Removed variables in comments of RPM spec template file for compatibility with RHEL9.
* Windows executable used wrong allocation block size in stat() info, leading to a note on the console about files possibly being sparse or compressed although they were not.

## v2.1.5 (Apr 30, 2022)

### General Changes
* Updated mimalloc memory allocation library to latest version 2.0.6
* Added dockerfile for SLES 15

### Contributors
* Thanks to Sherif Louis for helpful comments and suggestions.

## v2.1.3 (Apr 14, 2022)

### Fixes
* Fixed a problem that could lead to the csv header line of the "--csvfile" being printed for every run, not only once at the top.
* Fixed a problem when "--blockvarpct" values other than "0" or "100" were selected together with the default "--blockvaralgo fast".

### Contributors
* Thanks to Glenn K. Lockwood and Daniel Drozdowski for reporting an issue.

## v2.1.1 (Apr 10, 2022)

### New Features & Enhancements
* New support for Windows
  * The Windows version is built via Cygwin
  * Benchmark paths can be specified as `/cygdrive/<driveletter>/some/path`
* New option "--dirstats" to show number of completed directories in file write/read phase.
* New option "--livecsv" to log live statistics to csv file. This can be used e.g. to see performance drops within a benchmark phase.
  * New option "--livecsvex" to see individual worker/service results instead of only aggregate results.
  * New option "--liveint" can be used to set the interval for live statistics update in milliseconds.
* New elbencho-chart tool option for custom line colors ("--linecolors").
* When given path is a directory or bucket, option -n/--dirs accepts 0 (zero) now to place files directly in the given dir/bucket instead of using subdirs.
* Service instances now accept "--s3endpoint" argument to specify different S3 endpoints for each service.

### General Changes
* Added check for S3 multi-part uploads that would exceed 10,000 parts per object to warn user early.
* Old option "--refresh" has been renamed to "--liveint".
* Updated to latest AWS SDK CPP v1.9.228.
* Don't treat existing S3 buckets as error. Previously, existing S3 buckets were treated as error for the "-d" option if they were not owned by the current user.

### Fixes
* Init AWS SDK after daemonizing into background in service mode to not prevent shutdown of static builds.

### Contributors
* Thanks to Dima Kaputkin, Jeff Johnson, Greg Kemp and Shafay Latif for helpful comments and suggestions.

## v2.0.9 (Jan 24, 2022)

### New Features & Enhancements
* New option "--cores" to bind threads to CPU cores.
* New option "--nofdsharing" to have each thread open files individually instead of using a shared set of file descriptors for all threads if given path refers to a file or block device.
* New options "--limitread"/"--limitwrite" to limit throughput of each thread.
* New option "--numhosts" to limit the number of used hosts from a given hosts list or hosts file.

### General Changes ###
* Added optional makefile parameters "AWS_LIB_DIR" and "AWS_INCLUDE_DIR" to support using pre-built AWS SDK CPP.

### Contributors
* Thanks to Alon Horev and Ray Coetzee for helpful comments and suggestions.

## v2.0.7 (Jan 05, 2022)

### New Features & Enhancements
* New option "--rwmixthr N" to define the number of reader threads in a write phase. In contrast to "--rwmixpct N" (where each thread performs reads & writes and thus has to complete a certain number of writes before it can continue with reads), this removes the direct coupling of read and write speed by letting the readers run completely separate from the writers.
* In read/write mix mode (--rwmixpct, --rwmixthr) read latency is now measured separate from write latency.
  * Separate read latency rows will now be shown on console when "--lat" is given.
  * Corresponding rwmix read latency columns were added to csv result files.
* New option "--live1" to show single line live statistics instead of using full screen mode.
* New option "--label" to defined custom labels for benchmark run, which will be printed in result files and csv files.
* CSV files now contain columns for elbencho version and complete command line arguments.
* The contributed sweep tools were extended with a new dataset generator script (dgen.sh).
* New build option to use Microsoft mimalloc for memory allocations. This is useful to overcome AWS SDK performance limitations related to memory management when linking against musl-libc, e.g. on Alpine Linux.
  * Alpine-based docker containers have been updated to support S3 with mimalloc.

### General Changes ###
* Added new GPUDirect Storage option "--gds" as shortcut for "--direct --cufile --gdsbufreg".
* Added S3 support to Alpine Linux docker container.
* Updated to latest AWS SDK CPP v1.9.162.
* Label results as "RWMIX0" if "--rwmixpct 0" is given (instead of previously labeling as "WRITE" in this case).
* When "--rand" is used and multiple files/blockdevs are given directly as parameters then each thread now randomly selects the next file/blockdev.
  * Previously each thread iterated over all files/blockdevs in a round-robin fashion.
* Block variance percentage (--blockvarpct) now defines the percentage of bytes within each written block to be randomly generated.
  * Previously this defined the number out of 100 written blocks to be randomly filled. This change is intended to avoid longer sequences of identical blocks, e.g. in case of "--blockvarpct 50". (The result for "--blockvarpct 100" is the same as before.)
* The option "--s3rwmixthr" has been removed and is now replaced by the new option "--rwmixthr", which also works for S3.
* In files given as "--hostsfile", lines starting with a "#" character will now be ignored.

### Fixes
* Previously, block variance (--blockvarpct) was calculated for all blocks in rwmix mode (--rwmixpct). Now we do it only for blocks that are actually being written and skip the read blocks to avoid unnecessary CPU overhead.

### Contributors
* Thanks to Matt Gustafson, Kyle Lamb and Rupert Menezes for helpful comments and suggestions.
* Thanks to Chin Fang for extending the contributed storage sweep tools.

## v2.0.3 (Nov 26, 2021)

### New Features & Enhancements
* New option for infinite workload loop ("--infloop") to let I/O worker threads restart from the beginning instead of terminating when they reach the end of their workload specification. Use ctrl+c or time limit to interrupt.

### General Changes
* New support for static linking on Alpine Linux.

### Fixes
* Fixed init of AWS S3 SDK. Avoids memory errors for repeated S3 tests in service mode, caused by dynamic init/uninit of the SDK, which it is not made for.
* Fixed memory cleanup problem with S3 single-part uploads.

## v2.0.1 (Oct 17, 2021)

### New Features & Enhancements
* New support for S3 object storage benchmarking.
  * Not enabled by default due to extra build time. See README.md for easy instructions on how to enable.
  * S3 support is included in the default docker container.
* New support for random IO ("--rand") when benchmark path is a directory.
  * Can now be used in combination with "-n"/"-N" or custom tree files.
* New Nvidia Magnum IO based Docker container with CUDA and GPUDirect Storage (GDS) support included.
  * See README on Docker Hub for easy instructions: https://hub.docker.com/r/breuner/elbencho
* New Dockerfiles to build containers from local source copy.
  * See README on Docker Hub for easy instructions: https://hub.docker.com/r/breuner/elbencho
* New guide for using elbencho with the Slurm Workload Manager.
  * See here: https://github.com/breuner/elbencho/blob/master/docs/slurm-examples.md

### General Changes
* Using "--quit --hosts ..." now shows whether the service instances confirmed the quit command.
* New "--nodiocheck" parameter to skip check for direct IO alignment.
* New "--dryrun" option to see dataset size and number of entries.
* Additional columns appended to csv files for rwmix read results.
* Additional aggregate read info row in fullscreen live stats for mixed read+write mode.

### Fixes
* Read result in "last done" column for "--rwmixpct" option used completion time of first done thread for calulation of IOPS and throughput.

### Contributors
* Thanks to Chin Fang & Andy Pernsteiner for S3 support testing and feedback.
* Thanks to Paul Hargreaves, Michael Bloom & Glenn K. Lockwood for helpful comments and suggestions.

## v1.9.1 (May 29, 2021)

### New Features & Enhancements
* New custom tree mode option to create/read arbitrary tree structures of directories and files with different sizes (--treefile).
  * New elbencho-scan-path tool can be used to create a tree file from an existing dataset.
* New Dockerfile added
  * New official elbencho Docker repository with usage examples: https://hub.docker.com/r/breuner/elbencho
* New makefile option NO_BACKTRACE=1 to build without backtrace support for musl-libc compatibility.
* New option to load benchmark parameters from config file (-c / --configfile).
* New option to run multiple iterations of the same tests (-i / --iterations).
* The contributed sweep tools (mtelbencho.sh & graph_sweep.sh) are now included in rpm/deb packages.

### Contributors
* Thanks to Zettar team (esp. Chin Fang & Oleskandr Nazarenko) for contributing in multiple ways to the new custom tree mode.
* Thanks to first time contributor Andy Pernsteiner (VAST Data) for providing the Dockerfile and testing on Alpine Linux.
* Thanks to first time contributor Edgar Fajardo Hernandez (Penguin Computing) for providing the config file option and support for multiple iterations.

## v1.8.1 (March 21, 2021)

### New Features & Enhancements
* New option for number of decimal nines to show in latency percentiles (--latpercent9s).

### General Changes
* Improved shared file/blockdev mode with multiple files/blockdevs.
  * For random I/O, each thread now sends I/Os round-robin to the given files/blockdevs.
    * This change was made to improve balance across different files/blockdevs.
    * Previously, each thread started with a different file/blockdev, but then only moved on to the next one after completing its full I/O amount for a particular file/blockdev.
  * For sequential I/O, each thread now writes a consecutive range of the aggregate file/blockdev size.
    * This change was made to improve consecutive access per file and to reduce the minimum required file size in relation to number of threads and block size.
    * This approach might involve multiple files/blockdevs for a certain thread, but will typically only involve a subset of the given files/blockdevs for a single thread.
    * Previously, each thread started with a different file/blockdev, but got assigned a range in every file, thus requiring block size times number of threads as minimum usable size per file/blockdev.
* Size of given files/blockdevs can now also be auto-detected in distributed mode.

### Contributors
* Thanks to first time contributor Wes Vaske (Micron) for helpful suggestions.

## v1.7.1 (Feb 06, 2021)

### New Features & Enhancements
* New tool "elbencho-chart" to easily generate charts from elbencho csv result files.
* New option for mixed read+write (--rwmixpct) to define percentage of block reads in write phase (-w).
* Faster default random number generators for "--rand" and "--blockvarpct" and new options for random number generator selection ("--blockvaralgo" and "--randalgo").

## v1.6.5 (Dec 26, 2020)

### New Features & Enhancements
* New option to verify data by reading directly after writing (--verifydirect).
* New option to control how well written data can be compressed or dedupe'd (--blockvarpct).

### Contributions
* New mtelbencho.sh (multi-test) script to run a storage sweep based on different file sizes. Added to new contrib subdir. Thanks to Chin Fang (Zettar) for this contribution.

### Fixes
* Fix compilation on platforms that don't define gettid().

## v1.6.3 (Dec 12, 2020)

### New Features & Enhancements
* New option to override service mode benchmark paths (--service PATH [MORE_PATHS]). These path overrides can be different for each service instance and will be used by a service instead of any path list provided by master.

### General Changes
* When NUMA zones are given (--zones), then set memory bind policy to only prefer these zones instead of strictly enforcing these zones. This is to avoid the OOM killer on systems where other processes already used up most of the available memory in a particular zone.
* Log backtrace in fault signal handler.

### Fixes
* Correct internal update order of phase done flags for worker threads, which could previously lead to process not terminating correctly in rare cases.

## v1.6.1 (Nov 28, 2020)

### New Features & Enhancements
* New directory sharing option (--dirsharing) to make all threads work in the same dirs instead of individual dirs for each thread.
* New option to truncate files to full size (--trunctosize) in creation/write phase.
* New option to preallocate file disk space (--preallocfile) in creation/write phase.
* File deletion (-F) can be used when benchmark path is a file.

### General Changes
* Renamed option "--notshared" to "--nosvcshare" for clear separation from new "--dirsharing" option.
* Filenames in dir mode now include the thread index (rank) to avoid conflicts in case of dir sharing.

## v1.5.3 (Nov 15, 2020)

### Fixes
* Fixed typos in Makefile related to automatic detection of CUDA path.

## v1.5.1 (Nov 9, 2020)

### New Features & Enhancements
* New support for Nvidia GPUDirect Storage (GDS) via cuFile API (--cufile & --gdsbufreg).
* New bash completion support in deb/rpm packages to complete parameters via tab key.
* Update embedded http server to latest release.
* New option to override service mode GPUs to bind service instances to specific GPUs (--service --gpids ...).
* Show service error messages in preparation phase also on master instead of only in service log.

### Fixes
* Disable phase time limit for sync and cache drop phase, as the low level calls in these phases are blocking.
* Disable phase time limit for worker preparation phase, as a time limit makes no sense in this phase.
* Avoid possible division by 0 with enable live stats in sync and cache drop phase.
* Properly release GPU resources at end of benchmark phase in service mode.
* Update automatic path detection during make for GDS v0.9 beta release.

## v1.4-3 (Oct 30, 2020)

### New Features & Enhancements
* Automatically detect availability of CUDA development libs on system and enable support. (Manually enable/disable still possible via Makefile options.)
* New `--version` parameter to show executable version, network protocol version and included optional build features (such as CUDA support).
* Move boost.org dependencies to separate Makefile variables (CXXFLAGS_BOOST, LDFLAGS_BOOST) to make it easier for users of custom boost libs.

### Fixes
* Properly release service host file descriptors and libaio context if benchmark was interrupted by I/O error or ctrl+c.
* Properly update cpu usage info during single line live stats.
* Do not return application error code when phase time limit exceeded.

### Contributors
* Thanks to first time contributors Chin Fang (Zettar) & team, Michael Chichik (Excelero), Marco Cicala (E4), Glenn Lockwood (NERSC/LBNL) for helpful comments.

## v1.4-1 (Oct 12, 2020)

### New Features & Enhancements
* New phase to sync write buffers to stable storage (--sync).
* New phase to drop linux kernel page cache, dentry cache and inode cache (--dropcache).
* New phase to stat files (--stat).
* New option to read service hosts from file (--hostsfile).
* New CPU utilization info in live stats and phase results (--cpu).
* New option to configure service host update retrieval interval (--svcupint).
* Print CSV headers only to first line of CSV file.
* Reorder CSV file columns for better readability and add CPU utilization info.

### Contributors
* Thanks to first time contributors Salvador Martin (HPCNow), Joe Harlan (Excelero), Wolfgang Szoecs (HPE) for helpful comments.

## v1.3 (Sep 8, 2020)

### New Features & Enhancements
* Improved console output to be easier to read
* Add more accurate dependencies to .deb package
* Add user name to service mode log file name to prevent conflicts between different users
* Update embedded http server to latest release
* Use given `--port` value also in master mode
* Increased internal elapsed time resolution from milli to microseconds

## v1.2 (Aug 17, 2020)

### New Features & Enhancements
* Added data integrity checks.

### Contributors
* Thanks to first time contributor Kirill Shoikhet (Excelero) for helpful comments.

## v1.1 (Aug 12, 2020)

### New Features & Enhancements
* Added GPU data transfer support via Nvidia CUDA library.

### Contributors
* Thanks to first time contributor Eliott Kespi (Excelero).

## v1.0 (July 8, 2020)
* Initial release by Sven Breuner.
