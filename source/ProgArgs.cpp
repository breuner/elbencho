#include <boost/algorithm/string.hpp>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <libgen.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "ProgArgs.h"
#include "ProgException.h"
#include "Terminal.h"
#include "toolkits/FileTk.h"
#include "toolkits/NumaTk.h"
#include "toolkits/TranslatorTk.h"
#include "toolkits/UnitTk.h"

#ifdef CUFILE_SUPPORT
	#include <cuda_runtime.h>
	#include <cufile.h>
#endif

#define DIRECTIO_MINSIZE			512 // min size in bytes for direct IO

#define BENCHPATH_DELIMITER			",\n\r@" // delimiters for user-defined bench dir paths
#define HOSTLIST_DELIMITERS			", \n\r" // delimiters for hosts string (comma or space)
#define HOST_PORT_SEPARATOR			":" // separator for hostname:port
#define ZONELIST_DELIMITERS			", " // delimiters for numa zones string (comma or space)
#define GPULIST_DELIMITERS			", \n\r" // delimiters for gpuIDs string
#define S3ENDPOINTS_DELIMITERS		", \n\r" // delimiters for S3 endpoints list string
#define NETDEV_DELIMITERS			", \n\r" // delimiters for net dev list string

#define ENDL						<< std::endl << // just to make help text print lines shorter

#define FILESHAREBLOCKFACTOR		32 // in custom tree mode, blockSize factor as of which to share
#define FILESHAREBLOCKFACTOR_STR	STRINGIZE(FILESHAREBLOCKFACTOR)

#define CSVFILE_EXPECTED_COMMAS		54 // to check if existing csv was written with other version

#define NETBENCH_PORT_OFFSET		1000 // offset from service port for netbench listen socket
#define NETBENCH_PORT_OFFSET_STR	STRINGIZE(NETBENCH_PORT_OFFSET)

#define NUMAZONES_ALL_ARG			"all" // shortcut for user to select all numa zones
#define CPUCORES_ALL_ARG			"all" // shortcut for user to select all cpu cores
#define GPUIDS_ALL_ARG				"all" // shortcut for user to select all gpus

#define AWS_SDK_LOGPREFIX_DEFAULT	"aws_sdk_"


/**
 * Constructor.
 *
 * @param argc argument strings count as in main().
 * @param argv argument strings vector as in main().
 * @throw ProgException if config is invalid.
 */
ProgArgs::ProgArgs(int argc, char** argv) :
	argsGenericDescription("", Terminal::getTerminalLineLength(80) )
{
	this->argc = argc;
	this->argv = argv;

	progPath = absolutePath(argv[0] );

	defineDefaults();
	defineAllowedArgs();

	try
	{
		// parse user-given command line args

		bpo::options_description argsDescription;
		argsDescription.add(argsGenericDescription).add(argsHiddenDescription);

		bpo::positional_options_description positionalArgsDescription;
		positionalArgsDescription.add(ARG_BENCHPATHS_LONG, -1); // "-1" means "all positional args"

		bpo::store(bpo::command_line_parser(argc, argv).
			options(argsDescription).
			positional(positionalArgsDescription).
			run(),
			argsVariablesMap);
		bpo::notify(argsVariablesMap);
	}
	catch(bpo::too_many_positional_options_error& e)
	{
		throw ProgException(std::string("Too many positional options error: ") + e.what() );
	}
	catch(bpo::error_with_option_name& e)
	{
		throw ProgException(std::string("Error for option name: ") + e.what() );
	}
	catch(std::exception& e)
	{
		throw ProgException(std::string("Arguments error: ") + e.what() );
	}

	LoggerBase::setFilterLevel( (LogLevel)logLevel);

	if(hasUserRequestedHelp() || hasUserRequestedVersion() )
		return;

	bpo::options_description configFileOptions;
	configFileOptions.add(argsGenericDescription); // Adding same conf options from file that are available from cl

	if(!configFilePath.empty())
	{
		std::ifstream ifs{configFilePath.c_str() };
		if(!ifs)
			throw ProgException(std::string("Cannot open config file: " + configFilePath) );
		else
		{
			bpo::store(bpo::parse_config_file(ifs, configFileOptions), argsVariablesMap);
			bpo::notify(argsVariablesMap);
		}
	}

	initImplicitValues();
	convertUnitStrings();
	checkCSVFileCompatibility();
	checkArgs();
}

ProgArgs::~ProgArgs()
{
	for(int fd : benchPathFDsVec)
		close(fd);

	// note: dereg won't hurt if never reg'ed, because CuFileHandleData can handle that case.
	for(CuFileHandleData& cuFileHandleData : cuFileHandleDataVec)
			cuFileHandleData.deregisterHandle();

	for(char* mmapPtr : mmapVec)
		munmap(mmapPtr, fileSize);

#ifdef CUFILE_SUPPORT
	if(isCuFileDriverOpen)
		cuFileDriverClose();
#endif
}

/**
 * Define allowed args and description for help text.
 */
