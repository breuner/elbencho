# Changelog of elbencho

## v2.1.0 (work in progress)

### New Features & Enhancements
* New support for Windows
  * The Windows version is built via Cygwin
  * Benchmark paths can be specified as `/cygdrive/<driveletter>/some/path`
* New option "--dirstats" to show number of completed directories in file write/read phase.
* New option "--livecsv" to log live statistics to csv file. This can be used e.g. to see performance drops within a benchmark phase.
  * New option "--livecsvex" to see individual worker/service results instead of only aggregate results.
  * New option "--liveint" can be used to set the interval for live statistics update in milliseconds.

### General Changes
* Added check for S3 multi-part uploads that would exceed 10,000 parts per object to warn user early.
* Old option "--refresh" has been renamed to "--liveint".

### Fixes
* Init AWS SDK after daemonizing into background in service mode to not prevent shutdown of static builds.

### Contributors
* Thanks to Dima Kaputkin and Jeff Johnson for helpful comments and suggestions.

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
  * See here: https://github.com/breuner/elbencho/blob/master/tools/slurm-examples.md

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