void ProgArgs::defineAllowedArgs()
{
	// paths arg is hidden, because they are set as positional arg
    argsHiddenDescription.add_options()
		(ARG_BENCHPATHS_LONG, bpo::value<StringVec>(),
			"Comma-separated list of either directories, files or block devices to use for "
			"benchmark.")
		(ARG_HELP_LONG "," ARG_HELP_SHORT, "Print this help message.")
		(ARG_HELPBLOCKDEV_LONG, "Print block device & large shared file help message.")
		(ARG_HELPLARGE_LONG, "Print block device & large shared file help message.")
		(ARG_HELPMULTIFILE_LONG, "Print multi-file / multi-directory help message.")
		(ARG_HELPS3_LONG, "Print S3 object storage help message.")
		(ARG_HELPDISTRIBUTED_LONG, "Print distributed benchmark help message.")
		(ARG_HELPALLOPTIONS_LONG, "Print all available options help message.")
	;

    // ordered by user option strings to print user help in alphabetical order
    argsGenericDescription.add_options()
/*al*/	(ARG_SHOWALLELAPSED_LONG, bpo::bool_switch(&this->showAllElapsed),
			"Show elapsed time to completion of each I/O worker thread.")
#ifdef ALTHTTPSVC_SUPPORT
/*al*/	(ARG_ALTHTTPSERVER_LONG, bpo::bool_switch(&this->useAlternativeHTTPService),
			"Use alternative implementation of HTTP service (for testing).")
#endif // ALTHTTPSVC_SUPPORT
/*b*/	(ARG_BLOCK_LONG "," ARG_BLOCK_SHORT, bpo::value(&this->blockSizeOrigStr),
			"Number of bytes to read/write in a single operation. (Default: 1M; "
			"supports base2 suffixes, e.g. \"2M\")")
/*ba*/	(ARG_REVERSESEQOFFSETS_LONG, bpo::bool_switch(&this->doReverseSeqOffsets),
			"Do backwards sequential reads/writes.")
/*bl*/	(ARG_BLOCKVARIANCEALGO_LONG, bpo::value(&this->blockVarianceAlgo),
			"Random number algorithm for \"--" ARG_BLOCKVARIANCE_LONG "\". Values: \""
			RANDALGO_FAST_STR "\" for high speed but weaker randomness; \""
			RANDALGO_BALANCED_SIMD_STR "\" for good balance of speed and randomness; \""
			RANDALGO_STRONG_STR "\" for high CPU cost but strong randomness. "
			"When GPUs are given then the CUDA default (\"XORWOW\") random generator will be "
			"used and this value is ignored. "
			"(Default: " RANDALGO_FAST_STR ")")
/*bl*/	(ARG_BLOCKVARIANCE_LONG, bpo::value(&this->blockVariancePercent),
			"Block variance percentage. Defines the percentage of each block that will be refilled "
			"with random data between writes. This can be used to defeat "
			"compression/deduplication. (Default: 100; Range: 0-100)")
/*c*/	(ARG_CONFIGFILE_LONG "," ARG_CONFIGFILE_SHORT, bpo::value(&this->configFilePath),
			"Path to benchmark configuration file. All command line options starting with "
			"double dashes can be used as \"OPTIONNAME=VALUE\" in the config file. Multiple "
			"options are newline-separated. Lines starting with \"#\" are ignored.")
/*cl*/	(ARG_CLIENTS_LONG, bpo::value(&this->clientsStr),
			"Comma-separated list of service hosts to use as clients in netbench mode. "
			"(Format: hostname[:port])")
/*cl*/	(ARG_CLIENTSFILE_LONG, bpo::value(&this->clientsFilePath),
			"Path to file containing line-separated service hosts to use as clients in netbench "
			"mode. (Format: hostname[:port])")
#ifdef COREBIND_SUPPORT
/*co*/	(ARG_CPUCORES_LONG, bpo::value(&this->cpuCoresStr),
			"Comma-separated list of CPU cores to bind this process to. If multiple cores are "
			"given, then worker threads are bound round-robin to the cores. The special value "
			"\"" CPUCORES_ALL_ARG "\" is short for the list of all available CPU cores."
			"(Hint: See 'lscpu' for available CPU cores.)")
#endif // COREBIND_SUPPORT
/*cp*/	(ARG_CPUUTIL_LONG, bpo::bool_switch(&this->showCPUUtilization),
			"Show CPU utilization in phase stats results.")
/*cs*/	(ARG_CSVFILE_LONG, bpo::value(&this->csvFilePath),
			"Path to file for end results in csv format. This way, results can be imported e.g. "
			"into MS Excel. If the file exists, results will be appended. (See also \"--"
			ARG_CSVLIVEFILE_LONG "\" for progress results in csv format.)")
#ifdef CUFILE_SUPPORT
/*cu*/	(ARG_CUFILE_LONG, bpo::bool_switch(&this->useCuFile),
			"Use cuFile API for reads/writes to/from GPU memory, also known as GPUDirect Storage "
			"(GDS).")
/*cu*/	(ARG_CUFILEDRIVEROPEN_LONG, bpo::bool_switch(&this->useCuFileDriverOpen),
			"Explicitly initialize cuFile lib and open the nvida-fs driver.")
#endif
#ifdef CUDA_SUPPORT
/*cu*/	(ARG_CUHOSTBUFREG_LONG, bpo::bool_switch(&this->useCuHostBufReg),
			"Pin host memory buffers and register with CUDA for faster transfer to/from GPU.")
#endif
/*D*/	(ARG_DELETEDIRS_LONG "," ARG_DELETEDIRS_SHORT, bpo::bool_switch(&this->runDeleteDirsPhase),
			"Delete directories.")
/*d*/	(ARG_CREATEDIRS_LONG "," ARG_CREATEDIRS_SHORT, bpo::bool_switch(&this->runCreateDirsPhase),
			"Create directories. (Already existing dirs are not treated as error.)")
/*di*/	(ARG_DIRECTIO_LONG, bpo::bool_switch(&this->useDirectIO),
			"Use direct IO to avoid caching.")
/*di*/	(ARG_DIRSHARING_LONG, bpo::bool_switch(&this->doDirSharing),
			"If benchmark path is a directory, all threads create their files in the same dirs "
			"instead of using different dirs for each thread. In this case, \"-" ARG_NUMDIRS_SHORT
			"\" defines the total number of dirs for all threads instead of the number of dirs per "
			"thread.")
/*di*/	(ARG_DIRSTATS_LONG, bpo::bool_switch(&this->showDirStats),
			"Show directory completion statistics in file write/read phase. A directory counts as "
			"completed if all files in the directory have been written/read. Only effective if "
			"benchmark path is a directory.")
/*dr*/	(ARG_DROPCACHESPHASE_LONG, bpo::bool_switch(&this->runDropCachesPhase),
			"Drop linux file system page cache, dentry cache and inode cache before/after each "
			"benchmark phase. Requires root privileges. This should be used together with \"--"
			ARG_SYNCPHASE_LONG "\", because only data on stable storage can be dropped from cache. "
			"Note for distributed file systems that this only drops caches on the clients where "
			"elbencho runs, but there might still be cached data on the server.")
/*dr*/	(ARG_DRYRUN_LONG,
			"Don't run any benchmark phase, just print the number of expected entries and dataset "
			"size per benchmark phase.")
/*F*/	(ARG_DELETEFILES_LONG "," ARG_DELETEFILES_SHORT,
			bpo::bool_switch(&this->runDeleteFilesPhase),
			"Delete files.")
/*fa*/	(ARG_FADVISE_LONG, bpo::value(&this->fadviseFlagsOrigStr),
			"Provide file access hints via fadvise(). This value is a comma-separated list of the "
			"following flags: seq, rand, willneed, dontneed, noreuse.")
/*fo*/	(ARG_FOREGROUNDSERVICE_LONG, bpo::bool_switch(&this->runServiceInForeground),
			"When running as service, stay in foreground and connected to console instead of "
			"detaching from console and daemonizing into backgorund.")
#ifdef CUFILE_SUPPORT
/*gd*/	(ARG_GPUDIRECTSSTORAGE_LONG,
			"Use Nvidia GPUDirect Storage API. Enables \"--" ARG_DIRECTIO_LONG "\", \"--"
			ARG_CUFILE_LONG "\", \"--" ARG_GDSBUFREG_LONG "\".")
/*gd*/	(ARG_GDSBUFREG_LONG, bpo::bool_switch(&this->useGDSBufReg),
			"Register GPU buffers for GPUDirect Storage (GDS) when using cuFile API.")
#endif
#ifdef CUDA_SUPPORT
/*gp*/	(ARG_GPUIDS_LONG, bpo::value(&this->gpuIDsStr),
			"Comma-separated list of CUDA GPU IDs to use for buffer allocation. If no other "
			"option for GPU buffers is given then read/write timings will include copy "
			"to/from GPU buffers. GPU IDs will be assigned round robin to different threads. "
			"When this is given in service mode then the given list will override any list given "
			"by the master, which can be used to bind specific service instances to specific GPUs. "
			"The special value \"" GPUIDS_ALL_ARG "\" is short for the list of all available GPUs. "
			"(Hint: CUDA GPU IDs are 0-based; see 'nvidia-smi' for available GPU IDs.)")
/*gp*/	(ARG_GPUPERSERVICE_LONG, bpo::bool_switch(&this->assignGPUPerService),
			"Assign GPUs round robin to service instances (i.e. one GPU per service) instead of "
			"default round robin to threads (i.e. multiple GPUs per service, if multiple given).")
#endif
#ifdef HDFS_SUPPORT
/*hd*/	(ARG_HDFS_LONG, bpo::bool_switch(&this->useHDFS),
			"Use Hadoop HDFS through the official libhdfs. Make sure that CLASSPATH contains "
			"Hadoop XML conf files dir as first value, so that connecting to the \"default\" HDFS "
			"instance works. This can only be used with \"-" ARG_NUMDIRS_SHORT "\" & \"-"
			ARG_NUMFILES_SHORT "\" and a single dir given as argument. (Hint: You might need to "
			"set these environment variables: CLASSPATH=\"$(hadoop classpath)\" "
			"LD_LIBRARY_PATH=\"$JAVA_HOME/lib/server:$HADOOP_HOME/lib/native\" "
			"LIBHDFS_OPTS=\"-Xrs -Xms20g\".)")
#endif
/*ho*/	(ARG_HOSTS_LONG, bpo::value(&this->hostsStr),
			"Comma-separated list of hosts in service mode for coordinated benchmark. When this "
			"argument is used, this program instance runs in master mode to coordinate the given "
			"service mode hosts. The given number of threads, dirs and files is per-host then. "
			"(Format: hostname[:port])")
/*ho*/	(ARG_HOSTSFILE_LONG, bpo::value(&this->hostsFilePath),
			"Path to file containing line-separated service hosts to use for benchmark. Lines "
			"starting with \"#\" will be ignored. (Format: hostname[:port])")
/*i*/	(ARG_ITERATIONS_LONG "," ARG_ITERATIONS_SHORT, bpo::value(&this->iterations),
			"Number of iterations to run the benchmark. (Default: 1)")
/*in*/	(ARG_INFINITEIOLOOP_LONG, bpo::bool_switch(&this->doInfiniteIOLoop),
			"Let I/O threads run in an infinite loop, i.e. they restart from the beginning when "
			"the reach the end of the specified workload. Terminate this via ctrl+c or by using "
			"\"--" ARG_TIMELIMITSECS_LONG "\"")
/*in*/	(ARG_INTERRUPT_LONG, bpo::bool_switch(&this->interruptServices),
			"Interrupt current benchmark phase on given service mode hosts.")
/*io*/	(ARG_IODEPTH_LONG, bpo::value(&this->ioDepth),
			"Depth of I/O queue per thread for asynchronous I/O. Setting this to 2 or higher "
			"turns on async I/O. (Default: 1)")
/*cs*/	(ARG_BENCHLABEL_LONG, bpo::value(&this->benchLabel),
			"Custom label to identify benchmark run in result files.")
/*la*/	(ARG_LATENCY_LONG, bpo::bool_switch(&this->showLatency),
			"Show minimum, average and maximum latency for read/write operations and entries. "
			"In read and write phases, entry latency includes file open, read/write and close.")
/*la*/	(ARG_LATENCYHISTOGRAM_LONG, bpo::bool_switch(&this->showLatencyHistogram),
			"Show latency histogram.")
/*la*/	(ARG_LATENCYPERCENTILES_LONG, bpo::bool_switch(&this->showLatencyPercentiles),
			"Show latency percentiles.")
/*la*/	(ARG_LATENCYPERCENT9S_LONG, bpo::value(&this->numLatencyPercentile9s),
			"Number of decimal nines to show in latency percentiles. 0 for 99%, 1 for 99.9%, 2 for "
			"99.99% and so on. (Default: 0)")
/*li*/	(ARG_LIMITREAD_LONG, bpo::value(&this->limitReadBpsOrigStr),
			"Per-thread read limit in bytes per second.")
/*li*/	(ARG_LIMITWRITE_LONG, bpo::value(&this->limitWriteBpsOrigStr),
			"Per-thread write limit in bytes per second. (In combination with "
			"\"--" ARG_RWMIXPERCENT_LONG "\" this defines the limit for read+write.)")
/*liv*/	(ARG_BRIEFLIVESTATS_LONG, bpo::bool_switch(&this->useBriefLiveStats),
			"Use brief live statistics format, i.e. a single line instead of full screen stats. "
			"The line gets updated in-place.")
/*liv*/	(ARG_LIVESTATSNEWLINE_LONG, bpo::bool_switch(&this->useBriefLiveStatsNewLine),
			"Use brief live statistics format, i.e. a single line instead of full screen stats. "
			"A new line is written to stderr for each update.")
/*liv*/	(ARG_CSVLIVEFILE_LONG, bpo::value(&this->liveCSVFilePath),
			"Path to file for live progress results in csv format. If the file exists, results "
			"will be appended. This must not be the same file that is given as \"--"
			ARG_CSVFILE_LONG "\".")
/*liv*/	(ARG_CSVLIVEEXTENDED_LONG, bpo::bool_switch(&this->useExtendedLiveCSV),
			"Use extended live results csv file. By default, only aggregate results of all worker "
			"threads will be added. This option also adds results of individual threads in "
			"standalone mode or results of individual services in distributed mode.")
/*liv*/	(ARG_LIVEINTERVAL_LONG, bpo::value(&this->liveStatsSleepMS),
			"Update interval for console and csv file live statistics in milliseconds. "
			"(Default: 2000)")
/*lo*/	(ARG_LOGLEVEL_LONG, bpo::value(&this->logLevel),
			"Log level. (Default: 0; Verbose: 1; Debug: 2)")
/*ma*/	(ARG_MADVISE_LONG, bpo::value(&this->madviseFlagsOrigStr),
			"When using mmap, provide access hints via madvise(). This value is a comma-separated "
			"list of the following flags: seq, rand, willneed, dontneed, hugepage, nohugepage.")
/*mm*/	(ARG_MMAP_LONG, bpo::bool_switch(&this->useMmap),
			"Do file IO through memory mapping. Mmap writes cannot extend a file beyond its "
			"current size; if you try this, you will cause a Bus Error (SIGBUS). Thus, you "
			"typically use mmap writes together with \"--" ARG_TRUNCTOSIZE_LONG "\" or \"--"
			ARG_PREALLOCFILE_LONG "\". For random read tests, consider using direct IO and adding "
			"an madvise for random access to disable prefetching. But caching is in the nature of "
			"how mmap IO works, so direct IO won't disable caching in this case. Note also that "
			"memory maps count towards the total virtual address limit of a process and "
			"platform. A typical limit is 128TB, seen as 48 bits virtual address size in "
			"\"/proc/cpuinfo\".")
/*N*/	(ARG_NUMFILES_LONG "," ARG_NUMFILES_SHORT, bpo::value(&this->numFilesOrigStr),
			"Number of files per thread per directory. (Default: 1) Example: \""
			"-" ARG_NUMTHREADS_SHORT "2 -" ARG_NUMDIRS_SHORT "3 -" ARG_NUMFILES_SHORT "4\" will "
			"use 2x3x4=24 files.")
/*n*/	(ARG_NUMDIRS_LONG "," ARG_NUMDIRS_SHORT, bpo::value(&this->numDirsOrigStr),
			"Number of directories per thread. This can be 0 to disable creation of any subdirs, "
			"in which case all workers share the given dir. (Default: 1)")
/*net*/	(ARG_NETBENCH_LONG, bpo::bool_switch(&this->useNetBench),
			"Run network benchmarking. To simulate the typical storage access request/response "
			"pattern, each client thread will send blocksized chunks (\"-" ARG_BLOCK_SHORT "\") to "
			"one of the servers and wait for a reponse of length \"--" ARG_RESPSIZE_LONG "\" bytes "
			"before transmitting the next block. Client threads will get connected round-robin to "
			"the given servers. Blocksize larger than response size simulates writes from clients "
			"to servers, blocksize smaller than response size simulates reads. Client threads use "
			"filesize as limit for amount of data to send. "
			"See \"--" ARG_SERVERS_LONG "\" & \"--" ARG_CLIENTS_LONG "\" for how to define servers "
			"and clients. "
			"The used network port for data transfer connections will be "
			"\"--" ARG_SERVICEPORT_LONG "\" plus " NETBENCH_PORT_OFFSET_STR ". "
			"(Netbench mode defaults to zero block variance.)")
/*net*/	(ARG_NETDEVS_LONG, bpo::value(&this->netDevsStr),
			"Comma-separated list of network device names (e.g. \"eth0\") for "
			"round-robin binding of outgoing (client-side) connections in network benchmark mode. "
			"Requires root privileges.")
/*no0*/	(ARG_IGNORE0USECERR_LONG, bpo::bool_switch(&this->ignore0USecErrors),
			"Do not warn if worker thread completion time is less than 1 microsecond.")
/*noc*/	(ARG_NOCSVLABELS_LONG, bpo::bool_switch(&this->noCSVLabels),
			"Do not print headline with labels to csv file.")
/*nod*/	(ARG_IGNOREDELERR_LONG, bpo::bool_switch(&this->ignoreDelErrors),
			"Ignore not existing files/dirs in deletion phase instead of treating this as error.")
/*nod*/	(ARG_NODIRECTIOCHECK_LONG, bpo::bool_switch(&this->noDirectIOCheck),
			"Don't check direct IO alignment. Many platforms require direct IO alignment. NFS is "
			"a prominent exception.")
/*nof*/	(ARG_NOFDSHARING_LONG, bpo::bool_switch(&this->useNoFDSharing),
			"If benchmark path is a file or block device, let each worker thread open the given"
			"file/bdev separately instead of sharing the same file descriptor among all threads.")
/*nol*/	(ARG_NOLIVESTATS_LONG, bpo::bool_switch(&this->disableLiveStats),
			"Disable live statistics on console.")
/*nop*/	(ARG_NOPATHEXPANSION_LONG, bpo::bool_switch(&this->disablePathBracketsExpansion),
			"Disable expansion of number lists and ranges in square brackets for given paths.")
/*nos*/	(ARG_NOSVCPATHSHARE_LONG, bpo::bool_switch(&this->noSharedServicePath),
			"Benchmark paths are not shared between service instances. Thus, each service instance "
			"will work on its own full dataset instead of a fraction of the data set.")
/*nu*/	(ARG_NUMHOSTS_LONG, bpo::value(&this->numHosts),
			"Number of hosts to use from given hosts list or hosts file. (Default: use all given "
			"hosts)")
/*op*/	(ARG_OPSLOGPATH_LONG, bpo::value(&this->opsLogPath),
			"Absolute path to logfile for all I/O operations (open, read, ...). In service mode, "
			"the service instances will log their operations locally to the given path. Log is in "
			"JSON format. (Default: disabled)")
/*op*/	(ARG_OPSLOGLOCKING_LONG, bpo::bool_switch(&this->useOpsLogLocking),
			"Use file locking to synchronize appends to \"--" ARG_OPSLOGPATH_LONG "\".")
/*ph*/	(ARG_PHASEDELAYTIME_LONG, bpo::value(&this->nextPhaseDelaySecs),
			"Delay between different benchmark phases in seconds. (Default: 0)")
/*po*/	(ARG_SERVICEPORT_LONG, bpo::value(&this->servicePort),
			"TCP port of background service. (Default: " ARGDEFAULT_SERVICEPORT_STR ")")
/*qr*/	(ARG_PREALLOCFILE_LONG, bpo::bool_switch(&this->doPreallocFile),
			"Preallocate file disk space in a write phase via posix_fallocate().")
/*qu*/	(ARG_QUIT_LONG, bpo::bool_switch(&this->quitServices),
			"Quit services on given service mode hosts.")
/*r*/	(ARG_READ_LONG "," ARG_READ_SHORT, bpo::bool_switch(&this->runReadPhase),
			"Read files.")
/*ra*/	(ARG_RANDOMOFFSETS_LONG, bpo::bool_switch(&this->useRandomOffsets),
			"Read/write at random offsets.")
/*ra*/	(ARG_RANDSEEKALGO_LONG, bpo::value(&this->randOffsetAlgo),
			"Random number algorithm for \"--" ARG_RANDOMOFFSETS_LONG "\". Values: \""
			RANDALGO_FAST_STR "\" for high speed but weaker randomness; \""
			RANDALGO_BALANCED_SEQUENTIAL_STR "\" for good balance of speed and randomness; \""
			RANDALGO_STRONG_STR "\" for high CPU cost but strong randomness. "
			"(Default: " RANDALGO_BALANCED_SEQUENTIAL_STR ")")
/*ra*/	(ARG_RANDOMALIGN_LONG, bpo::bool_switch(&this->useRandomAligned),
			"Align random offsets to block size.")
/*ra*/	(ARG_RANDOMAMOUNT_LONG, bpo::value(&this->randomAmountOrigStr),
			"Number of bytes to write/read when using random offsets. Only effective when "
			"benchmark path is a file or block device. (Default: Set to file size)")
/*ra*/	(ARG_RANKOFFSET_LONG, bpo::value(&this->rankOffset),
			"Rank offset for worker threads. (Default: 0)")
/*re*/	(ARG_READINLINE_LONG, bpo::bool_switch(&this->doReadInline),
			"When benchmark path is a directory, read files immediately after write while they are "
			"still open.")
/*re*/	(ARG_RECVBUFSIZE_LONG, bpo::value(&this->sockRecvBufSizeOrigStr),
			"In netbench mode, this sets the receive buffer size of sockets in bytes. "
			"(Supports base2 suffixes, e.g. \"2M\")")
/*re*/	(ARG_RESPSIZE_LONG, bpo::value(&this->netBenchRespSizeOrigStr),
			"Netbench mode server response size in bytes. Servers will send this amount of data as "
			"response to each received block from a client. (Default: 1; "
			"supports base2 suffixes, e.g. \"2M\")")
/*re*/	(ARG_RESULTSFILE_LONG, bpo::value(&this->resFilePath),
			"Path to file for human-readable results, similar to console output. If the file "
			"exists, new results will be appended.")
/*ro*/	(ARG_ROTATEHOSTS_LONG, bpo::value(&this->rotateHostsNum),
			"Number by which to rotate hosts between phases to avoid caching effects. (Default: 0)")
/*rw*/	(ARG_RWMIXPERCENT_LONG, bpo::value(&this->rwMixPercent),
			"Percentage of blocks that should be read in a write phase. (Default: 0; Max: 100)")
/*rw*/	(ARG_RWMIXTHREADS_LONG, bpo::value(&this->numRWMixReadThreads),
			"Number of threads that should do reads in a write phase for mixed read/write. The "
			"given number is out of the total number of threads per host (\"-" ARG_NUMTHREADS_SHORT
			"\"). This assumes that the full dataset has been precreated via normal write. "
			"In S3 mode, this only works in combination with \"-" ARG_NUMDIRS_SHORT "\" and \"-"
			ARG_NUMFILES_SHORT "\".")
/*s*/	(ARG_FILESIZE_LONG "," ARG_FILESIZE_SHORT, bpo::value(&this->fileSizeOrigStr),
			"File size. (Default: 0; supports base2 suffixes, e.g. \"2M\")")
#ifdef S3_SUPPORT
/*s3a*/	(ARG_S3ACLGET_LONG, bpo::bool_switch(&this->runS3AclGet),
			"Get S3 object ACLs.")
/*s3a*/	(ARG_S3ACLGRANTEE_LONG, bpo::value(&this->s3AclGrantee),
			"S3 object ACL grantee.")
/*s3a*/	(ARG_S3ACLGRANTEETYPE_LONG, bpo::value(&this->s3AclGranteeType),
			"S3 object ACL grantee type. Possible values: "
			ARG_S3ACL_GRANTEE_TYPE_ID ", " ARG_S3ACL_GRANTEE_TYPE_EMAIL ", "
			ARG_S3ACL_GRANTEE_TYPE_URI ", " ARG_S3ACL_GRANTEE_TYPE_GROUP)
/*s3a*/	(ARG_S3ACLGRANTS_LONG, bpo::value(&this->s3AclGranteePermissions),
			"S3 object ACL grantee permissions. Comma-separated list of these values: "
			ARG_S3ACL_PERM_NONE_NAME ", " ARG_S3ACL_PERM_FULL_NAME ", "
			ARG_S3ACL_PERM_FLAG_READ_NAME ", " ARG_S3ACL_PERM_FLAG_WRITE_NAME ", "
			ARG_S3ACL_PERM_FLAG_READACP_NAME ", " ARG_S3ACL_PERM_FLAG_WRITEACP_NAME)
/*s3a*/	(ARG_S3ACLPUT_LONG, bpo::bool_switch(&this->runS3AclPut),
			"Put S3 object ACLs. This requires definition of grantee, grantee type and "
			"permissions.")
/*s3a*/	(ARG_S3ACLVERIFY_LONG, bpo::bool_switch(&this->doS3AclVerify),
			"Verify S3 object and bucket ACLs based on given grantee, grantee type and "
			"permissions. This only effective in the corresponding get object or bucket ACL "
			"phase.")
/*s3b*/	(ARG_S3BUCKETACLGET_LONG, bpo::bool_switch(&this->runS3BucketAclGet),
			"Get S3 bucket ACLs.")
/*s3b*/	(ARG_S3BUCKETACLPUT_LONG, bpo::bool_switch(&this->runS3BucketAclPut),
			"Put S3 bucket ACLs. This requires definition of grantee, grantee type and "
			"permissions.")
/*s3e*/	(ARG_S3ENDPOINTS_LONG, bpo::value(&this->s3EndpointsStr),
			"Comma-separated list of S3 endpoints. When this argument is used, the given "
			"benchmark paths are used as bucket names. Also see \"--" ARG_S3ACCESSKEY_LONG "\" & "
			"\"--" ARG_S3ACCESSSECRET_LONG "\". (Format: [http(s)://]hostname[:port])")
/*s3f*/	(ARG_S3FASTGET_LONG, bpo::bool_switch(&this->useS3FastRead),
			"Send downloaded objects directly to /dev/null instead of a memory buffer. This option "
			"is incompatible with any buffer post-processing options like data verification or "
			"GPU data transfer.")
/*s3k*/	(ARG_S3ACCESSKEY_LONG, bpo::value(&this->s3AccessKey),
			"S3 access key.")
/*s3l*/	(ARG_S3LISTOBJ_LONG, bpo::value(&this->runS3ListObjNum),
			"List objects. The given number is the maximum number of objects to retrieve. Use "
			"\"--" ARG_S3OBJECTPREFIX_LONG "\" to start listing with the given prefix. (Multiple "
			"threads will only be effecive if multiple buckets are given.)")
/*s3l*/	(ARG_S3LISTOBJPARALLEL_LONG, bpo::bool_switch(&this->runS3ListObjParallel),
			"List objects in parallel. Requires a dataset created via \"-" ARG_NUMDIRS_SHORT "\" "
			"and \"-" ARG_NUMFILES_SHORT "\" options and parallelizes by using different S3 "
			"listing prefixes for each thread.")
/*s3l*/	(ARG_S3LISTOBJVERIFY_LONG, bpo::bool_switch(&this->doS3ListObjVerify),
			"Verify the correctness of S3 server object listing results in combination with "
			"\"--" ARG_S3LISTOBJPARALLEL_LONG "\". This requires the dataset to be created with "
			"the same values for \"-" ARG_NUMDIRS_SHORT "\" and \"-" ARG_NUMFILES_SHORT "\".")
/*s3l*/	(ARG_S3LOGLEVEL_LONG, bpo::value(&this->s3LogLevel),
			"Log level of AWS S3 SDK. See \"--" ARG_S3LOGFILEPREFIX_LONG "\" for filename. "
			"(Default: 0=disabled; Max: 6)")
/*s3l*/	(ARG_S3LOGFILEPREFIX_LONG, bpo::value(&this->s3LogfilePrefix),
			"Path and filename prefix of AWS S3 SDK log file. \"DATE.log\" will get appended to "
			"the given filename. "
			"(Default: \"" AWS_SDK_LOGPREFIX_DEFAULT "\" in current working directory)")
/*s3m*/	(ARG_S3MULTIDELETE_LONG, bpo::value(&this->runS3MultiDelObjNum),
			"Delete multiple objects in a single DeleteObjects request. This loops on retrieving "
			"a chunk of objects from a listing request and then deleting the retrieved set of "
			"objects in a single request. This makes no assumption about the retrieved object "
			"names and deletes arbitrary object names in the given bucket(s). The given number is "
			"the maximum number of objects to retrieve and delete in a single request. 1000 is a "
			"typical max value. Use \"--" ARG_S3OBJECTPREFIX_LONG "\" to list/delete only objects "
			"with the given prefix. (Multiple threads will only be effecive if multiple buckets "
			"are given.)")
/*s3n*/	(ARG_S3NOMPCHECK_LONG, bpo::bool_switch(&this->ignoreS3PartNum),
			"Don't check for S3 multi-part uploads exceeding 10,000 parts.")
/*s3o*/	(ARG_S3OBJECTPREFIX_LONG, bpo::value(&this->s3ObjectPrefix),
			"S3 object prefix. This will be prepended to all object names when the benchmark path "
			"is a bucket. (A sequence of 3 to 16 \"" RAND_PREFIX_MARKS_SUBSTR "\" chars will be "
			"replaced by a random hex string of the same length.)")
/*s3r*/	(ARG_S3RANDOBJ_LONG, bpo::bool_switch(&this->useS3RandObjSelect),
			"Read at random offsets and randomly select a new object for each S3 block read. Only "
			"effective in read phase and in combination with \"-" ARG_NUMDIRS_SHORT "\" & \"-"
			ARG_NUMFILES_SHORT "\". Read limit for all threads is defined by \"--"
			ARG_RANDOMAMOUNT_LONG "\".")
/*s3r*/	(ARG_S3REGION_LONG, bpo::value(&this->s3Region),
			"S3 region.")
/*s3s*/	(ARG_S3ACCESSSECRET_LONG, bpo::value(&this->s3AccessSecret),
			"S3 access secret.")
/*s3s*/	(ARG_S3SIGNPAYLOAD_LONG, bpo::value(&this->s3SignPolicy),
			"S3 payload signing policy. 0=RequestDependent, 1=Always, 2=Never. Default: 0.")
/*s3t*/	(ARG_S3TRANSMAN_LONG, bpo::bool_switch(&this->useS3TransferManager),
			"Use AWS SDK TransferManager for object downloads. This enables iodepth greater than "
			"1, but only supports simple sequential downloads. This is incompatible with "
			"post-processing options similar to \"--" ARG_S3FASTGET_LONG "\".")
#endif // S3_SUPPORT
/*se*/	(ARG_SENDBUFSIZE_LONG, bpo::value(&this->sockSendBufSizeOrigStr),
			"In netbench mode, this sets the send buffer size of sockets in bytes. "
			"(Supports base2 suffixes, e.g. \"2M\")")
/*se*/	(ARG_SERVERS_LONG, bpo::value(&this->serversStr),
			"Comma-separated list of service hosts to use as servers in netbench mode. "
			"(Format: hostname[:port])")
/*se*/	(ARG_SERVERSFILE_LONG, bpo::value(&this->serversFilePath),
			"Path to file containing line-separated service hosts to use as servers in netbench "
			"mode. (Format: hostname[:port])")
/*se*/	(ARG_RUNASSERVICE_LONG, bpo::bool_switch(&this->runAsService),
			"Run as service for distributed mode, waiting for requests from master.")
/*sh*/	(ARG_FILESHARESIZE_LONG, bpo::value(&this->fileShareSizeOrigStr),
			"In custom tree mode, this defines the file size as of which files are no longer "
			"exlusively assigned to a thread. This means multiple threads read/write different "
			"parts of files that exceed the given size. "
			"(Default: 0, which means " FILESHAREBLOCKFACTOR_STR " x blocksize)")
/*st*/	(ARG_STATFILES_LONG, bpo::bool_switch(&this->runStatFilesPhase),
			"Run file stat benchmark phase.")
/*re*/	(ARG_STATFILESINLINE_LONG, bpo::bool_switch(&this->doStatInline),
			"When benchmark path is a diretory, stat files immediately after open in a write or "
			"read phase.")
/*st*/	(ARG_STARTTIME_LONG, bpo::value(&this->startTime),
			"Start time of first benchmark in UTC seconds since the epoch. Intended to synchronize "
			"start of benchmarks on different hosts, assuming they use synchronized clocks. "
			"(Hint: Try 'date +%s' to get seconds since the epoch.)")
/*sv*/	(ARG_SHOWSVCELAPSED_LONG, bpo::bool_switch(&this->showServicesElapsed),
			"Show elapsed time to completion of each service instance ordered by slowest thread.")
/*sv*/	(ARG_SVCUPDATEINTERVAL_LONG, bpo::value(&this->svcUpdateIntervalMS),
			"Update retrieval interval for service hosts in milliseconds. (Default: 500)")
/*sy*/	(ARG_SYNCPHASE_LONG, bpo::bool_switch(&this->runSyncPhase),
			"Sync Linux kernel page cache to stable storage before/after each phase.")
/*t*/	(ARG_NUMTHREADS_LONG "," ARG_NUMTHREADS_SHORT, bpo::value(&this->numThreads),
			"Number of I/O worker threads. (Default: 1)")
/*ti*/	(ARG_TIMELIMITSECS_LONG, bpo::value(&this->timeLimitSecs),
			"Time limit in seconds for each benchmark phase. If the limit is exceeded for a phase "
			"then no further phases will run. (Default: 0 for disabled)")
/*tr*/	(ARG_TREEFILE_LONG, bpo::value(&this->treeFilePath),
			"The path to a treefile containing a list of dirs and filenames to use. This is called "
			"\"custom tree mode\" and enables testing with mixed file sizes. The general benchmark "
			"path needs to be a directory. Paths contained in treefile are used relative to the "
			"general benchmark directory. The elbencho-scan-path tool is a way to create a "
			"treefile based on an existing data set. Otherwise, options are similar to \"--"
			ARG_HELPMULTIFILE_LONG "\" with the exception of file size and number of dirs/files, "
			"as these are defined in the treefile. (Note: The file list will be split across "
			"worker threads, but dir create/delete is not fully parallel, so don't use this for "
			"dir create/delete performance testing.)")
/*tr*/	(ARG_TREERANDOMIZE_LONG, bpo::bool_switch(&this->useCustomTreeRandomize),
			"In custom tree mode: Randomize file order. Default is order by file size.")
/*tr*/	(ARG_TREEROUNDUP_LONG, bpo::value(&this->treeRoundUpSizeOrigStr),
			"When loading a treefile, round up all contained file sizes to a multiple of the given "
			"size. This is useful for \"--" ARG_DIRECTIO_LONG "\" with its alignment requirements "
			"on many platforms. (Default: 0 for disabled)")
/*tr*/	(ARG_TRUNCATE_LONG, bpo::bool_switch(&this->doTruncate),
			"Truncate files to 0 size when opening for writing.")
/*tr*/	(ARG_TRUNCTOSIZE_LONG, bpo::bool_switch(&this->doTruncToSize),
			"Truncate files to given \"--" ARG_FILESIZE_LONG "\" via ftruncate() when opening for "
			"writing. If the file previously was larger then the remainder is discarded.")
/*ve*/	(ARG_INTEGRITYCHECK_LONG, bpo::value(&this->integrityCheckSalt),
			"Enable data integrity check. Writes sum of given 64bit salt plus current 64bit offset "
			"as file or block device content, which can afterwards be verified in a read phase "
			"using the same salt (e.g. \"1\"). Different salt values can be used to ensure "
			"different contents when running multiple consecutive write and read verifications. "
			"(Default: 0 for disabled)")
/*ve*/	(ARG_VERIFYDIRECT_LONG, bpo::bool_switch(&this->doDirectVerify),
			"Verify data integrity by reading each block directly after writing. Use together with "
			"\"--" ARG_INTEGRITYCHECK_LONG "\", \"--" ARG_CREATEFILES_LONG "\".")
/*ve*/	(ARG_VERSION_LONG, "Show version and included optional build features.")
/*w*/	(ARG_CREATEFILES_LONG "," ARG_CREATEFILES_SHORT,
			bpo::bool_switch(&this->runCreateFilesPhase),
			"Write files. Create them if they don't exist.")
#ifdef LIBNUMA_SUPPORT
/*zo*/	(ARG_NUMAZONES_LONG, bpo::value(&this->numaZonesStr),
			"Comma-separated list of NUMA zones to bind this process to. If multiple zones are "
			"given, then worker threads are bound round-robin to the zones. The special value "
			"\"" NUMAZONES_ALL_ARG "\" is short for the list of all available NUMA zones."
			"(Hint: See 'lscpu' for available NUMA zones.)")
#endif // LIBNUMA_SUPPORT
    ;
}

/**
 * Set default values for not provided command line args.
 */
void ProgArgs::defineDefaults()
{
	/* We define defaults here, because defining them as argsDescription will make them appear in
		the auto-generated help output in an ugly way. */

	this->numThreads = 1;
	this->numDataSetThreads = numThreads; // internally set for service mode
	this->numDirs = 1;
	this->numDirsOrigStr = "1";
	this->numFiles = 1;
	this->numFilesOrigStr = "1";
	this->iterations = 1;
	this->fileSize = 0;
	this->fileSizeOrigStr = "0";
	this->blockSize = 1024*1024;
	this->blockSizeOrigStr = "1M";
	this->useDirectIO = false;
	this->disableLiveStats = false;
	this->ignoreDelErrors = false;
	this->ignore0USecErrors = false;
	this->runCreateDirsPhase = false;
	this->runCreateFilesPhase = false;
	this->runReadPhase = false;
	this->runDeleteFilesPhase = false;
	this->runDeleteDirsPhase = false;
	this->startTime = 0;
	this->runAsService = false;
	this->runServiceInForeground = false;
	this->servicePort = ARGDEFAULT_SERVICEPORT;
	this->interruptServices = false;
	this->quitServices = false;
	this->noSharedServicePath = false;
	this->rankOffset = 0;
	this->logLevel = Log_NORMAL;
	this->showAllElapsed = false;
	this->showServicesElapsed = false;
	this->liveStatsSleepMS = 2000;
	this->useRandomOffsets = false;
	this->useRandomAligned = false;
	this->randomAmount = 0;
	this->randomAmountOrigStr = "0";
	this->ioDepth = 1;
	this->showLatency = false;
	this->showLatencyPercentiles = false;
	this->numLatencyPercentile9s = 0;
	this->showLatencyHistogram = false;
	this->doTruncate = false;
	this->timeLimitSecs = 0;
	this->useExtendedLiveCSV = false;
	this->noCSVLabels = false;
	this->assignGPUPerService = false;
	this->useCuFile = false;
	this->useGDSBufReg = false;
	this->useCuFileDriverOpen = false;
	this->useCuHostBufReg = false;
	this->integrityCheckSalt = 0;
	this->runSyncPhase = false;
	this->runDropCachesPhase = false;
	this->runStatFilesPhase = false;
	this->showCPUUtilization = false;
	this->svcUpdateIntervalMS = 500;
	this->doTruncToSize = false;
	this->doPreallocFile = false;
	this->doDirSharing = false;
	this->doDirectVerify = false;
	this->blockVariancePercent = 100;
	this->rwMixPercent = 0;
	this->useRWMixPercent = false;
	this->blockVarianceAlgo = RANDALGO_FAST_STR;
	this->randOffsetAlgo = RANDALGO_BALANCED_SEQUENTIAL_STR;
	this->fileShareSize = 0;
	this->fileShareSizeOrigStr = "0";
	this->useCustomTreeRandomize = false;
	this->treeRoundUpSize = 0;
	this->treeRoundUpSizeOrigStr = "0";
	this->useS3FastRead = false;
	this->useS3TransferManager = false;
	this->s3LogLevel = 0;
	this->s3LogfilePrefix = AWS_SDK_LOGPREFIX_DEFAULT;
	this->noDirectIOCheck = false;
	this->runS3ListObjNum = 0;
	this->runS3ListObjParallel = false;
	this->doS3ListObjVerify = false;
	this->doReverseSeqOffsets = false;
	this->doInfiniteIOLoop = false;
	this->s3SignPolicy = 0;
	this->useS3RandObjSelect = false;
	this->numRWMixReadThreads = 0;
	this->useRWMixReadThreads = false;
	this->useBriefLiveStats = false;
	this->useBriefLiveStatsNewLine = false;
	this->useNoFDSharing = false;
	this->limitReadBps = 0;
	this->limitReadBpsOrigStr = "0";
	this->limitWriteBps = 0;
	this->limitWriteBpsOrigStr = "0";
	this->numHosts = -1;
	this->ignoreS3PartNum = false;
	this->showDirStats = false;
	this->useAlternativeHTTPService = false;
	this->useHDFS = false;
	this->useNetBench = false;
	this->numNetBenchServers = 0;
	this->netBenchRespSize = 1;
	this->netBenchRespSizeOrigStr = "1";
	this->sockRecvBufSize = 0;
	this->sockRecvBufSizeOrigStr = "0";
	this->sockSendBufSize = 0;
	this->sockSendBufSizeOrigStr = "0";
	this->useMmap = false;
	this->fadviseFlags = 0;
	this->madviseFlags = 0;
	this->runS3MultiDelObjNum = 0;
	this->disablePathBracketsExpansion = false;
	this->useS3ObjectPrefixRand = false;
	this->doReadInline = false;
	this->doStatInline = false;
	this->nextPhaseDelaySecs = 0;
	this->rotateHostsNum = 0;
	this->runS3AclPut = false;
	this->runS3AclGet = false;
	this->doS3AclVerify = false;
	this->runS3BucketAclPut = false;
	this->runS3BucketAclGet = false;
	this->useOpsLogLocking = false;
}

/**
 * Initialize implicit values that depend e.g. on user config but are not directly given as user
 * config values.
 */
void ProgArgs::initImplicitValues()
{
	/* note: boost bool type options are always present (because they default to false), so can't
		just be checked via argsVariablesMap.count(). */

	if(argsVariablesMap.count(ARG_RWMIXPERCENT_LONG) )
		useRWMixPercent = true;

	if(argsVariablesMap.count(ARG_RWMIXTHREADS_LONG) )
		useRWMixReadThreads = true;

	numRWMixReadThreads = std::min(numRWMixReadThreads, numThreads);

	if(argsVariablesMap.count(ARG_GPUDIRECTSSTORAGE_LONG) )
	{
		useDirectIO = true;
		useCuFile = true;
		useGDSBufReg = true;
	}

	if(useBriefLiveStatsNewLine)
		useBriefLiveStats = true;

	if(!s3EndpointsStr.empty() && runAsService)
	{
		LOGGER(Log_NORMAL, "NOTE: S3 endpoints given. These will be used instead of any endpoints "
			"provided by master." << std::endl);

		s3EndpointsServiceOverrideStr = s3EndpointsStr;
	}

	benchLabelNoCommas = benchLabel;
	std::replace(benchLabelNoCommas.begin(), benchLabelNoCommas.end(), ',', ' ');

	if(useNetBench)
	{
		runCreateFilesPhase = true;

		// default to no block variance unless explicitly requested by user
		if(!argsVariablesMap.count(ARG_BLOCKVARIANCE_LONG) )
			blockVariancePercent = 0;

		// netbench uses dir mode. transfer size is 1x filesize per thread. this is for live stats.
		numDirsOrigStr = "0";
		numDirs = 0;
		numFilesOrigStr = "1";
		numFiles = 1;
	}

	if(integrityCheckSalt && blockVariancePercent)
	{
		if(runCreateFilesPhase)
			LOGGER(Log_NORMAL, "NOTE: Integrity check disables block variance." << std::endl);

		blockVariancePercent = 0;
	}

	useS3ObjectPrefixRand = (s3ObjectPrefix.find(RAND_PREFIX_MARKS_SUBSTR) != std::string::npos);
}

/**
 * Convert human strings/units to machine units. This includes conversion of unit suffixes
 * (e.g. "4K") to actual numbers and human flags (e.g. madvise=rand,hugepage) to combined numeric
 * flags values.
 */
void ProgArgs::convertUnitStrings()
{
	blockSize = UnitTk::numHumanToBytesBinary(blockSizeOrigStr, false);
	fileSize = UnitTk::numHumanToBytesBinary(fileSizeOrigStr, false);
	numDirs = UnitTk::numHumanToBytesBinary(numDirsOrigStr, false);
	numFiles = UnitTk::numHumanToBytesBinary(numFilesOrigStr, false);
	randomAmount = UnitTk::numHumanToBytesBinary(randomAmountOrigStr, false);
	fileShareSize = UnitTk::numHumanToBytesBinary(fileShareSizeOrigStr, false);
	treeRoundUpSize = UnitTk::numHumanToBytesBinary(treeRoundUpSizeOrigStr, false);
	limitReadBps = UnitTk::numHumanToBytesBinary(limitReadBpsOrigStr, false);
	limitWriteBps = UnitTk::numHumanToBytesBinary(limitWriteBpsOrigStr, false);
	netBenchRespSize = UnitTk::numHumanToBytesBinary(netBenchRespSizeOrigStr, false);
	sockRecvBufSize = UnitTk::numHumanToBytesBinary(sockRecvBufSizeOrigStr, false);
	sockSendBufSize = UnitTk::numHumanToBytesBinary(sockSendBufSizeOrigStr, false);

	fadviseFlags = TranslatorTk::fadviseArgsStrToFlags(fadviseFlagsOrigStr);
	madviseFlags = TranslatorTk::madviseArgsStrToFlags(madviseFlagsOrigStr);
}

/**
 * Check parsed args and assign internal values.
 *
 * @throw ProgException if a problem is found.
 */
void ProgArgs::checkArgs()
{
	// parse/apply numa zone as early as possible to have all further allocations in right zone
	parseNumaZones();
	parseCPUCores();
	parseNetDevs();

	if(runAsService)
	{
		numThreads = 0; // master will set the actual number, so no reason to start with high num
		numDataSetThreads = numThreads;

		// service mode and host list for coordinated master mode are mutually exclusive
		if(!hostsStr.empty() )
			throw ProgException("Service mode and host list definition are mutually exclusive.");

		// check/apply override of benchmark paths
		if(argsVariablesMap.count(ARG_BENCHPATHS_LONG) )
			benchPathsVec = argsVariablesMap[ARG_BENCHPATHS_LONG].as<StringVec>();

		if(!benchPathsVec.empty() )
		{
			LOGGER(Log_NORMAL, "NOTE: Benchmark paths given. These paths will be used instead of "
				"any paths provided by master." << std::endl);
			benchPathsServiceOverrideVec = benchPathsVec;
		}

		// check/apply override of GPU IDs
		if(!gpuIDsStr.empty() )
		{
			LOGGER(Log_NORMAL, "NOTE: GPU IDs given. These GPU IDs will be used instead of any GPU "
				"ID list provided by master." << std::endl);
			gpuIDsServiceOverride = gpuIDsStr;
		}

		return;
	}

	///////////// if we get here, we are not running as service...

	parseHosts();
	parseGPUIDs();
	parseRandAlgos();
	parseS3Endpoints();

	if( (interruptServices || quitServices) && hostsVec.empty() )
		throw ProgException("Service interruption/termination requires a host list.");

	if(interruptServices || quitServices)
		return; // interruption does not require any args except for host list

	parseAndCheckPaths();

	if(!numThreads)
		throw ProgException("Number of threads may not be zero.");
	if(!iterations)
		throw ProgException("Number of iterations may not be zero.");

	numDataSetThreads = (!hostsVec.empty() && getIsServicePathShared() ) ?
		(numThreads * hostsVec.size() ) : numThreads;

	if(!fileShareSize)
		fileShareSize = FILESHAREBLOCKFACTOR * blockSize;

	loadCustomTreeFile();

	if(useCuFile && (ioDepth > 1) )
		throw ProgException("cuFile API does not support \"IO depth > 1\"");

	if(useCuFile && !useDirectIO)
	{
		LOGGER(Log_NORMAL,
			"NOTE: cuFile API requires direct IO. "
			"Enabling \"--" ARG_DIRECTIO_LONG "\"." << std::endl);

		useDirectIO = true;
	}

	if(useNetBench && hostsVec.empty() )
		throw ProgException("Missing servers & clients definition for netbench mode.");

	if(useNetBench && !numNetBenchServers)
		throw ProgException("At least one server needs to be defined for netbench mode.");

	if(useNetBench && (numNetBenchServers >= hostsVec.size() ) )
		throw ProgException("At least one client needs to be defined for netbench mode.");

	if(useNetBench && (!blockSize || !netBenchRespSize || !fileSize) )
		throw ProgException(
			"Blocksize, response size and file size must not be zero in netbench mode.");

	if(useNetBench && (runCreateDirsPhase || runDeleteDirsPhase || runStatFilesPhase ||
		runReadPhase || runDeleteFilesPhase) )
		throw ProgException("Netbench mode only run in write phase.");

	if(useRandomOffsets && useHDFS && runCreateFilesPhase)
		throw ProgException("HDFS does not support random offsets for writes.");

	if(!treeFilePath.empty() && useHDFS)
		throw ProgException("HDFS mode does not support custom tree files.");

	if(!gpuIDsStr.empty() && useHDFS)
		throw ProgException("HDFS mode does not support GPUs.");

	if(rwMixPercent && useHDFS)
		throw ProgException("HDFS does not support rwmix.");

	if( (ioDepth > 1) && useHDFS)
		throw ProgException("HDFS does not support IO depth larger than 1.");

	if(!noDirectIOCheck && useHDFS)
		noDirectIOCheck = true; // direct IO flag not relevant for hdfs buffering

	if( (ioDepth > 1) && useMmap)
		throw ProgException("Memory mapped IO (mmap) does not support IO depth larger than 1.");

	if(!gpuIDsStr.empty() && useMmap)
		throw ProgException("Memory mapped IO (mmap) cannot be used with GPUs.");

	if(useRandomOffsets && !s3EndpointsStr.empty() && runCreateFilesPhase)
		LOGGER(Log_NORMAL, "NOTE: S3 write/upload cannot be used with random offsets. "
			"Falling back to \"--" ARG_REVERSESEQOFFSETS_LONG "\"." << std::endl);

	if(useRandomOffsets && !s3EndpointsStr.empty() && useS3TransferManager)
		throw ProgException("S3 TransferManager does not support random offsets.");

	if(!ignoreS3PartNum && !s3EndpointsStr.empty() && fileSize && blockSize &&
		runCreateFilesPhase && ( (fileSize/blockSize) > 10000) )
		throw ProgException("Specified multi-part upload would exceed 10,000 parts per object. "
			"This exceeds the S3 specification and thus is likely to get rejected by the server. "
			"(\"--" ARG_S3NOMPCHECK_LONG "\" disables this check.)");

	if(useCuFile && !s3EndpointsStr.empty() )
		throw ProgException("cuFile API cannot be used with S3");

	if(hasUserSetRWMixPercent() && !s3EndpointsStr.empty() )
		throw ProgException("Option \"--" ARG_RWMIXPERCENT_LONG "\" cannot be used with S3. "
			"Consider \"--" ARG_RWMIXTHREADS_LONG "\" as alternative.");

	if(hasUserSetRWMixPercent() && hasUserSetRWMixReadThreads() )
		throw ProgException("Option \"--" ARG_RWMIXPERCENT_LONG "\" cannot be used together with "
			"\"--" ARG_RWMIXTHREADS_LONG "\"");

	if(rwMixPercent && !gpuIDsVec.empty() && !useCuFile)
		throw ProgException("Option \"--" ARG_RWMIXPERCENT_LONG "\" cannot be used together with "
			"GPU memory copy");

	if(integrityCheckSalt && rwMixPercent)
		throw ProgException("Option --" ARG_RWMIXPERCENT_LONG " cannot be used together with "
			"option \"--" ARG_INTEGRITYCHECK_LONG "\"");

	if(integrityCheckSalt && blockVariancePercent && runCreateFilesPhase)
		throw ProgException("Option \"--" ARG_INTEGRITYCHECK_LONG "\" requires "
			"\"--" ARG_BLOCKVARIANCE_LONG " 0\"");

	if(integrityCheckSalt && runCreateFilesPhase && useRandomOffsets)
		throw ProgException("Integrity check writes are not supported in combination with random "
			"offsets.");

	if(doDirectVerify && (!integrityCheckSalt || !runCreateFilesPhase) )
		throw ProgException("Direct verification requires --" ARG_INTEGRITYCHECK_LONG " and "
			"--" ARG_CREATEFILES_LONG);

	if(doDirectVerify && (ioDepth > 1) )
		throw ProgException("Direct verification cannot be used together with --" ARG_IODEPTH_LONG);

	if(doReadInline && (ioDepth > 1) )
		throw ProgException("Inline read cannot be used together with --" ARG_IODEPTH_LONG);

	if(!hostsVec.empty() )
		return;

	///////////// if we get here, we are running in local standalone mode...

	checkPathDependentArgs();
}

/**
 * Check arguments that depend on the bench path type, which also includes possibly auto-detected
 * file size in file/bdev mode. This is not intended to be called in master mode (e.g. because it
 * uses numDataSetThreads, which is set correctly only in service mode and local standalone mode).
 *
 * @throw ProgException if a problem is found.
 */
void ProgArgs::checkPathDependentArgs()
{
	if( ( (benchPathType != BenchPathType_DIR) || !treeFilePath.empty() ) &&
		(argsVariablesMap.count(ARG_NUMDIRS_LONG) || argsVariablesMap.count(ARG_NUMDIRS_SHORT) ) )
		LOGGER(Log_NORMAL, "NOTE: \"--" ARG_NUMDIRS_LONG "\" is only effective when benchmark "
			"path is a directory (or bucket) and when no custom tree file is given." << std::endl);

	if( ( (benchPathType != BenchPathType_DIR) || !treeFilePath.empty() ) &&
		(argsVariablesMap.count(ARG_NUMFILES_LONG) || argsVariablesMap.count(ARG_NUMFILES_SHORT) ) )
		LOGGER(Log_NORMAL, "NOTE: \"--" ARG_NUMFILES_LONG "\" is only effective when benchmark "
			"path is a directory (or bucket) and when no custom tree file is given." << std::endl);

	if( (benchPathType != BenchPathType_DIR) && runStatFilesPhase)
		throw ProgException("File stat phase can only be used when benchmark path is a directory.");

	// ensure bench path is dir when tree file is given
	if( (benchPathType != BenchPathType_DIR) && !treeFilePath.empty() )
		throw ProgException("Custom tree mode requires benchmark path to be a directory.");

	if(runS3ListObjNum && (benchPathType != BenchPathType_DIR) )
		throw ProgException("Object listing requires a bucket name as benchmark path.");

	if(runS3ListObjNum && s3EndpointsVec.empty() )
		throw ProgException("Object listing requires S3 endpoints definition.");

	if( (runS3AclPut || runS3AclGet || runS3BucketAclPut || runS3BucketAclGet) &&
		s3EndpointsVec.empty() )
		throw ProgException("Putting/getting bucket or object ACLs requires S3 endpoints "
			"definition.");

	if( (hasUserSetRWMixPercent() || hasUserSetRWMixReadThreads() ) &&
		!s3EndpointsStr.empty() &&
		!treeFilePath.empty() )
		throw ProgException("Options \"--" ARG_RWMIXPERCENT_LONG "\" & "
			"\"--" ARG_RWMIXTHREADS_LONG "\" cannot be used with S3 custom tree.");

	if(runS3ListObjNum && !treeFilePath.empty() )
		LOGGER(Log_NORMAL, "NOTE: Ignoring custom tree file for object listing." << std::endl);

	/* ensure only single bench dir with tree file (otherwise we can't guarantee that the right dir
		for each file exist in a certain bench path) */
	if(!treeFilePath.empty() &&  (benchPathsVec.size() > 1) )
		throw ProgException("Custom tree mode can only be used with a single benchmark path.");

	// prevent blockSize==0 if fileSize>0 should be read or written
	if(fileSize && !blockSize && (runReadPhase || runCreateFilesPhase) )
		throw ProgException("Block size must not be 0 when file size is given.");

	// reduce block size to file size (unless file size unknown in custom tree mode)
	if( (blockSize > fileSize) && treeFilePath.empty() )
	{
		// only log if file size is not zero, so that we actually need block size
		if(fileSize  && (runReadPhase || runCreateFilesPhase) )
			LOGGER(Log_VERBOSE, "NOTE: Reducing block size to not exceed file size. "
				"Old: " << blockSize << "; " <<
				"New: " << fileSize << std::endl);

		blockSize = fileSize; // to avoid allocating large buffers if file size is small
	}

	// reduce file size to multiple of block size for directIO
	if(useDirectIO && fileSize && (runCreateFilesPhase || runReadPhase) && (fileSize % blockSize) )
	{
		size_t newFileSize = fileSize - (fileSize % blockSize);

		LOGGER(Log_NORMAL, "NOTE: File size for direct IO is not a multiple of block size. "
			"Reducing file size. " <<
			"Old: " << fileSize << "; " <<
			"New: " << newFileSize << std::endl);

		fileSize = newFileSize;
	}

	// auto-set randomAmount if not set by user
	if(!randomAmount)
	{
		if( (benchPathType != BenchPathType_DIR) && useRandomOffsets)
			randomAmount = fileSize * benchPathsVec.size();
		else
		if( (benchPathType == BenchPathType_DIR) && !s3EndpointsVec.empty() && useS3RandObjSelect)
			randomAmount = fileSize * (numDirs ? numDirs : 1) * numFiles * numDataSetThreads;
	}

	if(useDirectIO && fileSize && (runCreateFilesPhase || runReadPhase) )
	{
		if(useRandomOffsets && !useRandomAligned)
		{
			LOGGER(Log_VERBOSE, "NOTE: Direct IO requires alignment. "
				"Enabling \"--" ARG_RANDOMALIGN_LONG "\"." << std::endl);

			useRandomAligned = true;
		}

		if(!noDirectIOCheck && ( (blockSize % DIRECTIO_MINSIZE) != 0) )
			throw ProgException("Block size for direct IO is not a multiple of required size. "
				"(Note that a system's actual required size for direct IO might be even higher "
				"depending on system page size and drive sector size. "
				"\"--" ARG_NODIRECTIOCHECK_LONG "\" disables this check.) "
				"Required size: " + std::to_string(DIRECTIO_MINSIZE) );
	}

	if(useRandomOffsets && useRandomAligned && blockSize && (randomAmount % blockSize) &&
		(benchPathType != BenchPathType_DIR) )
	{
		size_t newRandomAmount = randomAmount - (randomAmount % blockSize);

		LOGGER(Log_NORMAL, "NOTE: Random amount for aligned IO is not a multiple of block size. "
			"Reducing random amount. " <<
			"Old: " << randomAmount << "; " <<
			"New: " << newRandomAmount << std::endl);

		randomAmount = newRandomAmount;
	}

	if( (benchPathType == BenchPathType_DIR) && useRandomOffsets && (fileSize < blockSize) &&
		treeFilePath.empty() )
		throw ProgException("For random offsets, file size must not be smaller than block size.");

	// shared file or block device mode
	if(benchPathType != BenchPathType_DIR)
	{
		// note: we ensure before this point that fileSize, blockSize and thread count are not 0.

		const size_t numFiles = benchPathFDsVec.size();
		const uint64_t numBlocksPerFile = (fileSize / blockSize) +
			( (fileSize % blockSize) ? 1 : 0);
		const uint64_t numBlocksTotal = numBlocksPerFile * numFiles; // total for all files
		const uint64_t blockSetSize = blockSize * numDataSetThreads;

		if(useRandomOffsets && (randomAmount < blockSetSize) && !doInfiniteIOLoop)
			throw ProgException("Random I/O amount (--" ARG_RANDOMAMOUNT_LONG ") must be large "
				"enough so that each I/O thread can at least read/write one block. "
				"Current block size: " + std::to_string(blockSize) + "; "
				"Current dataset thread count: " + std::to_string(numDataSetThreads) + "; "
				"Resulting min valid random amount: " + std::to_string(blockSetSize) );

		// reduce randomAmount to multiple of blockSetSize
		// (check above ensures that randAmount>=blockSetSize)
		if(useRandomOffsets && useRandomAligned && (randomAmount % blockSetSize) )
		{
			size_t newRandomAmount = randomAmount - (randomAmount % blockSetSize);

			LOGGER(Log_NORMAL, "NOTE: Random amount for aligned IO is not a multiple of block size "
				"times number of threads. Reducing random amount. " <<
				"Old: " << randomAmount << "; " <<
				"New: " << newRandomAmount << std::endl);

			randomAmount = newRandomAmount;
		}

		if(!useRandomOffsets && blockSize && (numBlocksTotal < numDataSetThreads) )
			throw ProgException("Aggregate usable file size must be large enough so that each I/O "
				"thread can at least read/write one block. "
				"Block size: " + std::to_string(blockSize) + "; "
				"Dataset thread count: " + std::to_string(numDataSetThreads) + "; "
				"Aggregate file size: " + std::to_string(numFiles*fileSize) + "; "
				"Aggregate blocks: " + std::to_string(numBlocksTotal) );

		if(useRandomOffsets && useRandomAligned && (fileSize < blockSize) )
			throw ProgException("File size must not be smaller than block size when random I/O "
				"with alignment is selected.");

		// verbose log messages...

		if(useRandomOffsets && (randomAmount % blockSetSize) )
			LOGGER(Log_VERBOSE, "NOTE: Random amount is not a multiple of block size times "
				"number of I/O threads, so the last I/O is not a full block." << std::endl);

		if(!useRandomOffsets && (numBlocksTotal % numDataSetThreads) )
			LOGGER(Log_VERBOSE, "NOTE: Number of blocks with given aggregate file size is not a "
				"multiple of given I/O threads, so the number of written blocks slightly differs "
				"among I/O threads." << std::endl);

		// recheck randomAmount after alignment auto-decrease above
		if(useRandomOffsets && !randomAmount && fileSize && (runCreateFilesPhase || runReadPhase) )
			throw ProgException("File size must not be smaller than block size when random I/O "
				"with alignment is selected.");
	}

	if(benchPathType == BenchPathType_FILE)
		ignoreDelErrors = true; // multiple threads will try to delete the same files
}

/**
 * Parse benchPathStr to vector. If this is not the master of a distributed benchmark, also check
 * existence of paths and open them for pathFDsVec.
 *
 * benchPathStr will contain absolute paths afterwards.
 *
 * This also includes initialization of benchPathsVec and cuFileHandleDataVec.
 *
 * @throw ProgException if a problem is found, e.g. path not existing.
 */
void ProgArgs::parseAndCheckPaths()
{
	benchPathsVec.resize(0); // reset before reuse in service mode

	/* bench paths can come in three ways:
		1) In service mode...
			a) As benchPathStr from master (using absolute paths).
			b) As benchPathsServiceOverrideVec, a user-given override list on service start.
		2) As benchPathsVec from command line.
		In all cases, benchPathsVec and benchPathStr will contain the paths afterwards. */

	if(getRunAsService() )
	{ // service: use user-given override list xor take paths from benchPathStr
		if(!benchPathsServiceOverrideVec.empty() )
		{ // user-given override list
			benchPathsVec = benchPathsServiceOverrideVec;
			benchPathStr = "";

			for(std::string path : benchPathsVec)
			{
				// resolve absolute path if path is not an s3 bucket
				benchPathStr += s3EndpointsStr.empty() ? absolutePath(path) : path;
				benchPathStr += std::string(BENCHPATH_DELIMITER)[0];
			}
		}
		else
		{ // no override given, use benchPathStr from master

			// (note: no TranslatorTk::splitAndExpandStr() here; paths from master are taken as-is.)
			boost::split(benchPathsVec, benchPathStr, boost::is_any_of(BENCHPATH_DELIMITER),
				boost::token_compress_on);
		}

		// delete empty string elements from vec (they come from delimiter use at beginning or end)
		TranslatorTk::eraseEmptyStringsFromVec(benchPathsVec);
	}
	else
	{ // master or local: take paths from command line as vector
		if(argsVariablesMap.count(ARG_BENCHPATHS_LONG) )
			benchPathsVec = argsVariablesMap[ARG_BENCHPATHS_LONG].as<StringVec>();

		// expand lists/ranges in square brackets
		if(!disablePathBracketsExpansion)
			TranslatorTk::expandSquareBrackets(benchPathsVec);

		// delete empty string elements from vec (they come from delimiter use at beginning or end)
		TranslatorTk::eraseEmptyStringsFromVec(benchPathsVec);

		convertS3PathsToCustomTree();

		// update benchPathStr to contain absolute paths (for distributed run & for verbose print)
		benchPathStr = "";
		for(std::string path : benchPathsVec)
		{
			// resolve absolute path if path is not an s3 bucket
			benchPathStr += s3EndpointsStr.empty() ? absolutePath(path) : path;
			benchPathStr += std::string(BENCHPATH_DELIMITER)[0];
		}
	}

	if( (benchPathsVec.empty() || benchPathStr.empty() ) && !useNetBench)
		throw ProgException("Benchmark path missing.");

	// skip open of local paths if this is the master of a distributed run
	if(!hostsStr.empty() )
		return;

	// if we get here then this is not the master of a distributed run...

	// skip open of local paths for S3
	if(!s3EndpointsStr.empty() || useHDFS || useNetBench)
	{
		benchPathType = BenchPathType_DIR;
		return;
	}

	prepareBenchPathFDsVec();
	prepareCuFileHandleDataVec();
	prepareMmapVec();
}

/**
 * If S3 endpoints are given and also paths with slashes, then user wants to do shared uploads/
 * downloads. The logic for this is slightly complicated, especially for shared uploads from
 * multiple hosts. So instead of implementing this logic again, we just convert this to a custom
 * tree file and run this as custom tree mode.
 *
 * This assumes that benchPathsVec has already been set and that the custom tree file will be
 * loaded afterwards.
 *
 * This assumes that all paths use the same bucketName or throws exception otherwise.
 *
 * @throw ProgException on error.
 */
void ProgArgs::convertS3PathsToCustomTree()
{
	// nothing to do if not s3 mode or already a treefile specified by user
	if(s3EndpointsStr.empty() || !treeFilePath.empty() || benchPathsVec.empty() )
		return;

	// nothing to do if paths don't contain slashes. (search non-leading slashes, hence pos "1")
	if(benchPathsVec[0].find("/", 1) == std::string::npos)
		return;

	LOGGER(Log_VERBOSE,
		"Implicit conversion of given S3 paths to custom tree mode for shared upload/download "
		"support. File: " << S3_IMPLICIT_TREEFILE_PATH << std::endl);

	std::ofstream fileStream(S3_IMPLICIT_TREEFILE_PATH, std::ofstream::trunc);
	if(!fileStream)
		throw ProgException("Opening output tree file failed: " + S3_IMPLICIT_TREEFILE_PATH);

	std::string bucketName;

	for(std::string& currentPath : benchPathsVec)
	{
		StringVec currentPathVec;
		std::string currentPathCopy = currentPath;

		// remove multiple leading/trailing slashes
		boost::trim_if(currentPath, boost::is_any_of("/") );

		boost::split(currentPathVec, currentPath, boost::is_any_of("/"), boost::token_compress_on);

		// ensure we have a bucketName and objectName in each user-given path
		if(currentPathVec.size() < 2)
			throw ProgException("Conversion to S3 custom tree mode failed because a path without "
				"elements after slash was found: " + currentPathCopy);

		// ensure all paths have the same bucketName
		if(bucketName.empty() )
			bucketName = currentPathVec[0];
		else
		if(bucketName != currentPathVec[0])
			throw ProgException("Different bucket names are not suppported in this mode. "
				"BucketName1: " + bucketName + "; "
				"BucketName2: " + currentPathVec[0] );

		std::string objectName;

		// re-assemble objectName without bucketName
		for(unsigned i=1; i < currentPathVec.size(); i++)
		{
			if(i > 1)
				objectName += "/";

			objectName += currentPathVec[i];
		}

		// write new line to treefile
		fileStream << PathStore::generateFileLine(objectName, fileSize);
	}

	// set bucket name as only path
	benchPathsVec.resize(1);
	benchPathsVec[0] = bucketName;

	// set treefile path
	treeFilePath = S3_IMPLICIT_TREEFILE_PATH;
}

/**
 * Fill benchPathFDsVec with open file descriptors from benchPathsVec.
 * Closing of old FDs usually happens in ProgArgs destructor or resetBenchPath(), but this method
 * also includes closing any previously set FDs in benchPathFDsVec.
 *
 * @throw ProgException on error, e.g. unable to open path.
 */
void ProgArgs::prepareBenchPathFDsVec()
{
	// close existing FDs before reuse in service mode
	for(int fd : benchPathFDsVec)
		close(fd);

	benchPathFDsVec.resize(0); // reset before reuse in service mode

	// check if each given path exists as dir and add it to pathFDsVec
	for(std::string path : benchPathsVec)
	{
		BenchPathType pathType = findBenchPathType(path);

		// fail if different path types are detected
		if(benchPathFDsVec.empty() )
			benchPathType = pathType; // init path type in first round
		else
		if(benchPathType != pathType)
			throw ProgException("Conflicting path type found. All benchmark paths need to have the "
				"same type. "
				"Path: " + path + "; "
				"Path of different type: " + benchPathsVec[0] );

		// sanity check: user prolly doesn't want to create files in /dev
		if( (benchPathType != BenchPathType_DIR) &&
			!path.rfind("/dev/", 0) &&
			!checkPathExists(path) )
			throw ProgException("Refusing to work with not existing entry in /dev: " + path);

		// sanity check: user prolly doesn't want to use /dev in dir mode
		if( (benchPathType == BenchPathType_DIR) &&
			( !path.rfind("/dev/", 0) || (path == "/dev") ) )
			throw ProgException("Refusing to work with directories in /dev: " + path);

		if( (benchPathType != BenchPathType_DIR) &&
			(runCreateDirsPhase || runDeleteDirsPhase) )
			throw ProgException("Directory create and delete options are only allowed if benchmark "
				"path is a directory.");

		if( (benchPathType == BenchPathType_BLOCKDEV) && runDeleteFilesPhase)
			throw ProgException("File delete option is not allowed if benchmark path is a block "
				"device.");

		int fd;
		int openFlags = 0;

		// note: keep flags below in sync with LocalWorker::initThreadFDVec

		if(pathType == BenchPathType_DIR)
			openFlags |= (O_DIRECTORY | O_RDONLY); // O_DIRECTORY only works with O_RDONLY on xfs
		else
		{ // file or blockdev mode
			if(runCreateFilesPhase || runDeleteFilesPhase)
				openFlags |= O_RDWR;
			else
				openFlags |= O_RDONLY;

			if(useDirectIO)
				openFlags |= O_DIRECT;

			// note: no O_TRUNC here, because prepareFileSize() later needs original size
			if( (pathType == BenchPathType_FILE) && runCreateFilesPhase)
				openFlags |= O_CREAT;
		}

		fd = open(path.c_str(), openFlags, MKFILE_MODE);

		if(fd == -1)
			throw ProgException("Unable to open benchmark path: " + path + "; "
				"SysErr: " + strerror(errno) );

		benchPathFDsVec.push_back(fd);

		prepareFileSize(fd, path);
	}
}

/**
 * In file or blockdev mode, fill cuFileHandleDataVec with registered file descriptors from
 * benchPathFDsVec. This is required for use of cuFile API.
 *
 * Deregistration usually happens in ProgArgs destructor or resetBenchPath(), but this method
 * also includes deregistration of any previously set FDs in cuFileHandleDataVec.
 *
 * cuFileHandleDataVec will be initialized (with unregistered handles) even if cuFile API is not
 * selected to make things easier for localworkers.
 *
 * @throw ProgException on error, e.g. registration failed.
 */
void ProgArgs::prepareCuFileHandleDataVec()
{
	// cleanup old registrations before vec reuse in service mode
	for(CuFileHandleData& cuFileHandleData : cuFileHandleDataVec)
		cuFileHandleData.deregisterHandle();

	cuFileHandleDataVec.resize(0); // reset vec before reuse in service mode

	if(benchPathType == BenchPathType_DIR)
		return; // nothing to do in dir mode, because localworkers will do registration then

	for(int fd : benchPathFDsVec)
	{
		// add new element to vec and reference it
		cuFileHandleDataVec.resize(cuFileHandleDataVec.size() + 1);
		CuFileHandleData& cuFileHandleData = cuFileHandleDataVec[cuFileHandleDataVec.size() - 1];

		if(!useCuFile)
			continue; // no registration to be done if cuFile API is not used

		// note: cleanup won't be a prob if reg not done, as CuFileHandleData can handle that case

		cuFileHandleData.registerHandle<ProgException>(fd);
	}
}

/**
 * Fill mmapVec based on benchPathFDsVec with open file descriptors from benchPathsVec.
 * This is only effective in file/bdev mode for random IO (where we really need to have mapped all
 * files/bdevs instead of only one file/bdev per thread at a time), because there is typically a
 * 128TB max virtual address size limit per process (as seen in "lscpu | grep Address").
 *
 * Unmapping usually happens in ProgArgs destructor or resetBenchPath(), but this method also
 * includes unmapping of any previous mappings.
 *
 * @throw ProgException on error.
 */
void ProgArgs::prepareMmapVec()
{
	// cleanup any old memory mappings

	for(char*& mmapPtr : mmapVec)
	{
		if(mmapPtr == MAP_FAILED)
			continue;

		int unmapRes = munmap(mmapPtr, fileSize);

		if(unmapRes == -1)
			ERRLOGGER(Log_NORMAL, "File memory unmap failed. "
				"SysErr: " << strerror(errno) << std::endl);

		mmapPtr = (char*)MAP_FAILED;
	}

	mmapVec.resize(0);

	if(!useMmap)
		return; // nothing to do

	if(benchPathType == BenchPathType_DIR)
		return; // each worker will do own init

	if(!useRandomOffsets)
		return; // each worker will do own init

	// prep mappings based on benchPathFDsVec

	mmapVec.resize(benchPathFDsVec.size(), (char*)MAP_FAILED);

	for(unsigned i=0; i < benchPathFDsVec.size(); i++)
	{
		int protectionMode = runCreateFilesPhase ? (PROT_WRITE | PROT_READ) : PROT_READ;
		int fd = benchPathFDsVec[i];

		if(fd == -1)
		{
			LOGGER(Log_DEBUG, "Cancelling mmap init loop because of fd \"-1\". "
				"Problem path: " << "File: " + benchPathsVec[i] << std::endl);
			break;
		}

		FileTk::fadvise<ProgException>(fd, fadviseFlags, benchPathsVec[i].c_str() );

		mmapVec[i] = (char*)FileTk::mmapAndMadvise<ProgException>(fileSize, protectionMode,
			MAP_SHARED, fd, madviseFlags, benchPathsVec[i].c_str() );
	}
}

/**
 * In file mode and with random writes, truncate file to full size so that reads will work
 * afterwards across the full file size.
 *
 * @fd filedescriptor for file to be prepared if necessary.
 * @path only for error messages.
 * @throw ProgException on error.
 */
void ProgArgs::prepareFileSize(int fd, std::string& path)
{
	if(benchPathType == BenchPathType_FILE)
	{
		struct stat statBuf;

		int statRes = fstat(fd, &statBuf);

		if(statRes == -1)
			throw ProgException("Unable to check size of file through fstat: " + path + "; "
				"SysErr: " + strerror(errno) );

		off_t currentFileSize = statBuf.st_size;

		if(!fileSize && !statBuf.st_size && (runReadPhase || runCreateFilesPhase) )
			throw ProgException("File size must not be 0 when benchmark path is a file. "
				"File: " + path);

		if(!fileSize)
		{
			LOGGER(Log_NORMAL,
				"NOTE: Auto-setting file size. "
				"Size: " << currentFileSize << "; "
				"Path: " << path << std::endl);

			fileSize = currentFileSize;
		}

		if(!runCreateFilesPhase && ( (uint64_t)currentFileSize < fileSize) &&
			S_ISREG(statBuf.st_mode) ) // ignore character devices like "/dev/zero"
			throw ProgException("Given size to use is larger than detected size. "
				"File: " + path + "; "
				"Detected size: " + std::to_string(currentFileSize) + "; "
				"Given size: " + std::to_string(fileSize) );

		if(runCreateFilesPhase)
		{
			// truncate file to 0. (make sure to keep this after all other file size checks.)
			if(doTruncate)
			{
				int truncRes = ftruncate(fd, 0);
				if(truncRes == -1)
					throw ProgException("Unable to ftruncate file. "
						"File: " + path + "; "
						"Size: " + std::to_string(0) + "; "
						"SysErr: " + strerror(errno) );
			}

			// truncate file to given size if set by user or when running in random mode
			// (note: this is for reads in random mode to work across full length)
			if(doTruncToSize ||
				(useRandomOffsets && ( (size_t)currentFileSize < fileSize) ) )
			{
				LOGGER(Log_VERBOSE,
					"Truncating file to full size. "
					"Path: " << path << "; "
					"Size: " << currentFileSize << std::endl);

				int truncRes = ftruncate(fd, fileSize);
				if(truncRes == -1)
					throw ProgException("Unable to set file size through ftruncate. "
						"File: " + path + "; "
						"Size: " + std::to_string(fileSize) + "; "
						"SysErr: " + strerror(errno) );
			}

			// preallocate file blocks if set by user
			if(doPreallocFile)
			{
				LOGGER(Log_DEBUG,
					"Preallocating file blocks. "
					"Path: " << path << "; "
					"Size: " << currentFileSize << std::endl);

				// (note: posix_fallocate does not set errno.)
				int preallocRes = posix_fallocate(fd, 0, fileSize);
				if(preallocRes != 0)
					throw ProgException(
						"Unable to preallocate file disk space through posix_fallocate. "
						"File: " + path + "; "
						"Size: " + std::to_string(fileSize) + "; "
						"SysErr: " + strerror(preallocRes) );
			}
		} // end of runCreateFilesPhase

		// warn when reading sparse (or compressed) files
		if(runReadPhase && !runCreateFilesPhase)
		{
			off_t allocatedFileSize;

			bool isFileSparseorCompressed =
				FileTk::checkFileSparseOrCompressed(statBuf, allocatedFileSize);

			double allocPercent = currentFileSize ?
				100 * (double(allocatedFileSize) / double(currentFileSize) ) : 100;

			// let user know about reading sparse/compressed files
			if(isFileSparseorCompressed)
				LOGGER(Log_NORMAL,
					"NOTE: Allocated file disk space smaller than file size. File seems sparse or "
					"compressed. (Sequential write can fill sparse areas.) "
					"Path: " << path << "; "
					"Allocated: " << std::setprecision(2) << std::fixed << allocPercent << "%" <<
					std::endl);
		}
	}

	if(benchPathType == BenchPathType_BLOCKDEV)
	{
		off_t blockdevSize = lseek(fd, 0, SEEK_END);

		if(blockdevSize == -1)
			throw ProgException("Unable to check size of blockdevice through lseek: " + path + "; "
				"SysErr: " + strerror(errno) );

		if(!blockdevSize)
			throw ProgException("Block device size seems to be 0: " + path);

		if(!fileSize)
		{
			LOGGER(Log_NORMAL,
				"NOTE: Setting file size to block dev size: " << blockdevSize << std::endl);
			fileSize = blockdevSize;
		}

		if(fileSize > (uint64_t)blockdevSize)
			throw ProgException("Given size to use is larger than detected blockdevice size. "
				"Blockdevice: " + path + "; "
				"Detected size: " + std::to_string(blockdevSize) + "; "
				"Given size: " + std::to_string(fileSize) );

		lseek(fd, 0, SEEK_SET); // seek back to start
	}
}

/**
 * Parse hosts string (and prepended servers string and appended clients string) to fill hostsVec.
 * Do nothing if hosts strings and hosts file paths are empty.
 *
 * hostsVec is the result of this. elements will have default port appended if no port was defined.
 *
 * @throw ProgException if a problem is found, e.g. hosts string was not empty, but parsed result
 * 		is empty.
 */
void ProgArgs::parseHosts()
{
	if(!numHosts)
	{ // user explicitly selected zero hosts, so ignore any given hosts list or hosts file
		serversStr = hostsStr = clientsStr = "";
		serversFilePath = hostsFilePath = clientsFilePath = "";
		return;
	}

	struct ArgStrAndFilePathPair
	{
		std::string* hostsStr;
		std::string* hostsFilePath;
	};


	std::vector<ArgStrAndFilePathPair> hostsStrAndPathsVec;
	if(!useNetBench)
		hostsStrAndPathsVec.push_back( { &hostsStr, &hostsFilePath } );
	else
	{
		// note: the order is important: servers first and clients last (for netbench mode)

		hostsStrAndPathsVec.push_back( { &serversStr, &serversFilePath } );
		hostsStrAndPathsVec.push_back( { &clientsStr, &clientsFilePath } );
	}

	// read service hosts from file and add to hostsStr, then add all together to hostsVec
	for(ArgStrAndFilePathPair& currentPair : hostsStrAndPathsVec)
	{
		std::string& currentHostsStr = *currentPair.hostsStr;
		std::string& currentHostsFilePath = *currentPair.hostsFilePath;

		if(currentHostsStr.empty() && currentHostsFilePath.empty() )
			continue; // nothing to do for this pair

		if(!currentHostsFilePath.empty() )
		{
			std::ifstream hostsFile(currentHostsFilePath);

			if(!hostsFile)
				throw ProgException("Unable to read hosts file. Path: " + currentHostsFilePath);

			currentHostsStr += " "; // add separator to existing hosts

			std::string lineStr;

			while(std::getline(hostsFile, lineStr) )
			{
				if(lineStr.rfind("#", 0) == 0)
					continue; // skip lines starting with "#" as comment char

				currentHostsStr += lineStr + ",";
			}

			hostsFile.close();
		}

		StringVec currentHostsVec;

		// split by given delimiters and expand lists/ranges in square brackets
		TranslatorTk::splitAndExpandStr(currentHostsStr, HOSTLIST_DELIMITERS, currentHostsVec);

		// delete empty string elements from vec (they come from delimiter use at beginning or end)
		TranslatorTk::eraseEmptyStringsFromVec(currentHostsVec);

		for(std::string& host : currentHostsVec)
		{
			std::size_t findRes = host.find(HOST_PORT_SEPARATOR);

			// add default port to hosts where port is not provided by user
			if(findRes == std::string::npos)
				host += HOST_PORT_SEPARATOR + std::to_string(servicePort);
		}

		if(currentHostsVec.empty() )
			throw ProgException(
				"Hosts defined, but parsing resulted in an empty list. "
				"Given list: \"" + currentHostsStr + "\"");

		// init number of netbench servers
		if(currentPair.hostsStr == &serversStr)
			numNetBenchServers = currentHostsVec.size();

		// append hostsVec of current round to general hostsVec
		hostsVec.insert(hostsVec.end(), currentHostsVec.begin(), currentHostsVec.end() );

	} // end of for-loop

	// check for duplicates
	std::set<std::string> hostsSet(hostsVec.begin(), hostsVec.end() );
	if(hostsSet.size() != hostsVec.size() )
		throw ProgException("List of hosts contains duplicates. "
			"Number of duplicates: " + std::to_string(hostsVec.size() - hostsSet.size() ) + "; "
			"List: " + TranslatorTk::stringVecToString(hostsVec, ",") );

	// reduce to user-defined number of hosts
	// ("numHosts==-1" means "use all hosts")
	if( (numHosts != -1) && (hostsVec.size() > (unsigned)numHosts) )
		hostsVec.resize(numHosts);

	LOGGER(Log_DEBUG,
		"Finished parsing hosts list. "
		"numHosts: " << numHosts << "; "
		"numNetBenchServers: " << numNetBenchServers << "; "
		"hostsVec: " << TranslatorTk::stringVecToString(hostsVec, ",") << std::endl);
}

/**
 * Parse netBenchServersVec from netBenchServersStr. This is only used in service instances.
 * The master sends the full service hosts list, so we only keep the configured numNetBenchServers
 * and calculate the ports as service port +NETBENCH_PORT_OFFSET for each service.
 */
void ProgArgs::parseNetBenchServersForService()
{
	if(!useNetBench || netBenchServersStr.empty() || !numNetBenchServers)
		return; // nothing to do

	LOGGER(Log_DEBUG, __func__ << ": " <<
		"netBenchServersStr: " << netBenchServersStr << std::endl);

	StringVec tmpServersVec; // intermediate vec before host/port split
	std::string tmpNetBenchServersLogStr; // list of servers just for log message

	// split by given delimiters and expand lists/ranges in square brackets
	TranslatorTk::splitAndExpandStr(netBenchServersStr, HOSTLIST_DELIMITERS, tmpServersVec);

	// delete empty string elements from vec (they come from delimiter use at beginning or end)
	TranslatorTk::eraseEmptyStringsFromVec(tmpServersVec);

	// fill actual netBenchServersVec from individual host names/IPs that might also come with port
	for(std::string& server : tmpServersVec)
	{
		std::size_t findRes = server.find(HOST_PORT_SEPARATOR);

		NetBenchServerAddr newServerAddr;

		// add default port to hosts where port is not provided by user
		// note: netbench port is service port + NETBENCH_PORT_OFFSET.
		if(findRes == std::string::npos)
		{ // no port given
			newServerAddr.host = server;
			newServerAddr.port = ARGDEFAULT_SERVICEPORT + NETBENCH_PORT_OFFSET;
		}
		else
		{ // port given by user
			newServerAddr.host = server.substr(0, findRes);

			std::string portStr = server.substr(findRes+1);
			newServerAddr.port = std::stoi(portStr) + NETBENCH_PORT_OFFSET;
		}

		netBenchServersVec.push_back(newServerAddr);
	}

	if(netBenchServersVec.empty() )
		throw ProgException("Netbench servers defined, but parsing resulted in an empty list: " +
			netBenchServersStr);

	// reduce to user-defined number of servers
	if(netBenchServersVec.size() > numNetBenchServers)
		netBenchServersVec.resize(numNetBenchServers);

	// just for log message
	for(NetBenchServerAddr& addr : netBenchServersVec)
		tmpNetBenchServersLogStr += addr.host + ":" + std::to_string(addr.port) + " ";

	LOGGER(Log_VERBOSE, "netbench servers: " << tmpNetBenchServersLogStr << std::endl);
}

/**
 * Parse numa zones string to fill numaZonesVec. Do nothing if numa zones string is empty.
 * Also applies the given zones to the current thread.
 *
 * Note: We use libnuma in NumaTk, but we don't allow libnuma's NUMA string format to build
 * our vector. That allows us to easily bind I/O workers round-robin to given zones in vec.
 *
 * @throw ProgException if a problem is found, e.g. numa zones string was not empty, but parsed
 * 		result is empty.
 */
void ProgArgs::parseNumaZones()
{
	if(numaZonesStr.empty() )
		return; // nothing to do

	if(!NumaTk::isNumaInfoAvailable() )
		throw ProgException("No NUMA zone info available.");

	LOGGER(Log_DEBUG, __func__ << ": "
		"numCores: " << std::thread::hardware_concurrency() << "; "
		"cpu core affinity list: " << NumaTk::getCurrentCPUAffinityStrHuman() << std::endl);

	if(numaZonesStr == NUMAZONES_ALL_ARG)
		numaZonesStr = NumaTk::getAllNumaZonesStr();

	LOGGER(Log_DEBUG, "raw numaZonesStr: " + numaZonesStr << std::endl);

	StringVec zonesStrVec; // temporary for split()

	// split by given delimiters and expand lists/ranges in square brackets
	TranslatorTk::splitAndExpandStr(numaZonesStr, ZONELIST_DELIMITERS, zonesStrVec);

	// delete empty string elements from vec (they come from delimiter use at beginning or end)
	TranslatorTk::eraseEmptyStringsFromVec(zonesStrVec);

	if(zonesStrVec.empty() )
		throw ProgException("NUMA zones defined, but parsing resulted in an empty list: " +
			numaZonesStr);

	// rebuild numaZonesStr clean from trimmed vec, because libnuma has probs with leading delimter
	numaZonesStr = "";
	for(unsigned i=0; i < zonesStrVec.size(); i++)
		numaZonesStr += (i ? "," : "") + zonesStrVec[i];

	LOGGER(Log_DEBUG, __func__ << ": "
		"zonesStrVec: " << TranslatorTk::stringVecToString(zonesStrVec, " ") << std::endl);

	// convert from string vector to int vector
	for(std::string& zoneStr : zonesStrVec)
		numaZonesVec.push_back(std::stoi(zoneStr) );

	// apply given zones to current thread
	NumaTk::bindToNumaZones(numaZonesStr);

	LOGGER(Log_DEBUG, __func__ << ": "
		"numCores: " << std::thread::hardware_concurrency() << "; "
		"cpu core affinity list after: " << NumaTk::getCurrentCPUAffinityStrHuman() << std::endl);
}

/**
 * Parse cpu cores string to fill cpuCoresVec. Do nothing if cpu cores string is empty.
 * Also applies the given cores to the current thread.
 *
 * @throw ProgException if a problem is found, e.g. cpu cores string was not empty, but parsed
 * 		result is empty.
 */
void ProgArgs::parseCPUCores()
{
	if(cpuCoresStr.empty() )
		return; // nothing to do

	if(cpuCoresStr == CPUCORES_ALL_ARG)
		cpuCoresStr = NumaTk::getCurrentCPUAffinityStr();

	LOGGER(Log_DEBUG, __func__ << ": "
		"raw cpuCoresStr: " + cpuCoresStr << std::endl);

	StringVec coresStrVec; // temporary for split()

	// split by given delimiters and expand lists/ranges in square brackets
	TranslatorTk::splitAndExpandStr(cpuCoresStr, ZONELIST_DELIMITERS, coresStrVec);

	// delete empty string elements from vec (they come from delimiter use at beginning or end)
	TranslatorTk::eraseEmptyStringsFromVec(coresStrVec);

	if(coresStrVec.empty() )
		throw ProgException("CPU cores defined, but parsing resulted in an empty list: " +
			cpuCoresStr);

	// rebuild numaZonesStr clean from trimmed vec, because libnuma has probs with leading delimter
	cpuCoresStr = "";
	for(unsigned i=0; i < coresStrVec.size(); i++)
		cpuCoresStr += (i ? "," : "") + coresStrVec[i];

	// convert from string vector to int vector
	for(std::string& coreStr : coresStrVec)
		cpuCoresVec.push_back(std::stoi(coreStr) );

	/* note: we don't sort the vectors here, because user might initionally have chosen the order
		based on order of other args like gpu IDs. */

	LOGGER(Log_DEBUG, __func__ << ": "
		"numCores: " << std::thread::hardware_concurrency() << "; "
		"cpu core affinity list before: " << TranslatorTk::intVectoHumanStr(cpuCoresVec) <<
		std::endl);

	// apply given cores to current thread
	NumaTk::bindToCPUCores(cpuCoresVec);

	LOGGER(Log_DEBUG, __func__ << ": "
		"numCores: " << std::thread::hardware_concurrency() << "; "
		"cpu core affinity list after: " << NumaTk::getCurrentCPUAffinityStrHuman() << std::endl);
}

/**
 * Parse GPU IDs string to fill gpuIDsVec. Do nothing if gpuIDsStr is empty.
 *
 * @throw ProgException if a problem is found, e.g. gpuIDsStr string was not empty, but parsed
 * 		result is empty.
 */
void ProgArgs::parseGPUIDs()
{
	if(useCuFile && gpuIDsStr.empty() )
		throw ProgException("cuFile API requested, but no GPU specified.");

	if(gpuIDsStr.empty() )
		return; // nothing to do

	#ifndef CUDA_SUPPORT
		throw ProgException("GPU IDs defined, but built without CUDA support.");
	#else
		if(gpuIDsStr == GPUIDS_ALL_ARG)
		{
			gpuIDsStr = "";
			int numCudaDevs;

			cudaError_t getDevsRes = cudaGetDeviceCount(&numCudaDevs);

			if(getDevsRes != cudaSuccess)
				throw ProgException(std::string("Getting CUDA device count failed. ") +
					"CUDA Error: " + cudaGetErrorString(getDevsRes) );

			for(int i=0; i < numCudaDevs; i++)
				gpuIDsStr += ( (i > 0) ? "," + std::to_string(i) : std::to_string(i) );
		}
	#endif // CUDA_SUPPORT

	LOGGER(Log_DEBUG, __func__ << ": "
		"raw gpuIDsStr: " + gpuIDsStr << std::endl);

	StringVec gpuIDsStrVec; // temporary for split()

	// split by given delimiters and expand lists/ranges in square brackets
	TranslatorTk::splitAndExpandStr(gpuIDsStr, GPULIST_DELIMITERS, gpuIDsStrVec);

	// delete empty string elements from vec (they come from delimiter use at beginning or end)
	TranslatorTk::eraseEmptyStringsFromVec(gpuIDsStrVec);

	if(gpuIDsStrVec.empty() )
		throw ProgException("GPU IDs defined, but parsing resulted in an empty list: " +
			gpuIDsStr);

	// convert from string vector to int vector
	for(std::string& gpuIDStr : gpuIDsStrVec)
		gpuIDsVec.push_back(std::stoi(gpuIDStr) );

	#ifndef CUFILE_SUPPORT
		if(useCuFile)
			throw ProgException("cuFile API requested, but this executable was not built with "
				"cuFile support.");
	#else
		if(useCuFile && useCuFileDriverOpen)
		{
			CUfileError_t driverOpenRes = cuFileDriverOpen();

			if(driverOpenRes.err != CU_FILE_SUCCESS)
				throw ProgException(std::string("cuFileDriverOpen failed. cuFile Error: ") +
					CUFILE_ERRSTR(driverOpenRes.err) );

			isCuFileDriverOpen = true;
		}
	#endif // CUFILE_SUPPORT

	LOGGER(Log_DEBUG, __func__ << ": "
		"gpu vec: " << TranslatorTk::intVectoHumanStr(gpuIDsVec) << std::endl);
}

/**
 * Parse S3 endpoints string to fill s3EndpointsVec. Do nothing if s3 endpoints string is empty.
 *
 * @throw ProgException if a problem is found, e.g. s3 endpoints string was not empty, but parsed
 * 		result is empty.
 */
void ProgArgs::parseS3Endpoints()
{
#ifndef S3_SUPPORT
	if(!s3EndpointsStr.empty() )
		throw ProgException("S3 endpoints defined, but built without S3 support.");
#endif // S3_SUPPORT

	if(s3EndpointsStr.empty() )
		return; // nothing to do

	if(!s3EndpointsServiceOverrideStr.empty() && runAsService)
		s3EndpointsStr = s3EndpointsServiceOverrideStr; // user specified override for service

	// split by given delimiters and expand lists/ranges in square brackets
	TranslatorTk::splitAndExpandStr(s3EndpointsStr, S3ENDPOINTS_DELIMITERS, s3EndpointsVec);

	// delete empty string elements from vec (they come from delimiter use at beginning or end)
	TranslatorTk::eraseEmptyStringsFromVec(s3EndpointsVec);

	if(s3EndpointsVec.empty() )
		throw ProgException("S3 endpoints defined, but parsing resulted in an empty list: " +
			s3EndpointsStr);
}

/**
 * Parse network interface devices list. Do nothing if given list is empty.
 *
 * @throw ProgException if a problem is found, e.g. given list was not empty, but parsed
 * 		result is empty.
 */
void ProgArgs::parseNetDevs()
{
	if(netDevsStr.empty() )
		return; // nothing to do

	// split by given delimiters and expand lists/ranges in square brackets
	TranslatorTk::splitAndExpandStr(netDevsStr, NETDEV_DELIMITERS, netDevsVec);

	// delete empty string elements from vec (they come from delimiter use at beginning or end)
	TranslatorTk::eraseEmptyStringsFromVec(netDevsVec);

	if(netDevsVec.empty() )
		throw ProgException("Network devices for binding defined, but parsing resulted in "
			"an empty list: " + netDevsStr);
}


/**
 * Parse random number generator selection for random offsets and block variance..
 */
void ProgArgs::parseRandAlgos()
{
	RandAlgoType randAlgoBlockVariance = RandAlgoSelectorTk::stringToEnum(blockVarianceAlgo);
	if(randAlgoBlockVariance == RandAlgo_INVALID)
		throw ProgException("Invalid random generator selection for block variance: " +
			blockVarianceAlgo);

	RandAlgoType randAlgoOffsets = RandAlgoSelectorTk::stringToEnum(randOffsetAlgo);
	if(randAlgoOffsets == RandAlgo_INVALID)
		throw ProgException("Invalid random generator selection for random offsets: " +
			randOffsetAlgo);
}

/**
 * If tree file is given, load PathStores from tree file. Otherwise do nothing.
 *
 * @throw ProgException on error, such as tree file not exists.
 */
void ProgArgs::loadCustomTreeFile()
{
	if(treeFilePath.empty() )
		return; // nothing to do

	// load directory tree

	if(runCreateDirsPhase || runDeleteDirsPhase)
	{
		customTree.dirs.loadDirsFromFile(treeFilePath);
		customTree.dirs.sortByPathLen();
	}

	// load file trees

	if(runCreateFilesPhase || runStatFilesPhase || runReadPhase || runDeleteFilesPhase ||
		runS3AclPut || runS3AclGet || runS3BucketAclPut || runS3BucketAclGet)
	{
		// load tree of non-shared files (i.e. files that are equal to or smaller than blocksize)

		// (note: fileShareSize is guaranteed to not be 0, thus the "fileShareSize-1" is ok)
		customTree.filesNonShared.setBlockSize(blockSize);
		customTree.filesNonShared.loadFilesFromFile(
			treeFilePath, 0, fileShareSize-1, treeRoundUpSize);
		customTree.filesNonShared.sortByFileSize();

		// load tree of shared files (i.e. files that are greater than blocksize)

		// (note: workers use the same condition to decide special handing of tree file)
		if(runAsService && !s3EndpointsStr.empty() && runCreateFilesPhase &&
			(numDataSetThreads != numThreads) )
		{
			/* shared s3 uploads of an object possible, but may among threads within the same
				service instance (due to the uploadID not beeing shared among service instances).
				thus we split into full files per service according to rank offset. workers can
				afterwards grab shared files from this by subtracting the service's rank offset */

			size_t serviceRank = rankOffset / numThreads;
			size_t numServiceRanksTotal = numDataSetThreads / numThreads;
			PathStore filesSharedTemp;

			// build temporary list of files that are sharable based on their size
			filesSharedTemp.setBlockSize(blockSize);
			filesSharedTemp.loadFilesFromFile(treeFilePath, fileShareSize, ~0ULL, treeRoundUpSize);
			filesSharedTemp.sortByFileSize();

			// apply individual list of shared files for this worker
			// (note: getWorkerSublistNonShared() to get full files, not parts of files)
			customTree.filesShared.setBlockSize(blockSize);
			filesSharedTemp.getWorkerSublistNonShared(serviceRank, numServiceRanksTotal, false,
				customTree.filesShared);
		}
		else
		{
			// this is the normal case: fair sharing across all services/workers based on blocksize

			customTree.filesShared.setBlockSize(blockSize);
			customTree.filesShared.loadFilesFromFile(
				treeFilePath, fileShareSize, ~0ULL, treeRoundUpSize);
			customTree.filesNonShared.sortByFileSize();
		}
	}
}

/**
 * Turn given path into an absolute path. If given path was absolute before, it is returned
 * unmodified. Otherwise the path to the current work dir is prepended.
 *
 * This is intentionally not using realpath() to allow symlinks pointing to different dirs on
 * different hosts in distributed mode. On the other hand, we need absolute paths for distributed
 * mode to be independent of the current work dir, hence this approach.
 *
 * @pathStr possibly relative path, may not be empty.
 * @return absolute path.
 * @throw ProgException on error, e.g. pathStr empty or unable to resolve current working dir.
 */
std::string ProgArgs::absolutePath(std::string pathStr)
{
	if(pathStr.empty() )
		throw ProgException("The absolutePath() method can't handle empty paths");

	if(pathStr[0] == '/')
		return pathStr; // pathStr was already absolute, so nothing to do

	std::array<char, PATH_MAX> currentWorkDir;

	char* getCWDRes = getcwd(currentWorkDir.data(), PATH_MAX);
	if(!getCWDRes)
		throw ProgException(std::string("Failed to resolve current work dir. ") +
			"SysErr: " + strerror(errno) );

	std::string returnPath = std::string(currentWorkDir.data() ) + "/" + pathStr;;

	return returnPath;
}

/**
 * Find out type of given path. Fail if none of BenchPathType.
 *
 * @pathStr path for which we want to find out the type; defaults to BenchPathType_FILE is entry
 * 	does not exist.
 * @throw ProgException if path type is none of BenchPathType or if stat() error occurs.
 */
BenchPathType ProgArgs::findBenchPathType(std::string pathStr)
{
	struct stat statBuf;

	int statRes = stat(pathStr.c_str(), &statBuf);
	if(statRes == -1)
	{
		if(errno != ENOENT)
			throw ProgException("Unable to check type of benchmark path: " + pathStr + "; "
				"SysErr: " + strerror(errno) );

		return BenchPathType_FILE;
	}

    switch(statBuf.st_mode & S_IFMT)
    {
		case S_IFBLK: return BenchPathType_BLOCKDEV;
		case S_IFDIR: return BenchPathType_DIR;
		case S_IFREG: return BenchPathType_FILE;
		case S_IFCHR: return BenchPathType_FILE; // for "/dev/null"
    }

    throw ProgException("Invalid path type: " + pathStr);
}

/**
 * Find out if the given path exists as dir or file.
 *
 * @pathStr the path to be checked for existence.
 * @return false if not exists, true in all other cases (including errors other than not-exists).
 */
bool ProgArgs::checkPathExists(std::string pathStr)
{
	struct stat statBuf;

	int statRes = stat(pathStr.c_str(), &statBuf);
	if(statRes == -1)
	{
		if(errno == ENOENT)
			return false;

		return true;
	}

   return true;
}

/**
 * Check if user gave the argument to print help. If this returns true, then the rest of the
 * settings in this class is not initialized, so may not be used.
 *
 * @return true if help text was requested.
 */
bool ProgArgs::hasUserRequestedHelp()
{
	if(argsVariablesMap.count(ARG_HELP_LONG) ||
		argsVariablesMap.count(ARG_HELP_SHORT) ||
		argsVariablesMap.count(ARG_HELPBLOCKDEV_LONG) ||
		argsVariablesMap.count(ARG_HELPLARGE_LONG) ||
		argsVariablesMap.count(ARG_HELPMULTIFILE_LONG) ||
		argsVariablesMap.count(ARG_HELPS3_LONG) ||
		argsVariablesMap.count(ARG_HELPDISTRIBUTED_LONG) ||
		argsVariablesMap.count(ARG_HELPALLOPTIONS_LONG) )
		return true;

	return false;
}

/**
 * Print help text based on user-given selection.
 */
void ProgArgs::printHelp()
{
	if(argsVariablesMap.count(ARG_HELP_LONG) ||
		argsVariablesMap.count(ARG_HELP_SHORT) )
		printHelpOverview();
	else
	if(argsVariablesMap.count(ARG_HELPBLOCKDEV_LONG) ||
		argsVariablesMap.count(ARG_HELPLARGE_LONG) )
		printHelpBlockDev();
	else
	if(argsVariablesMap.count(ARG_HELPMULTIFILE_LONG) )
		printHelpMultiFile();
	else
#ifdef S3_SUPPORT
	if(argsVariablesMap.count(ARG_HELPS3_LONG) )
		printHelpS3();
	else
#endif // S3_SUPPORT
	if(argsVariablesMap.count(ARG_HELPDISTRIBUTED_LONG) )
		printHelpDistributed();
	else
	if(argsVariablesMap.count(ARG_HELPALLOPTIONS_LONG) )
		printHelpAllOptions();
}

void ProgArgs::printHelpOverview()
{
	std::cout <<
		EXE_NAME " - A distributed benchmark for files, objects and blocks" ENDL
		std::endl <<
		"Version: " EXE_VERSION ENDL
		std::endl <<
		"Tests include throughput, IOPS and access latency. Live statistics show how the" ENDL
		"system behaves under load and whether it is worth waiting for the end result." ENDL
		std::endl <<
		"Get started by selecting what you want to test..." ENDL
		std::endl <<
		"Large shared files or block devices (e.g. streaming or random IOPS):" ENDL
		"  $ " EXE_NAME " --" ARG_HELPLARGE_LONG ENDL
		std::endl <<
		"Multiple dirs and files per thread (e.g. lots of small files):" ENDL
		"  $ " EXE_NAME " --" ARG_HELPMULTIFILE_LONG ENDL
		std::endl <<
#ifdef S3_SUPPORT
		"S3 object storage:" ENDL
		"  $ " EXE_NAME " --" ARG_HELPS3_LONG ENDL
		std::endl <<
#endif // S3_SUPPORT
		"Multiple clients (e.g. shared file systems):" ENDL
		"  $ " EXE_NAME " --" ARG_HELPDISTRIBUTED_LONG ENDL
		std::endl <<
		"See all available options (e.g. csv file output):" ENDL
		"  $ " EXE_NAME " --" ARG_HELPALLOPTIONS_LONG ENDL
		std::endl <<
		"Happy benchmarking!" << std::endl;
}

void ProgArgs::printHelpAllOptions()
{
	std::cout <<
		"Overview of all available options." ENDL
		std::endl <<
		"Usage: ./" EXE_NAME " [OPTIONS] PATH [MORE_PATHS]" ENDL
		std::endl;

	std::cout << "All options in alphabetical order:" << std::endl;
	std::cout << argsGenericDescription << std::endl;
}

void ProgArgs::printHelpBlockDev()
{
	std::cout <<
		"Block device & large shared file testing." ENDL
		std::endl <<
		"Usage: ./" EXE_NAME " [OPTIONS] PATH [MORE_PATHS]" ENDL
		std::endl;

	bpo::options_description argsBlockdevBasicDescription(
		"Basic Options", Terminal::getTerminalLineLength(80) );

	argsBlockdevBasicDescription.add_options()
		(ARG_CREATEFILES_LONG "," ARG_CREATEFILES_SHORT, bpo::bool_switch(&this->runCreateFilesPhase),
			"Write to given block device(s) or file(s).")
		(ARG_READ_LONG "," ARG_READ_SHORT, bpo::bool_switch(&this->runReadPhase),
			"Read from given block device(s) or file(s).")
		(ARG_FILESIZE_LONG "," ARG_FILESIZE_SHORT, bpo::value(&this->fileSize),
			"Block device or file size to use. (Default: 0)")
		(ARG_BLOCK_LONG "," ARG_BLOCK_SHORT, bpo::value(&this->blockSize),
			"Number of bytes to read/write in a single operation. (Default: 1M)")
		(ARG_NUMTHREADS_LONG "," ARG_NUMTHREADS_SHORT, bpo::value(&this->numThreads),
			"Number of I/O worker threads. (Default: 1)")
    ;

    std::cout << argsBlockdevBasicDescription << std::endl;

	bpo::options_description argsBlockdevFrequentDescription(
		"Frequently Used Options", Terminal::getTerminalLineLength(80) );

	argsBlockdevFrequentDescription.add_options()
		(ARG_DIRECTIO_LONG, bpo::bool_switch(&this->useDirectIO),
			"Use direct IO to avoid buffering/caching.")
		(ARG_IODEPTH_LONG, bpo::value(&this->ioDepth),
			"Depth of I/O queue per thread for asynchronous read/write. Setting this to 2 or "
			"higher turns on async I/O. (Default: 1)")
		(ARG_RANDOMOFFSETS_LONG, bpo::bool_switch(&this->useRandomOffsets),
			"Read/write at random offsets.")
		(ARG_RANDOMAMOUNT_LONG, bpo::value(&this->randomAmount),
			"Number of bytes to write/read when using random offsets. (Default: Set to aggregate "
			"file size)")
		(ARG_RANDOMALIGN_LONG, bpo::bool_switch(&this->useRandomAligned),
			"Align random offsets to block size.")
		(ARG_LATENCY_LONG, bpo::bool_switch(&this->showLatency),
			"Show minimum, average and maximum latency for read/write operations.")
	;

    std::cout << argsBlockdevFrequentDescription << std::endl;

	bpo::options_description argsBlockdevMiscDescription(
		"Miscellaneous Options", Terminal::getTerminalLineLength(80) );

	argsBlockdevMiscDescription.add_options()
		(ARG_NUMAZONES_LONG, bpo::value(&this->numaZonesStr),
			"Comma-separated list of NUMA zones to bind this process to. If multiple zones are "
			"given, then worker threads are bound round-robin to the zones. "
			"(Hint: See 'lscpu' for available NUMA zones.)")
		(ARG_LATENCYPERCENTILES_LONG, bpo::bool_switch(&this->showLatencyPercentiles),
			"Show latency percentiles.")
		(ARG_LATENCYHISTOGRAM_LONG, bpo::bool_switch(&this->showLatencyHistogram),
			"Show latency histogram.")
		(ARG_SHOWALLELAPSED_LONG, bpo::bool_switch(&this->showAllElapsed),
			"Show elapsed time to completion of each I/O worker thread.")
	;

    std::cout << argsBlockdevMiscDescription << std::endl;

    std::cout <<
    	"Examples:" ENDL
		"  Sequentially write 4 large files and test random read IOPS for max 20 seconds:" ENDL
		"    $ " EXE_NAME " -w -b 4M -t 16 --direct -s 20g /mnt/myfs/file{1..4}" ENDL
		"    $ " EXE_NAME " -r -b 4k -t 16 --iodepth 16 --direct --rand --timelimit 20 \\" ENDL
		"        /mnt/myfs/file{1..4}" ENDL
		std::endl <<
		"  Test 4KiB multi-threaded write IOPS of devices /dev/nvme0n1 & /dev/nvme1n1:" ENDL
		"    $ " EXE_NAME " -w -b 4K -t 16 --iodepth 16 --direct --rand \\" ENDL
		"        /dev/nvme0n1 /dev/nvme1n1" ENDL
		std::endl <<
		"  Test 4KiB block random read latency of device /dev/nvme0n1:" ENDL
		"    $ " EXE_NAME " -r -b 4K --lat --direct --rand /dev/nvme0n1" ENDL
#ifdef CUDA_SUPPORT
		std::endl <<
		"  Stream data from large file into memory of first 2 GPUs via CUDA:" ENDL
		"    $ " EXE_NAME " -r -b 1M -t 8 --gpuids 0,1 \\" ENDL
		"        /mnt/myfs/file1" ENDL
#endif
#ifdef CUFILE_SUPPORT
		std::endl <<
		"  Stream data from large file into memory of first 2 GPUs via GPUDirect Storage:" ENDL
		"    $ " EXE_NAME " -r -b 1M -t 8 --gpuids 0,1 --gds \\" ENDL
		"        /mnt/myfs/file1" ENDL
#endif
		std::endl;
}

void ProgArgs::printHelpMultiFile()
{
	std::cout <<
		"Multi-file / multi-directory testing." ENDL
		std::endl <<
		"Usage: ./" EXE_NAME " [OPTIONS] DIRECTORY [MORE_DIRECTORIES]" ENDL
		std::endl;

	bpo::options_description argsMultiFileBasicDescription(
		"Basic Options", Terminal::getTerminalLineLength(80) );

	argsMultiFileBasicDescription.add_options()
		(ARG_CREATEDIRS_LONG "," ARG_CREATEDIRS_SHORT, bpo::bool_switch(&this->runCreateDirsPhase),
			"Create directories. (Already existing dirs are not treated as error.)")
		(ARG_CREATEFILES_LONG "," ARG_CREATEFILES_SHORT,
			bpo::bool_switch(&this->runCreateFilesPhase),
			"Write files. Create them if they don't exist.")
		(ARG_READ_LONG "," ARG_READ_SHORT, bpo::bool_switch(&this->runReadPhase),
			"Read files.")
		(ARG_STATFILES_LONG, bpo::bool_switch(&this->runStatFilesPhase),
			"Read file status attributes (file size, owner etc).")
		(ARG_DELETEFILES_LONG "," ARG_DELETEFILES_SHORT,
			bpo::bool_switch(&this->runDeleteFilesPhase),
			"Delete files.")
		(ARG_DELETEDIRS_LONG "," ARG_DELETEDIRS_SHORT, bpo::bool_switch(&this->runDeleteDirsPhase),
			"Delete directories.")
		(ARG_NUMTHREADS_LONG "," ARG_NUMTHREADS_SHORT, bpo::value(&this->numThreads),
			"Number of I/O worker threads. (Default: 1)")
		(ARG_NUMDIRS_LONG "," ARG_NUMDIRS_SHORT, bpo::value(&this->numDirs),
			"Number of directories per I/O worker thread. This can be 0 to disable creation of any "
			"subdirs, in which case all workers share the given dir. (Default: 1)")
		(ARG_NUMFILES_LONG "," ARG_NUMFILES_SHORT, bpo::value(&this->numFilesOrigStr),
			"Number of files per thread per directory. (Default: 1) Example: \""
			"-" ARG_NUMTHREADS_SHORT "2 -" ARG_NUMDIRS_SHORT "3 -" ARG_NUMFILES_SHORT "4\" will "
			"use 2x3x4=24 files.")
		(ARG_FILESIZE_LONG "," ARG_FILESIZE_SHORT, bpo::value(&this->fileSize),
			"File size. (Default: 0)")
		(ARG_BLOCK_LONG "," ARG_BLOCK_SHORT, bpo::value(&this->blockSize),
			"Number of bytes to read/write in a single operation. (Default: 1M)")
    ;

    std::cout << argsMultiFileBasicDescription << std::endl;

	bpo::options_description argsMultiFileFrequentDescription(
		"Frequently Used Options", Terminal::getTerminalLineLength(80) );

	argsMultiFileFrequentDescription.add_options()
		(ARG_DIRECTIO_LONG, bpo::bool_switch(&this->useDirectIO),
			"Use direct IO.")
		(ARG_IODEPTH_LONG, bpo::value(&this->ioDepth),
			"Depth of I/O queue per thread for asynchronous read/write. Setting this to 2 or "
			"higher turns on async I/O. (Default: 1)")
		(ARG_LATENCY_LONG, bpo::bool_switch(&this->showLatency),
			"Show minimum, average and maximum latency for read/write operations and entries. "
			"In read and write phases, entry latency includes file open, read/write and close.")
	;

    std::cout << argsMultiFileFrequentDescription << std::endl;

	bpo::options_description argsMultiFileMiscDescription(
		"Miscellaneous Options", Terminal::getTerminalLineLength(80) );

	argsMultiFileMiscDescription.add_options()
		(ARG_NUMAZONES_LONG, bpo::value(&this->numaZonesStr),
			"Comma-separated list of NUMA zones to bind this process to. If multiple zones are "
			"given, then worker threads are bound round-robin to the zones. "
			"(Hint: See 'lscpu' for available NUMA zones.)")
		(ARG_LATENCYPERCENTILES_LONG, bpo::bool_switch(&this->showLatencyPercentiles),
			"Show latency percentiles.")
		(ARG_LATENCYHISTOGRAM_LONG, bpo::bool_switch(&this->showLatencyHistogram),
			"Show latency histogram.")
		(ARG_IGNOREDELERR_LONG, bpo::bool_switch(&this->ignoreDelErrors),
			"Ignore not existing files/dirs in deletion phase instead of treating this as error.")
	;

    std::cout << argsMultiFileMiscDescription << std::endl;

    std::cout <<
    	"Examples:" ENDL
		"  Test 2 threads, each creating 3 directories with 4 1MiB files inside:" ENDL
		"    $ " EXE_NAME " -w -d -t 2 -n 3 -N 4 -s 1m -b 1m /data/testdir" ENDL
		std::endl <<
		"  Same as above with long option names:" ENDL
		"    $ " EXE_NAME " --write --mkdirs --threads 2 --dirs 3 --files 4 --size 1m \\" ENDL
		"        --block 1m /data/testdir" ENDL
		std::endl <<
		"  Test 2 threads, each reading 4 1MB files from 3 directories in 128KiB blocks:" ENDL
		"    $ " EXE_NAME " -r -t 2 -n 3 -N 4 -s 1m -b 128k /data/testdir" ENDL
#ifdef CUDA_SUPPORT
		std::endl <<
		"  As above, but also copy data into memory of first 2 GPUs via CUDA:" ENDL
		"    $ " EXE_NAME " -r -t 2 -n 3 -N 4 -s 1m -b 128k \\" ENDL
		"        --gpuids 0,1 /data/testdir" ENDL
#endif
#ifdef CUFILE_SUPPORT
		std::endl <<
		"  As above, but read data into memory of first 2 GPUs via GPUDirect Storage:" ENDL
		"    $ " EXE_NAME " -r -t 2 -n 3 -N 4 -s 1m -b 128k \\" ENDL
		"        --gpuids 0,1 --gds /data/testdir" ENDL
#endif
		std::endl <<
		"  Delete files and directories created by example above:" ENDL
		"    $ " EXE_NAME " -F -D -t 2 -n 3 -N 4 /data/testdir" <<
		std::endl;
}

void ProgArgs::printHelpS3()
{
	std::cout <<
		"S3 object storage testing. (The options here are intentionally similar to" ENDL
		"\"--" ARG_HELPMULTIFILE_LONG "\" to enable multi-protocol storage testing.)" ENDL
		std::endl <<
		"Usage: ./" EXE_NAME " [OPTIONS] BUCKET [MORE_BUCKETS]" ENDL
		std::endl;

	bpo::options_description argsS3ServiceArgsDescription(
		"S3 Service Arguments", Terminal::getTerminalLineLength(80) );

	argsS3ServiceArgsDescription.add_options()
		(ARG_S3ENDPOINTS_LONG, bpo::value(&this->s3EndpointsStr),
			"Comma-separated list of S3 endpoints. (Format: [http(s)://]hostname[:port])")
		(ARG_S3ACCESSKEY_LONG, bpo::value(&this->s3AccessKey),
			"S3 access key.")
		(ARG_S3ACCESSSECRET_LONG, bpo::value(&this->s3AccessSecret),
			"S3 access secret.")
    ;

    std::cout << argsS3ServiceArgsDescription << std::endl;

	bpo::options_description argsS3BasicDescription(
		"Basic Options", Terminal::getTerminalLineLength(80) );

	argsS3BasicDescription.add_options()
		(ARG_CREATEDIRS_LONG "," ARG_CREATEDIRS_SHORT, bpo::bool_switch(&this->runCreateDirsPhase),
			"Create buckets. (Already existing buckets are not treated as error.)")
		(ARG_CREATEFILES_LONG "," ARG_CREATEFILES_SHORT,
			bpo::bool_switch(&this->runCreateFilesPhase),
			"Write/upload objects.")
		(ARG_READ_LONG "," ARG_READ_SHORT, bpo::bool_switch(&this->runReadPhase),
			"Read/download objects.")
		(ARG_STATFILES_LONG, bpo::bool_switch(&this->runStatFilesPhase),
			"Read object status attributes (size etc).")
		(ARG_DELETEFILES_LONG "," ARG_DELETEFILES_SHORT,
			bpo::bool_switch(&this->runDeleteFilesPhase),
			"Delete objects.")
		(ARG_DELETEDIRS_LONG "," ARG_DELETEDIRS_SHORT, bpo::bool_switch(&this->runDeleteDirsPhase),
			"Delete buckets.")
		(ARG_NUMTHREADS_LONG "," ARG_NUMTHREADS_SHORT, bpo::value(&this->numThreads),
			"Number of I/O worker threads. (Default: 1)")
		(ARG_NUMDIRS_LONG "," ARG_NUMDIRS_SHORT, bpo::value(&this->numDirs),
			"Number of directories per I/O worker thread. Directories are slash-separated object "
			"key prefixes. This can be 0 to disable creation of any subdirs. (Default: 1)")
		(ARG_NUMFILES_LONG "," ARG_NUMFILES_SHORT, bpo::value(&this->numFilesOrigStr),
			"Number of objects per thread per directory. (Default: 1) Example: \""
			"-" ARG_NUMTHREADS_SHORT "2 -" ARG_NUMDIRS_SHORT "3 -" ARG_NUMFILES_SHORT "4\" will "
			"use 2x3x4=24 objects.")
		(ARG_FILESIZE_LONG "," ARG_FILESIZE_SHORT, bpo::value(&this->fileSize),
			"Object size. (Default: 0)")
		(ARG_BLOCK_LONG "," ARG_BLOCK_SHORT, bpo::value(&this->blockSize),
			"Number of bytes to read/write in a single operation. (Default: 1M)")
    ;

    std::cout << argsS3BasicDescription << std::endl;

	bpo::options_description argsS3FrequentDescription(
		"Frequently Used Options", Terminal::getTerminalLineLength(80) );

	argsS3FrequentDescription.add_options()
		(ARG_S3FASTGET_LONG, bpo::bool_switch(&this->useS3TransferManager),
			"Send downloaded objects directly to /dev/null instead of a memory buffer. This option "
			"is incompatible with any buffer post-processing options like data verification or "
			"GPU data transfer.")
		(ARG_TREEFILE_LONG, bpo::value(&this->treeFilePath),
			"The path to a treefile containing a list of object names to use for shared upload or "
			"download if the object size exceeds \"--" ARG_FILESHARESIZE_LONG "\".")
		(ARG_LATENCY_LONG, bpo::bool_switch(&this->showLatency),
			"Show minimum, average and maximum latency for PUT/GET operations and for complete "
			"upload/download in case of chunked transfers.")
	;

    std::cout << argsS3FrequentDescription << std::endl;

	bpo::options_description argsS3MiscDescription(
		"Miscellaneous Options", Terminal::getTerminalLineLength(80) );

	argsS3MiscDescription.add_options()
		(ARG_FILESHARESIZE_LONG, bpo::value(&this->fileShareSizeOrigStr),
			"In custom tree mode or when object keys are given directly as arguments, this defines "
			"the object size as of which multiple threads are used to upload/download an object. "
			"(Default: 0, which means " FILESHAREBLOCKFACTOR_STR " x blocksize)")
		(ARG_S3REGION_LONG, bpo::value(&this->s3Region),
			"S3 region.")
		(ARG_NUMAZONES_LONG, bpo::value(&this->numaZonesStr),
			"Comma-separated list of NUMA zones to bind this process to. If multiple zones are "
			"given, then worker threads are bound round-robin to the zones. "
			"(Hint: See 'lscpu' for available NUMA zones.)")
	;

    std::cout << argsS3MiscDescription << std::endl;

    std::cout <<
    	"Examples:" ENDL
		"  Create bucket \"mybucket\":" ENDL
		"    $ " EXE_NAME " --s3endpoints http://S3SERVER --s3key S3KEY --s3secret S3SECRET \\" ENDL
		"        -d mybucket" ENDL
		std::endl <<
		"  Test 2 threads, each creating 3 directories with 4 10MiB objects inside:" ENDL
		"    $ " EXE_NAME " --s3endpoints http://S3SERVER --s3key S3KEY --s3secret S3SECRET \\" ENDL
		"        -w -t 2 -n 3 -N 4 -s 10m -b 5m mybucket" ENDL
		std::endl <<
		"  Delete objects and bucket created by example above:" ENDL
		"    $ " EXE_NAME " --s3endpoints http://S3SERVER --s3key S3KEY --s3secret S3SECRET \\" ENDL
		"        -D -F -t 2 -n 3 -N 4 mybucket" ENDL
		std::endl <<
		"  Shared upload of 4 1GiB objects via 8 threads in 16MiB blocks:" ENDL
		"    $ " EXE_NAME " --s3endpoints http://S3SERVER --s3key S3KEY --s3secret S3SECRET \\" ENDL
		"        -w -t 8 -s 1g -b 16m mybucket/myobject{1..4}" <<
		std::endl;
}

void ProgArgs::printHelpDistributed()
{
	std::cout <<
		"Distributed benchmarking with multiple clients." ENDL
		std::endl <<
		"Usage:" ENDL
		"  First, start " EXE_NAME " in service mode on multiple hosts:" ENDL
		"  $ " EXE_NAME " --service [OPTIONS]" ENDL
		std::endl <<
		"  Then run master anywhere on the network to start benchmarks on service hosts:" ENDL
		"  $ " EXE_NAME " --hosts HOST_1,...,HOST_N [OPTIONS] PATH [MORE_PATHS]" ENDL
		std::endl <<
		"  When you're done, quit all services:" ENDL
		"  $ " EXE_NAME " --hosts HOST_1,...,HOST_N --quit" ENDL
		std::endl;

	bpo::options_description argsDistributedBasicDescription(
		"Basic Options", Terminal::getTerminalLineLength(80) );

	argsDistributedBasicDescription.add_options()
		(ARG_HOSTS_LONG, bpo::value(&this->hostsStr),
			"Comma-separated list of hosts in service mode for coordinated benchmark. When this "
			"argument is used, this program instance runs in master mode to coordinate the given "
			"service mode hosts. The given number of threads, dirs and files is per-service then. "
			"(Format: hostname[:port])")
		(ARG_RUNASSERVICE_LONG, bpo::bool_switch(&this->runAsService),
			"Run as service for distributed mode, waiting for requests from master.")
		(ARG_QUIT_LONG, bpo::bool_switch(&this->quitServices),
			"Quit services on given service mode hosts.")
    ;

    std::cout << argsDistributedBasicDescription << std::endl;

	bpo::options_description argsDistributedFrequentDescription(
		"Frequently Used Options", Terminal::getTerminalLineLength(80) );

	argsDistributedFrequentDescription.add_options()
		(ARG_NUMAZONES_LONG, bpo::value(&this->numaZonesStr),
			"Comma-separated list of NUMA zones to bind this service to. If multiple zones are "
			"given, then worker threads are bound round-robin to the zones. "
			"(Hint: See 'lscpu' for available NUMA zones.)")
		(ARG_SERVICEPORT_LONG, bpo::value(&this->servicePort),
			"TCP communication port of service. (Default: " ARGDEFAULT_SERVICEPORT_STR ") "
			"Different ports can be  used to run multiple service instances on different NUMA "
			"zones of a host.")
	;

    std::cout << argsDistributedFrequentDescription << std::endl;

	bpo::options_description argsDistributedMiscDescription(
		"Miscellaneous Options", Terminal::getTerminalLineLength(80) );

	argsDistributedMiscDescription.add_options()
		(ARG_NOSVCPATHSHARE_LONG, bpo::bool_switch(&this->noSharedServicePath),
			"Benchmark paths are not shared between service hosts. Thus, each service host will"
			"work on the full given dataset instead of its own fraction of the data set.")
		(ARG_SHOWSVCELAPSED_LONG, bpo::bool_switch(&this->showServicesElapsed),
			"Show elapsed time to completion of each service instance ordered by slowest thread.")
		(ARG_INTERRUPT_LONG, bpo::bool_switch(&this->interruptServices),
			"Interrupt current benchmark phase on given service mode hosts.")
		(ARG_FOREGROUNDSERVICE_LONG, bpo::bool_switch(&this->runServiceInForeground),
			"When running as service, stay in foreground and connected to console instead of "
			"detaching from console and daemonizing into backgorund.")
	;

    std::cout << argsDistributedMiscDescription << std::endl;

    std::cout <<
    	"Examples:" ENDL
		"  Start services on hosts node001 and node002:" ENDL
		"    $ ssh node001 " EXE_NAME " --service" ENDL
		"    $ ssh node002 " EXE_NAME " --service" ENDL
		std::endl <<
		"  Run distributed test on node001 and node002, using 4 threads per service" ENDL
		"  instance and creating 8 dirs per thread, each containing 16 1MiB files:" ENDL
		"    $ " EXE_NAME " --hosts node001,node002 \\" ENDL
		"        -t 4 -d -n 8 -w -N 16 -s 1M /data/testdir" ENDL
		std::endl <<
		"  Quit services on host node001 and node002:" ENDL
		"    $ " EXE_NAME " --hosts node001,node002 --quit" <<
		std::endl;
}

/**
 * Check if user gave the argument to print version and build info. If this returns true, then the
 * rest of the settings in this class is not initialized, so may not be used.
 *
 * @return true if version info was requested.
 */
bool ProgArgs::hasUserRequestedVersion()
{
	if(argsVariablesMap.count(ARG_VERSION_LONG) )
		return true;

	return false;
}

/**
 * Check if user gave the argument to do only a dry run.
 *
 * @return true if dry run requested.
 */
bool ProgArgs::hasUserRequestedDryRun()
{
	if(argsVariablesMap.count(ARG_DRYRUN_LONG) )
		return true;

	return false;
}

/**
 * Print version and included optional build features.
 */
void ProgArgs::printVersionAndBuildInfo()
{
	std::ostringstream includedStream; // included optional build features
	std::ostringstream notIncludedStream; // not included optional build features

	std::cout << EXE_NAME << std::endl;
	std::cout << " * Version: " EXE_VERSION << std::endl;
	std::cout << " * Net protocol version: " HTTP_PROTOCOLVERSION << std::endl;
	std::cout << " * Build date: " __DATE__ << " " << __TIME__ << std::endl;

#ifdef ALTHTTPSVC_SUPPORT
	includedStream << "althttpsvc ";
#else
	notIncludedStream << "althttpsvc ";
#endif

#ifdef BACKTRACE_SUPPORT
	includedStream << "backtrace ";
#else
	notIncludedStream << "backtrace ";
#endif

#ifdef COREBIND_SUPPORT
	includedStream << "corebind ";
#else
	notIncludedStream << "corebind ";
#endif

#ifdef CUDA_SUPPORT
	includedStream << "cuda ";
#else
	notIncludedStream << "cuda ";
#endif

#ifdef CUFILE_SUPPORT
	includedStream << "cufile/gds ";
#else
	notIncludedStream << "cufile/gds ";
#endif

#ifdef HDFS_SUPPORT
	includedStream << "hdfs ";
#else
	notIncludedStream << "hdfs ";
#endif

#ifdef LIBAIO_SUPPORT
	includedStream << "libaio ";
#else
	notIncludedStream << "libaio ";
#endif

#ifdef LIBNUMA_SUPPORT
	includedStream << "libnuma ";
#else
	notIncludedStream << "libnuma ";
#endif

#ifdef USE_MIMALLOC
	includedStream << "mimalloc ";
#else
	notIncludedStream << "mimalloc ";
#endif

#ifdef NCURSES_SUPPORT
	includedStream << "ncurses ";
#else
	notIncludedStream << "ncurses ";
#endif

#ifdef S3_SUPPORT
	includedStream << "s3 ";
#else
	notIncludedStream << "s3 ";
#endif

#ifdef SYNCFS_SUPPORT
	includedStream << "syncfs ";
#else
	notIncludedStream << "syncfs ";
#endif

#ifdef SYSCALLH_SUPPORT
	includedStream << "syscallh ";
#else
	notIncludedStream << "syscallh ";
#endif

	std::cout << " * Included optional build features: " <<
		(includedStream.str().empty() ? "-" : includedStream.str() ) << std::endl;
	std::cout << " * Excluded optional build features: " <<
		(notIncludedStream.str().empty() ? "-" : notIncludedStream.str() ) << std::endl;

	std::cout << " * System steady clock precision: " <<
		( (double)std::chrono::steady_clock::period::num / std::chrono::steady_clock::period::den)
		<< " sec" << std::endl;
}

/**
 * Sets the arguments that are relevant for a new benchmark in service mode.
 *
 * @throw ProgException if configuration error is detected or if bench dirs cannot be accessed.
 */
void ProgArgs::setFromPropertyTreeForService(bpt::ptree& tree)
{
	// note: alphabetical order by variable name ("benchLabel", "benchPathStr" etc)

	benchLabel = tree.get<std::string>(ARG_BENCHLABEL_LONG);
	benchPathStr = tree.get<std::string>(ARG_BENCHPATHS_LONG);
	blockSize = tree.get<size_t>(ARG_BLOCK_LONG);
	blockVarianceAlgo = tree.get<std::string>(ARG_BLOCKVARIANCEALGO_LONG);
	blockVariancePercent = tree.get<unsigned>(ARG_BLOCKVARIANCE_LONG);
	doDirectVerify = tree.get<bool>(ARG_VERIFYDIRECT_LONG);
	doDirSharing = tree.get<bool>(ARG_DIRSHARING_LONG);
	doInfiniteIOLoop = tree.get<bool>(ARG_INFINITEIOLOOP_LONG);
	doPreallocFile = tree.get<bool>(ARG_PREALLOCFILE_LONG);
	doReadInline = tree.get<bool>(ARG_READINLINE_LONG);
	doReverseSeqOffsets = tree.get<bool>(ARG_REVERSESEQOFFSETS_LONG);
	doS3AclVerify = tree.get<bool>(ARG_S3ACLVERIFY_LONG);
	doS3ListObjVerify = tree.get<bool>(ARG_S3LISTOBJVERIFY_LONG);
	doStatInline = tree.get<bool>(ARG_STATFILESINLINE_LONG);
	doTruncate = tree.get<bool>(ARG_TRUNCATE_LONG);
	doTruncToSize = tree.get<bool>(ARG_TRUNCTOSIZE_LONG);
	fadviseFlags = tree.get<unsigned>(ARG_FADVISE_LONG);
	fileShareSize = tree.get<uint64_t>(ARG_FILESHARESIZE_LONG);
	fileSize = tree.get<uint64_t>(ARG_FILESIZE_LONG);
	gpuIDsStr = tree.get<std::string>(ARG_GPUIDS_LONG);
	ignore0USecErrors = tree.get<bool>(ARG_IGNORE0USECERR_LONG);
	ignoreDelErrors = tree.get<bool>(ARG_IGNOREDELERR_LONG);
	integrityCheckSalt = tree.get<uint64_t>(ARG_INTEGRITYCHECK_LONG);
	ioDepth = tree.get<size_t>(ARG_IODEPTH_LONG);
	limitReadBps = tree.get<uint64_t>(ARG_LIMITREAD_LONG);
	limitWriteBps = tree.get<uint64_t>(ARG_LIMITWRITE_LONG);
	madviseFlags = tree.get<unsigned>(ARG_MADVISE_LONG);
	netBenchRespSize = tree.get<size_t>(ARG_RESPSIZE_LONG);
	netBenchServersStr = tree.get<std::string>(ARG_NETBENCHSERVERSSTR_LONG);
	noDirectIOCheck = tree.get<bool>(ARG_NODIRECTIOCHECK_LONG);
	numDirs = tree.get<size_t>(ARG_NUMDIRS_LONG);
	numFiles = tree.get<size_t>(ARG_NUMFILES_LONG);
	numNetBenchServers = tree.get<unsigned>(ARG_NUMNETBENCHSERVERS_LONG);
	numRWMixReadThreads = tree.get<size_t>(ARG_RWMIXTHREADS_LONG);
	numThreads = tree.get<size_t>(ARG_NUMTHREADS_LONG);
	opsLogPath = tree.get<std::string>(ARG_OPSLOGPATH_LONG);
	randOffsetAlgo = tree.get<std::string>(ARG_RANDSEEKALGO_LONG);
	randomAmount = tree.get<uint64_t>(ARG_RANDOMAMOUNT_LONG);
	runCreateDirsPhase = tree.get<bool>(ARG_CREATEDIRS_LONG);
	runCreateFilesPhase = tree.get<bool>(ARG_CREATEFILES_LONG);
	runDeleteDirsPhase = tree.get<bool>(ARG_DELETEDIRS_LONG);
	runDeleteFilesPhase = tree.get<bool>(ARG_DELETEFILES_LONG);
	runDropCachesPhase = tree.get<bool>(ARG_DROPCACHESPHASE_LONG);
	runReadPhase = tree.get<bool>(ARG_READ_LONG);
	runS3AclGet = tree.get<bool>(ARG_S3ACLGET_LONG);
	runS3AclPut = tree.get<bool>(ARG_S3ACLPUT_LONG);
	runS3BucketAclGet = tree.get<bool>(ARG_S3BUCKETACLGET_LONG);
	runS3BucketAclPut = tree.get<bool>(ARG_S3BUCKETACLPUT_LONG);
	runS3ListObjNum = tree.get<uint64_t>(ARG_S3LISTOBJ_LONG);
	runS3ListObjParallel = tree.get<bool>(ARG_S3LISTOBJPARALLEL_LONG);
	runS3MultiDelObjNum = tree.get<uint64_t>(ARG_S3MULTIDELETE_LONG);
	runStatFilesPhase = tree.get<bool>(ARG_STATFILES_LONG);
	runSyncPhase = tree.get<bool>(ARG_SYNCPHASE_LONG);
	rwMixPercent = tree.get<unsigned>(ARG_RWMIXPERCENT_LONG);
	s3AccessKey = tree.get<std::string>(ARG_S3ACCESSKEY_LONG);
	s3AccessSecret = tree.get<std::string>(ARG_S3ACCESSSECRET_LONG);
	s3AclGrantee = tree.get<std::string>(ARG_S3ACLGRANTEE_LONG);
	s3AclGranteePermissions = tree.get<std::string>(ARG_S3ACLGRANTS_LONG);
	s3AclGranteeType = tree.get<std::string>(ARG_S3ACLGRANTEETYPE_LONG);
	s3EndpointsStr = tree.get<std::string>(ARG_S3ENDPOINTS_LONG);
	s3ObjectPrefix = tree.get<std::string>(ARG_S3OBJECTPREFIX_LONG);
	s3Region = tree.get<std::string>(ARG_S3REGION_LONG);
	s3SignPolicy = tree.get<unsigned short>(ARG_S3SIGNPAYLOAD_LONG);
	sockRecvBufSize = tree.get<int>(ARG_RECVBUFSIZE_LONG);
	sockSendBufSize = tree.get<int>(ARG_SENDBUFSIZE_LONG);
	treeRoundUpSize = tree.get<uint64_t>(ARG_TREEROUNDUP_LONG);
	useCuFile = tree.get<bool>(ARG_CUFILE_LONG);
	useCuFileDriverOpen = tree.get<bool>(ARG_CUFILEDRIVEROPEN_LONG);
	useCuHostBufReg = tree.get<bool>(ARG_CUHOSTBUFREG_LONG);
	useCustomTreeRandomize = tree.get<bool>(ARG_TREERANDOMIZE_LONG);
	useDirectIO = tree.get<bool>(ARG_DIRECTIO_LONG);
	useGDSBufReg = tree.get<bool>(ARG_GDSBUFREG_LONG);
	useHDFS = tree.get<bool>(ARG_HDFS_LONG);
	useMmap = tree.get<bool>(ARG_MMAP_LONG);
	useNetBench = tree.get<bool>(ARG_NETBENCH_LONG);
	useNoFDSharing = tree.get<bool>(ARG_NOFDSHARING_LONG);
	useOpsLogLocking = tree.get<bool>(ARG_OPSLOGLOCKING_LONG);
	useRandomAligned = tree.get<bool>(ARG_RANDOMALIGN_LONG);
	useRandomOffsets = tree.get<bool>(ARG_RANDOMOFFSETS_LONG);
	useS3FastRead = tree.get<bool>(ARG_S3FASTGET_LONG);
	useS3RandObjSelect = tree.get<bool>(ARG_S3RANDOBJ_LONG);
	useS3TransferManager = tree.get<bool>(ARG_S3TRANSMAN_LONG);

	// dynamically calculated values for service hosts...

	rankOffset = tree.get<size_t>(ARG_RANKOFFSET_LONG);

	numDataSetThreads = tree.get<size_t>(ARG_NUMDATASETTHREADS_LONG);

	// prepend upload dir to tree file
	treeFilePath = tree.get<std::string>(ARG_TREEFILE_LONG);
	if(!treeFilePath.empty() )
	{
		char* filenameDup = strdup(treeFilePath.c_str() );
		if(!filenameDup)
			throw ProgException("Failed to alloc mem for filename dup: " + treeFilePath);

		// note: basename() ensures that there is no "../" or subdirs in the given filename
		std::string filename = basename(filenameDup);

		free(filenameDup);

		treeFilePath = SERVICE_UPLOAD_BASEPATH(servicePort) + "/" + filename;

		loadCustomTreeFile();
	}

	parseS3Endpoints();
	parseNetBenchServersForService();

	// rebuild benchPathsVec/benchPathFDsVec and check if bench dirs are accessible
	parseAndCheckPaths();

	checkPathDependentArgs();

	// apply GPU IDs override if given
	if(!gpuIDsStr.empty() && !gpuIDsServiceOverride.empty() )
		gpuIDsStr = gpuIDsServiceOverride;

	parseGPUIDs();

	// init implict vals
	useS3ObjectPrefixRand = (s3ObjectPrefix.find(RAND_PREFIX_MARKS_SUBSTR) != std::string::npos);
}

/**
 * Gets the arguments that are relevant for a new benchmark in service mode.
 *
 * @workerRank to calc rankOffset for host (unless user requested all to work on same dataset)
 */
void ProgArgs::getAsPropertyTreeForService(bpt::ptree& outTree, size_t serviceRank) const
{
	// note: alphabetical order by ARG_... name

	outTree.put(ARG_BLOCK_LONG, blockSize);
	outTree.put(ARG_BLOCKVARIANCE_LONG, blockVariancePercent);
	outTree.put(ARG_BLOCKVARIANCEALGO_LONG, blockVarianceAlgo);
	outTree.put(ARG_BENCHLABEL_LONG, benchLabel);
	outTree.put(ARG_BENCHPATHS_LONG, benchPathStr);
	outTree.put(ARG_CREATEDIRS_LONG, runCreateDirsPhase);
	outTree.put(ARG_CREATEFILES_LONG, runCreateFilesPhase);
	outTree.put(ARG_CUFILE_LONG, useCuFile);
	outTree.put(ARG_CUFILEDRIVEROPEN_LONG, useCuFileDriverOpen);
	outTree.put(ARG_CUHOSTBUFREG_LONG, useCuHostBufReg);
	outTree.put(ARG_DELETEDIRS_LONG, runDeleteDirsPhase);
	outTree.put(ARG_DELETEFILES_LONG, runDeleteFilesPhase);
	outTree.put(ARG_DIRSHARING_LONG, doDirSharing);
	outTree.put(ARG_DIRECTIO_LONG, useDirectIO);
	outTree.put(ARG_DROPCACHESPHASE_LONG, runDropCachesPhase);
	outTree.put(ARG_FADVISE_LONG, fadviseFlags);
	outTree.put(ARG_FILESHARESIZE_LONG, fileShareSize);
	outTree.put(ARG_FILESIZE_LONG, fileSize);
	outTree.put(ARG_GDSBUFREG_LONG, useGDSBufReg);
	outTree.put(ARG_HDFS_LONG, useHDFS);
	outTree.put(ARG_IGNORE0USECERR_LONG, ignore0USecErrors);
	outTree.put(ARG_IGNOREDELERR_LONG, ignoreDelErrors);
	outTree.put(ARG_INFINITEIOLOOP_LONG, doInfiniteIOLoop);
	outTree.put(ARG_INTEGRITYCHECK_LONG, integrityCheckSalt);
	outTree.put(ARG_IODEPTH_LONG, ioDepth);
	outTree.put(ARG_LIMITREAD_LONG, limitReadBps);
	outTree.put(ARG_LIMITWRITE_LONG, limitWriteBps);
	outTree.put(ARG_MADVISE_LONG, madviseFlags);
	outTree.put(ARG_MMAP_LONG, useMmap);
	outTree.put(ARG_NETBENCH_LONG, useNetBench);
	outTree.put(ARG_NETBENCHSERVERSSTR_LONG, serversStr);
	outTree.put(ARG_NUMDATASETTHREADS_LONG, numDataSetThreads);
	outTree.put(ARG_NUMDIRS_LONG, numDirs);
	outTree.put(ARG_NUMFILES_LONG, numFiles);
	outTree.put(ARG_NUMNETBENCHSERVERS_LONG, numNetBenchServers);
	outTree.put(ARG_NUMTHREADS_LONG, numThreads);
	outTree.put(ARG_NOFDSHARING_LONG, useNoFDSharing);
	outTree.put(ARG_NODIRECTIOCHECK_LONG, noDirectIOCheck);
	outTree.put(ARG_OPSLOGLOCKING_LONG, useOpsLogLocking);
	outTree.put(ARG_OPSLOGPATH_LONG, opsLogPath);
	outTree.put(ARG_PREALLOCFILE_LONG, doPreallocFile);
	outTree.put(ARG_RANDOMALIGN_LONG, useRandomAligned);
	outTree.put(ARG_RANDOMAMOUNT_LONG, randomAmount);
	outTree.put(ARG_RANDOMOFFSETS_LONG, useRandomOffsets);
	outTree.put(ARG_RANDSEEKALGO_LONG, randOffsetAlgo);
	outTree.put(ARG_READ_LONG, runReadPhase);
	outTree.put(ARG_READINLINE_LONG, doReadInline);
	outTree.put(ARG_RECVBUFSIZE_LONG, sockRecvBufSize);
	outTree.put(ARG_RESPSIZE_LONG, netBenchRespSize);
	outTree.put(ARG_REVERSESEQOFFSETS_LONG, doReverseSeqOffsets);
	outTree.put(ARG_RWMIXPERCENT_LONG, rwMixPercent);
	outTree.put(ARG_RWMIXTHREADS_LONG, numRWMixReadThreads);
	outTree.put(ARG_S3ACCESSKEY_LONG, s3AccessKey);
	outTree.put(ARG_S3ACCESSSECRET_LONG, s3AccessSecret);
	outTree.put(ARG_S3ACLGET_LONG, runS3AclGet);
	outTree.put(ARG_S3ACLGRANTEE_LONG, s3AclGrantee);
	outTree.put(ARG_S3ACLGRANTEETYPE_LONG, s3AclGranteeType);
	outTree.put(ARG_S3ACLGRANTS_LONG, s3AclGranteePermissions);
	outTree.put(ARG_S3ACLPUT_LONG, runS3AclPut);
	outTree.put(ARG_S3ACLVERIFY_LONG, doS3AclVerify);
	outTree.put(ARG_S3BUCKETACLGET_LONG, runS3BucketAclGet);
	outTree.put(ARG_S3BUCKETACLPUT_LONG, runS3BucketAclPut);
	outTree.put(ARG_S3ENDPOINTS_LONG, s3EndpointsStr);
	outTree.put(ARG_S3FASTGET_LONG, useS3FastRead);
	outTree.put(ARG_S3LISTOBJ_LONG, runS3ListObjNum);
	outTree.put(ARG_S3LISTOBJPARALLEL_LONG, runS3ListObjParallel);
	outTree.put(ARG_S3LISTOBJVERIFY_LONG, doS3ListObjVerify);
	outTree.put(ARG_S3MULTIDELETE_LONG, runS3MultiDelObjNum);
	outTree.put(ARG_S3OBJECTPREFIX_LONG, s3ObjectPrefix);
	outTree.put(ARG_S3RANDOBJ_LONG, useS3RandObjSelect);
	outTree.put(ARG_S3REGION_LONG, s3Region);
	outTree.put(ARG_S3SIGNPAYLOAD_LONG, s3SignPolicy);
	outTree.put(ARG_S3TRANSMAN_LONG, useS3TransferManager);
	outTree.put(ARG_SENDBUFSIZE_LONG, sockSendBufSize);
	outTree.put(ARG_STATFILES_LONG, runStatFilesPhase);
	outTree.put(ARG_STATFILESINLINE_LONG, doStatInline);
	outTree.put(ARG_SYNCPHASE_LONG, runSyncPhase);
	outTree.put(ARG_TRUNCATE_LONG, doTruncate);
	outTree.put(ARG_TRUNCTOSIZE_LONG, doTruncToSize);
	outTree.put(ARG_TREERANDOMIZE_LONG, useCustomTreeRandomize);
	outTree.put(ARG_TREEROUNDUP_LONG, treeRoundUpSize);
	outTree.put(ARG_VERIFYDIRECT_LONG, doDirectVerify);


	// dynamically calculated values for service hosts...

	size_t remoteRankOffset = getIsServicePathShared() ?
		rankOffset + (serviceRank * numThreads) : rankOffset;

	outTree.put(ARG_RANKOFFSET_LONG, remoteRankOffset);

	outTree.put(ARG_TREEFILE_LONG, treeFilePath.empty() ? "" : SERVICE_UPLOAD_TREEFILE);

	if(!assignGPUPerService || gpuIDsVec.empty() )
		outTree.put(ARG_GPUIDS_LONG, gpuIDsStr);
	else
	{ // assign one GPU per service instance
		size_t gpuIndex = serviceRank % gpuIDsVec.size();
		outTree.put(ARG_GPUIDS_LONG, std::to_string(gpuIDsVec[gpuIndex] ) );
	}

}

/**
 * Get configuration as vector of strings, so that it can be used e.g. for CSV output.
 */
void ProgArgs::getAsStringVec(StringVec& outLabelsVec, StringVec& outValuesVec) const
{
	outLabelsVec.push_back("label");
	outValuesVec.push_back(benchLabelNoCommas);

	outLabelsVec.push_back("path type");
	outValuesVec.push_back(TranslatorTk::benchPathTypeToStr(benchPathType, this) );

	outLabelsVec.push_back("paths");
	outValuesVec.push_back(std::to_string(benchPathsVec.size() ) );

	outLabelsVec.push_back("hosts");
	outValuesVec.push_back(std::to_string(hostsVec.empty() ? 1 : hostsVec.size() ) );

	outLabelsVec.push_back("threads");
	outValuesVec.push_back(std::to_string(numThreads) );

	outLabelsVec.push_back("dirs");
	outValuesVec.push_back( (benchPathType != BenchPathType_DIR) ? "" : std::to_string(numDirs) );

	outLabelsVec.push_back("files");
	outValuesVec.push_back( (benchPathType != BenchPathType_DIR) ? "" : std::to_string(numFiles) );

	outLabelsVec.push_back("file size");
	outValuesVec.push_back(std::to_string(fileSize) );

	outLabelsVec.push_back("block size");
	outValuesVec.push_back(std::to_string(blockSize) );

	outLabelsVec.push_back("direct IO");
	outValuesVec.push_back(std::to_string(useDirectIO) );

	outLabelsVec.push_back("random");
	outValuesVec.push_back(std::to_string(useRandomOffsets) );

	outLabelsVec.push_back("random aligned");
	outValuesVec.push_back(!useRandomOffsets ? "" : std::to_string(useRandomAligned) );

	outLabelsVec.push_back("IO depth");
	outValuesVec.push_back(std::to_string(ioDepth) );

	outLabelsVec.push_back("shared paths");
	outValuesVec.push_back(hostsVec.empty() ? "" : std::to_string(getIsServicePathShared() ) );

	outLabelsVec.push_back("truncate");
	outValuesVec.push_back( (benchPathType == BenchPathType_BLOCKDEV) ?
		"" : std::to_string(doTruncate) );
}

/**
 * Reset benchmark path, close associated file descriptors (incl. cuFile driver) and free/reset
 * any other resources that are associated with the previous benchmark phase. Intended to be used
 * in service mode to not waste/block resources while idle.
 */
void ProgArgs::resetBenchPath()
{
	// cleanup any old memory mappings

	for(char*& mmapPtr : mmapVec)
	{
		if(mmapPtr == MAP_FAILED)
			continue;

		int unmapRes = munmap(mmapPtr, fileSize);

		if(unmapRes == -1)
			ERRLOGGER(Log_NORMAL, "File memory unmap failed. "
				"SysErr: " << strerror(errno) << std::endl);

		mmapPtr = (char*)MAP_FAILED;
	}

	mmapVec.resize(0);

	// dereg prev registered handles. (CuFileHandleData can handle the case of entries not reg'ed.)
	for(CuFileHandleData& cuFileHandleData : cuFileHandleDataVec)
			cuFileHandleData.deregisterHandle();

	cuFileHandleDataVec.resize(0); // reset before reuse in service mode
	gpuIDsVec.resize(0); // reset before reuse in service mode
	gpuIDsStr = ""; // reset before reuse in service mode

	// close open file descriptors
	for(int fd : benchPathFDsVec)
		close(fd);

	benchPathFDsVec.resize(0); // reset before reuse in service mode

	benchPathsVec.resize(0); // reset before reuse in service mode
	benchPathStr = "";

	// reset custom tree mode path stores
	customTree.dirs.clear();
	customTree.filesNonShared.clear();
	customTree.filesShared.clear();

	s3EndpointsVec.clear();
	netBenchServersVec.clear();

#ifdef CUFILE_SUPPORT
	if(isCuFileDriverOpen)
		cuFileDriverClose();

	isCuFileDriverOpen = false;
#endif
}

/**
 * Get info (path type, auto-detected file size etc.) about benchmark paths. Used for reply to
 * master as result or preparation phase.
 *
 * @outTree path info for given paths.
 */
void ProgArgs::getBenchPathInfoTree(bpt::ptree& outTree)
{
	outTree.put(ARG_BENCHPATHS_LONG, benchPathStr);
	outTree.put(XFER_PREP_BENCHPATHTYPE, benchPathType);
	outTree.put(XFER_PREP_NUMBENCHPATHS, benchPathsVec.size() );
	outTree.put(ARG_FILESIZE_LONG, fileSize);
	outTree.put(ARG_BLOCK_LONG, blockSize);
	outTree.put(ARG_RANDOMAMOUNT_LONG, randomAmount);
}

/**
 * Check whether info from services is consistent among services and consistent with what master
 * sent for preparation, so intended to be called only in master mode after BenchPathInfo has been
 * received from services.
 *
 * @benchPathInfos BenchPathInfo of service hosts; element order must match getHostsVec().
 * @throw ProgException on error, e.g. inconsitency between different services.
 */
void ProgArgs::checkServiceBenchPathInfos(BenchPathInfoVec& benchPathInfos)
{
	// sanity check
	if(benchPathInfos.size() != hostsVec.size() )
		throw ProgException("Unexpected different number of elements for services and provided "
			"bench path infos");

	BenchPathInfo& firstInfo = benchPathInfos[0];

	// compare 1st info in provided list to our current info...

	if(firstInfo.numBenchPaths != benchPathsVec.size() )
		throw ProgException(
			"Service instance benchmark paths count does not match master paths count. "
			"Service: " + hostsVec[0] + "; "
			"Master paths: " + std::to_string(benchPathsVec.size() ) + " "
				"(" + benchPathStr + "); "
			"Service paths: " + std::to_string(firstInfo.numBenchPaths) + " "
				"(" + firstInfo.benchPathStr + ")");

	if(firstInfo.fileSize != fileSize)
		LOGGER(Log_NORMAL, "NOTE: Service instance adapted file size. "
			"New file size: " << firstInfo.fileSize << "; " <<
			"Service: " << hostsVec[0] << std::endl);

	if(firstInfo.blockSize != blockSize)
		LOGGER(Log_NORMAL, "NOTE: Service instance adapted block size. "
			"New block size: " << firstInfo.blockSize << "; " <<
			"Service: " << hostsVec[0] << std::endl);

	if(useRandomOffsets && (firstInfo.randomAmount != randomAmount) )
		LOGGER(Log_NORMAL, "NOTE: Service instance adapted random data amount. "
			"New random amount: " << firstInfo.randomAmount << "; " <<
			"Service: " << hostsVec[0] << std::endl);

	// apply settings provided by 1st info in list

	benchPathType = firstInfo.benchPathType;
	fileSize = firstInfo.fileSize;
	randomAmount = firstInfo.randomAmount;
	blockSize = firstInfo.blockSize;

	// compare all other bench path infos to 1st info in list
	for(size_t i=1; i < benchPathInfos.size(); i++)
	{
		BenchPathInfo& otherInfo = benchPathInfos[i];

		if(firstInfo.benchPathType != otherInfo.benchPathType)
			throw ProgException(
				"Conflicting benchmark path types on different service instances. "
				"Service_A: " + hostsVec[0] + "; "
				"Service_B: " + hostsVec[i] + "; "
				"Service_A paths: " + firstInfo.benchPathStr + "; "
				"Service_B paths: " + otherInfo.benchPathStr);

		if(firstInfo.numBenchPaths != otherInfo.numBenchPaths)
			throw ProgException(
				"Conflicting number of benchmark paths on different service instances. "
				"Service_A: " + hostsVec[0] + "; "
				"Service_B: " + hostsVec[i] + "; "
				"Service_A paths: " + std::to_string(firstInfo.numBenchPaths) + "; "
				"Service_B paths: " + std::to_string(otherInfo.numBenchPaths) );

		if(firstInfo.fileSize != otherInfo.fileSize)
			throw ProgException(
				"Conflicting file size on different service instances. "
				"Service_A: " + hostsVec[0] + "; "
				"Service_B: " + hostsVec[i] + "; "
				"Service_A file size: " + std::to_string(firstInfo.fileSize) + "; "
				"Service_B file size: " + std::to_string(otherInfo.fileSize) );

		if(firstInfo.blockSize != otherInfo.blockSize)
			throw ProgException(
				"Conflicting block sizes on different service instances. "
				"Service_A: " + hostsVec[0] + "; "
				"Service_B: " + hostsVec[i] + "; "
				"Service_A block size: " + std::to_string(firstInfo.blockSize) + "; "
				"Service_B block size: " + std::to_string(otherInfo.blockSize) );

		if(useRandomOffsets && (firstInfo.randomAmount != otherInfo.randomAmount) )
			throw ProgException(
				"Conflicting random amount on different service instances. "
				"Service_A: " + hostsVec[0] + "; "
				"Service_B: " + hostsVec[i] + "; "
				"Service_A random amount: " + std::to_string(firstInfo.randomAmount) + "; "
				"Service_B random amount: " + std::to_string(otherInfo.randomAmount) );
	}
}

/**
 * Check an existing non-empty CSV file for compatibility. This means we count the number of commas
 * in the header line to see if it matches the current number of commas. This way, we can avoid
 * adding to a CSV file that was written with a different version and thus uses a different number
 * of columns.
 *
 * @throw ProgException on incompatibility
 */
void ProgArgs::checkCSVFileCompatibility()
{
	if(csvFilePath.empty() )
		return; // nothing to do

	if(FileTk::checkFileEmpty(csvFilePath) )
		return; // no csv file yet or file is empty, so nothing to do

	std::string lineStr;

	std::ifstream fileStream(csvFilePath.c_str() );
	if(!fileStream)
		throw ProgException("Opening csv file for compatibility check failed: " + csvFilePath);

	// read first line
	std::getline(fileStream, lineStr);

	int numCommas = std::count(lineStr.begin(), lineStr.end(), ',');

	if(numCommas != CSVFILE_EXPECTED_COMMAS)
		throw ProgException("CSV output file exists. Trying to append, but column compatibility "
			"check failed. "
			"Was this file generated by a different " EXE_NAME " version? "
			"Found commas: " + std::to_string(numCommas) + "; "
			"Expected commas: " + std::to_string(CSVFILE_EXPECTED_COMMAS) + "; "
			"File: " + csvFilePath);
}

/**
 * Rotate hosts vector by user-defined number.
 *
 * This is a no-op if user did not specify rotation or if netbench mode is used.
 *
 * This is not thread-safe because it's intended to be called between test phases and with a
 * correponsing restart of workers.
 */
void ProgArgs::rotateHosts()
{
	if( (hostsVec.size() < 2) || useNetBench)
		return;

	for(unsigned i = 0; i < rotateHostsNum; i++)
		std::rotate(hostsVec.begin(), hostsVec.begin() + 1, hostsVec.end() );
}
