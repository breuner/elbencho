#include <boost/algorithm/string.hpp>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <libgen.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "ProgArgs.h"
#include "ProgException.h"
#include "Terminal.h"
#include "toolkits/NumaTk.h"
#include "toolkits/TranslatorTk.h"
#include "toolkits/UnitTk.h"

#ifdef CUFILE_SUPPORT
	#include <cufile.h>
#endif

#define MKFILE_MODE					(S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH)
#define DIRECTIO_MINSIZE			512 // min size in bytes for direct IO
#define BENCHPATH_DELIMITER			",\n\r@" // delimiters for user-defined bench dir paths
#define HOSTLIST_DELIMITERS			", \n\r" // delimiters for hosts string (comma or space)
#define HOST_PORT_SEPARATOR			":" // separator for hostname:port
#define ZONELIST_DELIMITERS			", " // delimiters for numa zones string (comma or space)
#define GPULIST_DELIMITERS			", \n\r" // delimiters for gpuIDs string (comma or space)

#define ENDL						<< std::endl << // just to make help text print lines shorter

#define FILESHAREBLOCKFACTOR		32 // in custom tree mode, blockSize factor as of which to share
#define FILESHAREBLOCKFACTOR_STR	STRINGIZE(FILESHAREBLOCKFACTOR)

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

		// Add the configfile option before all the other options to prevent a cycle

		argsDescription.add_options()
		(ARG_CONFIGFILE_LONG "," ARG_CONFIGFILE_SHORT, bpo::value(&this->configFilePath),
			"Path to benchmark configuration file. All command line options starting with "
			"double dashes can be used as \"OPTIONNAME=VALUE\" in the config file. Multiple "
			"options are newline-separated. Lines starting with \"#\" are ignored.");

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

	convertUnitStrings();
	checkArgs();
}

ProgArgs::~ProgArgs()
{
	for(int fd : benchPathFDsVec)
		close(fd);

	// note: dereg won't hurt if never reg'ed, because CuFileHandleData can handle that case.
	for(CuFileHandleData& cuFileHandleData : cuFileHandleDataVec)
			cuFileHandleData.deregisterHandle();

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
		(ARG_HELPMULTIFILE_LONG, "Print multi-file / multi-directory help message.")
		(ARG_HELPDISTRIBUTED_LONG, "Print distributed benchmark help message.")
		(ARG_HELPALLOPTIONS_LONG, "Print all available options help message.")
	;

    // alphabetic order to print help in alphabetical order
    argsGenericDescription.add_options()
/*al*/	(ARG_SHOWALLELAPSED_LONG, bpo::bool_switch(&this->showAllElapsed),
			"Show elapsed time to completion of each I/O worker thread.")
/*b*/	(ARG_BLOCK_LONG "," ARG_BLOCK_SHORT, bpo::value(&this->blockSizeOrigStr),
			"Number of bytes to read/write in a single operation. (Default: 1M)")
/*bl*/	(ARG_BLOCKVARIANCEALGO_LONG, bpo::value(&this->blockVarianceAlgo),
			"Random number algorithm for \"--" ARG_BLOCKVARIANCE_LONG "\". Values: \""
			RANDALGO_FAST_STR "\" for high speed but weaker randomness; \"" RANDALGO_BALANCED_STR
			"\" for good balance of speed and randomness; \"" RANDALGO_STRONG_STR "\" for high CPU "
			"cost but strong randomness. (Default: " RANDALGO_FAST_STR ")")
/*bl*/	(ARG_BLOCKVARIANCE_LONG, bpo::value(&this->blockVariancePercent),
			"Percentage of blocks that should be refilled with random data between writes. This "
			"can be used to control how well the generated data can be compressed or deduplicated. "
			"(Default: 0; Max: 100)")
/*cp*/	(ARG_CPUUTIL_LONG, bpo::bool_switch(&this->showCPUUtilization),
			"Show CPU utilization in phase stats results.")
/*cs*/	(ARG_CSVFILE_LONG, bpo::value(&this->csvFilePath),
			"Path to file for results in csv format. This way, result can be imported e.g. into "
			"MS Excel. If the file exists, results will be appended.")
#ifdef CUFILE_SUPPORT
/*cu*/	(ARG_CUFILE_LONG, bpo::bool_switch(&this->useCuFile),
			"Use cuFile API for reads/writes to/from GPU memory.")
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
/*dr*/	(ARG_DROPCACHESPHASE_LONG, bpo::bool_switch(&this->runDropCachesPhase),
			"Drop linux file system page cache, dentry cache and inode cache before/after each "
			"benchmark phase. Requires root privileges. This should be used together with \"--"
			ARG_SYNCPHASE_LONG "\", because only data on stable storage can be dropped from cache. "
			"Note for distributed file systems that this only drops caches on the clients where "
			"elbencho runs, but there might still be cached data on the server.")
/*F*/	(ARG_DELETEFILES_LONG "," ARG_DELETEFILES_SHORT,
			bpo::bool_switch(&this->runDeleteFilesPhase),
			"Delete files.")
/*fo*/	(ARG_FOREGROUNDSERVICE_LONG, bpo::bool_switch(&this->runServiceInForeground),
			"When running as service, stay in foreground and connected to console instead of "
			"detaching from console and daemonizing into backgorund.")
#ifdef CUFILE_SUPPORT
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
			"(Hint: CUDA GPU IDs are 0-based.)")
/*gp*/	(ARG_GPUPERSERVICE_LONG, bpo::bool_switch(&this->assignGPUPerService),
			"Assign GPUs round robin to service instances (i.e. one GPU per service) instead of "
			"default round robin to threads (i.e. multiple GPUs per service, if multiple given).")
#endif
/*ho*/	(ARG_HOSTS_LONG, bpo::value(&this->hostsStr),
			"Comma-separated list of hosts in service mode for coordinated benchmark. When this "
			"argument is used, this program instance runs in master mode to coordinate the given "
			"service mode hosts. The given number of threads, dirs and files is per-host then. "
			"(Format: hostname[:port])")
/*ho*/	(ARG_HOSTSFILE_LONG, bpo::value(&this->hostsFilePath),
			"Path to file containing line-separated service hosts to use for benchmark. (Format: "
			"hostname[:port])")
/*i*/	(ARG_ITERATIONS_LONG "," ARG_ITERATIONS_SHORT, bpo::value(&this->iterations),
			"Number of iterations to run the benchmark. (Default: 1)")
/*in*/	(ARG_INTERRUPT_LONG, bpo::bool_switch(&this->interruptServices),
			"Interrupt current benchmark phase on given service mode hosts.")
/*io*/	(ARG_IODEPTH_LONG, bpo::value(&this->ioDepth),
			"Depth of I/O queue per thread for asynchronous I/O. Setting this to 2 or higher "
			"turns on async I/O. (Default: 1)")
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
/*lo*/	(ARG_LOGLEVEL_LONG, bpo::value(&this->logLevel),
			"Log level. (Default: 0; Verbose: 1; Debug: 2)")
/*N*/	(ARG_NUMFILES_LONG "," ARG_NUMFILES_SHORT, bpo::value(&this->numFilesOrigStr),
			"Number of files per thread per directory. (Default: 1) Example: \""
			"-" ARG_NUMTHREADS_SHORT "2 -" ARG_NUMDIRS_SHORT "3 -" ARG_NUMFILES_SHORT "4\" will "
			"use 2x3x4=24 files.")
/*n*/	(ARG_NUMDIRS_LONG "," ARG_NUMDIRS_SHORT, bpo::value(&this->numDirsOrigStr),
			"Number of directories per thread. (Default: 1)")
/*no0*/	(ARG_IGNORE0USECERR_LONG, bpo::bool_switch(&this->ignore0USecErrors),
			"Do not warn if worker thread completion time is less than 1 microsecond.")
/*noc*/	(ARG_NOCSVLABELS_LONG, bpo::bool_switch(&this->noCSVLabels),
			"Do not print headline with labels to csv file.")
/*nod*/	(ARG_IGNOREDELERR_LONG, bpo::bool_switch(&this->ignoreDelErrors),
			"Ignore not existing files/dirs in deletion phase instead of treating this as error.")
/*nol*/	(ARG_NOLIVESTATS_LONG, bpo::bool_switch(&this->disableLiveStats),
			"Disable live statistics.")
/*nos*/	(ARG_NOSVCPATHSHARE_LONG, bpo::bool_switch(&this->noSharedServicePath),
			"Benchmark paths are not shared between service instances. Thus, each service instance "
			"will work on its own full dataset instead of a fraction of the data set.")
/*po*/	(ARG_SERVICEPORT_LONG, bpo::value(&this->servicePort),
			"TCP port of background service. (Default: " ARGDEFAULT_SERVICEPORT_STR ")")
/*qr*/	(ARG_PREALLOCFILE_LONG, bpo::bool_switch(&this->doPreallocFile),
			"Preallocate file disk space on creation via posix_fallocate().")
/*qu*/	(ARG_QUIT_LONG, bpo::bool_switch(&this->quitServices),
			"Quit services on given service mode hosts.")
/*r*/	(ARG_READ_LONG "," ARG_READ_SHORT, bpo::bool_switch(&this->runReadPhase),
			"Read files.")
/*ra*/	(ARG_RANDOMOFFSETS_LONG, bpo::bool_switch(&this->useRandomOffsets),
			"Read/write at random offsets.")
/*ra*/	(ARG_RANDSEEKALGO_LONG, bpo::value(&this->randOffsetAlgo),
			"Random number algorithm for \"--" ARG_RANDOMOFFSETS_LONG "\". Values: \""
			RANDALGO_FAST_STR "\" for high speed but weaker randomness; \"" RANDALGO_BALANCED_STR
			"\" for good balance of speed and randomness; \"" RANDALGO_STRONG_STR "\" for high CPU "
			"cost but strong randomness. (Default: " RANDALGO_BALANCED_STR ")")
/*ra*/	(ARG_RANDOMALIGN_LONG, bpo::bool_switch(&this->useRandomAligned),
			"Align random offsets to block size.")
/*ra*/	(ARG_RANDOMAMOUNT_LONG, bpo::value(&this->randomAmountOrigStr),
			"Number of bytes to write/read when using random offsets. (Default: Set to file size)")
/*ra*/	(ARG_RANKOFFSET_LONG, bpo::value(&this->rankOffset),
			"Rank offset for worker threads. (Default: 0)")
/*re*/	(ARG_LIVESLEEPSEC_LONG, bpo::value(&this->liveStatsSleepSec),
			"Sleep interval between live stats console refresh in seconds. (Default: 2)")
/*re*/	(ARG_RESULTSFILE_LONG, bpo::value(&this->resFilePath),
			"Path to file for human-readable results, similar to console output. If the file "
			"exists, new results will be appended.")
/*rw*/	(ARG_RWMIXPERCENT_LONG, bpo::value(&this->rwMixPercent),
			"Percentage of blocks that should be read in a write phase. (Default: 0; Max: 100)")
/*s*/	(ARG_FILESIZE_LONG "," ARG_FILESIZE_SHORT, bpo::value(&this->fileSizeOrigStr),
			"File size. (Default: 0)")
/*se*/	(ARG_RUNASSERVICE_LONG, bpo::bool_switch(&this->runAsService),
			"Run as service for distributed mode, waiting for requests from master.")
/*sh*/	(ARG_FILESHARESIZE_LONG, bpo::value(&this->fileShareSizeOrigStr),
			"In custom tree mode, this defines the file size as of which files are no longer "
			"exlusively assigned to a thread. This means multiple threads read/write different "
			"parts of files that exceed the given size. "
			"(Default: 0, which means " FILESHAREBLOCKFACTOR_STR " x blocksize)")
/*st*/	(ARG_STATFILES_LONG, bpo::bool_switch(&this->runStatFilesPhase),
			"Run file stat benchmark phase.")
/*st*/	(ARG_STARTTIME_LONG, bpo::value(&this->startTime),
			"Start time of first benchmark in UTC seconds since the epoch. Intended to synchronize "
			"start of benchmarks on different hosts, assuming they use synchronized clocks. "
			"(Hint: Try 'date +%s' to get seconds since the epoch.)")
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
			"worker threads, but each thread create/delete all of the dirs, so don't use this for "
			"dir create/delete performance testing.)")
/*tr*/	(ARG_TREERANDOMIZE_LONG, bpo::bool_switch(&this->useCustomTreeRandomize),
			"In custom tree mode: Randomize file order. Default is order by file size.")
/*tr*/	(ARG_TREEROUNDUP_LONG, bpo::value(&this->treeRoundUpSizeOrigStr),
			"When loading a treefile, round up all contained file sizes to a multiple of the given "
			"size. This is useful for \"" ARG_DIRECTIO_LONG "\" with its alignment requirements on "
			"many platforms. (Default: 0 for disabled)")
/*tr*/	(ARG_TRUNCATE_LONG, bpo::bool_switch(&this->doTruncate),
			"Truncate files to 0 size when opening for writing.")
/*tr*/	(ARG_TRUNCTOSIZE_LONG, bpo::bool_switch(&this->doTruncToSize),
			"Truncate files to given \"--" ARG_FILESIZE_LONG "\" via ftruncate() when opening for "
			"writing. If the file previously was larger then the remainder is discarded. This flag "
			"is automatically enabled when \"--" ARG_RANDOMOFFSETS_LONG "\" is given.")
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
/*zo*/	(ARG_NUMAZONES_LONG, bpo::value(&this->numaZonesStr),
			"Comma-separated list of NUMA zones to bind this process to. If multiple zones are "
			"given, then worker threads are bound round-robin to the zones. "
			"(Hint: See 'lscpu' for available NUMA zones.)")
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
	this->liveStatsSleepSec = 2;
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
	this->blockVariancePercent = 0;
	this->rwMixPercent = 0;
	this->blockVarianceAlgo = RANDALGO_FAST_STR;
	this->randOffsetAlgo = RANDALGO_BALANCED_STR;
	this->fileShareSize = 0;
	this->fileShareSizeOrigStr = "0";
	this->useCustomTreeRandomize = false;
	this->treeRoundUpSize = 0;
	this->treeRoundUpSizeOrigStr = "0";
}

/**
 * Convert human strings with units (e.g. "4K") to actual numbers.
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

	if(rwMixPercent && !gpuIDsVec.empty() && !useCuFile)
		throw ProgException("Option --" ARG_RWMIXPERCENT_LONG " cannot be used together with "
			"GPU memory copy");

	if(integrityCheckSalt && rwMixPercent)
		throw ProgException("Option --" ARG_RWMIXPERCENT_LONG " cannot be used together with "
			"option --" ARG_INTEGRITYCHECK_LONG);

	if(integrityCheckSalt && blockVariancePercent)
		throw ProgException("Option --" ARG_BLOCKVARIANCE_LONG " cannot be used together with "
			"option --" ARG_INTEGRITYCHECK_LONG);

	if(integrityCheckSalt && runCreateFilesPhase && useRandomOffsets)
		throw ProgException("Integrity check writes are not supported in combination with random "
			"offsets.");

	if(doDirectVerify && (!integrityCheckSalt || !runCreateFilesPhase) )
		throw ProgException("Direct verification requires --" ARG_INTEGRITYCHECK_LONG " and "
			"--" ARG_CREATEFILES_LONG);

	if(doDirectVerify && (ioDepth > 1) )
		throw ProgException("Direct verification cannot be used together with --" ARG_IODEPTH_LONG);

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
	if( (benchPathType != BenchPathType_DIR) && runStatFilesPhase)
		throw ProgException("File stat phase can only be used when benchmark path is a directory.");

	if( (benchPathType == BenchPathType_DIR) && useRandomOffsets)
		throw ProgException("Random offsets are not allowed when benchmark path is a directory.");

	// ensure bench path is dir when tree file is given
	if( (benchPathType != BenchPathType_DIR) && !treeFilePath.empty() )
		throw ProgException("Custom tree mode requires benchmark path to be a directory.");

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
	if(useRandomOffsets && !randomAmount)
		randomAmount = (benchPathType == BenchPathType_DIR) ?
			fileSize : (fileSize * benchPathsVec.size() );

	if(useDirectIO && fileSize && (runCreateFilesPhase || runReadPhase) )
	{
		if(useRandomOffsets && !useRandomAligned)
		{
			LOGGER(Log_VERBOSE, "NOTE: Direct IO requires alignment. "
				"Enabling \"--" ARG_RANDOMALIGN_LONG "\"." << std::endl);

			useRandomAligned = true;
		}

		if( (blockSize % DIRECTIO_MINSIZE) != 0)
			throw ProgException("Block size for direct IO is not a multiple of required size. "
				"(Note that a system's actual required size for direct IO might be even higher "
				"depending on system page size and drive sector size.) "
				"Required size: " + std::to_string(DIRECTIO_MINSIZE) );
	}

	if(useRandomOffsets && useRandomAligned && (randomAmount % blockSize) )
	{
		size_t newRandomAmount = randomAmount - (randomAmount % blockSize);

		LOGGER(Log_NORMAL, "NOTE: Random amount for aligned IO is not a multiple of block size. "
			"Reducing random amount. " <<
			"Old: " << randomAmount << "; " <<
			"New: " << newRandomAmount << std::endl);

		randomAmount = newRandomAmount;
	}

	// shared file or block device mode
	if(benchPathType != BenchPathType_DIR)
	{
		// note: we ensure before this point that fileSize, blockSize and thread count are not 0.

		const size_t numFiles = benchPathFDsVec.size();
		const uint64_t numBlocksPerFile = (fileSize / blockSize) +
			( (fileSize % blockSize) ? 1 : 0);
		const uint64_t numBlocksTotal = numBlocksPerFile * numFiles; // total for all files
		const uint64_t blockSetSize = blockSize * numDataSetThreads;

		if(useRandomOffsets && (randomAmount < blockSetSize) )
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

	}

	// recheck randomAmount after auto-decrease above
	if(useRandomOffsets && !randomAmount && fileSize && (runCreateFilesPhase || runReadPhase) )
		throw ProgException("File size must not be smaller than block size when random I/O "
			"with alignment is selected.");

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
				benchPathStr += absolutePath(path) + std::string(BENCHPATH_DELIMITER)[0];
		}
		else // no override given, use benchPathStr from master
			boost::split(benchPathsVec, benchPathStr, boost::is_any_of(BENCHPATH_DELIMITER),
				boost::token_compress_on);

		// delete empty string elements from pathsVec (they come from delimiter use at start/end)
		for( ; ; )
		{
			StringVec::iterator iter = std::find(benchPathsVec.begin(), benchPathsVec.end(), "");
			if(iter == benchPathsVec.end() )
				break;

			benchPathsVec.erase(iter);
		}
	}
	else
	{ // master or local: take paths from command line as vector
		if(argsVariablesMap.count(ARG_BENCHPATHS_LONG) )
			benchPathsVec = argsVariablesMap[ARG_BENCHPATHS_LONG].as<StringVec>();

		// update benchPathStr to contain absolute paths (for distributed run & for verbose print)
		benchPathStr = "";
		for(std::string path : benchPathsVec)
			benchPathStr += absolutePath(path) + std::string(BENCHPATH_DELIMITER)[0];
	}

	if(benchPathsVec.empty() || benchPathStr.empty() )
		throw ProgException("Benchmark path missing.");

	// skip open of local paths if this is the master of a distributed run
	if(!hostsStr.empty() )
		return;

	// if we get here then this is not the master of a distributed run...

	prepareBenchPathFDsVec();
	prepareCuFileHandleDataVec();
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

		if( (benchPathType == BenchPathType_DIR) && !numDirs)
			throw ProgException("Number of directories may not be zero");

		int fd;
		int openFlags = 0;

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

		// note: cleanup won't be a problem if reg no done, as CuFileHandleData can handle that case

		if(!useCuFile)
			continue; // no registration to be done if cuFile API is not used

		cuFileHandleData.registerHandle<ProgException>(fd);
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

		if(!runCreateFilesPhase && ( (uint64_t)currentFileSize < fileSize) )
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
			// (note: in random mode for reads to work across full length)
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
			// let user know about reading sparse/compressed files
			// (note: statBuf.st_blocks is in 512-byte units)
			if( (statBuf.st_blocks * 512) < currentFileSize)
				LOGGER(Log_NORMAL,
					"NOTE: Allocated file disk space smaller than file size. File seems sparse or "
					"compressed. (Sequential write can fill sparse areas.) "
					"Path: " << path << "; "
					"File size: " << currentFileSize << "; "
					"Allocated size: " << (statBuf.st_blocks * 512) << std::endl);
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
 * Parse hosts string to fill hostsVec. Do nothing if hosts string is empty.
 *
 * hostsVec elements will have default port appended if no port was defined.
 *
 * @throw ProgException if a problem is found, e.g. hosts string was not empty, but parsed result
 * 		is empty.
 */
void ProgArgs::parseHosts()
{
	if(hostsStr.empty() && hostsFilePath.empty() )
		return; // nothing to do

	// read service hosts from file and add to hostsStr
	if(!hostsFilePath.empty() )
	{
		std::ifstream hostsFile(hostsFilePath);

		if(!hostsFile)
			throw ProgException("Unable to read hosts file. Path: " + hostsFilePath);

		hostsStr += " "; // add separator to existing hosts

		std::string lineStr;

		while(std::getline(hostsFile, lineStr) )
			hostsStr += lineStr + ",";

		hostsFile.close();
	}

	boost::split(hostsVec, hostsStr, boost::is_any_of(HOSTLIST_DELIMITERS),
			boost::token_compress_on);

	// delete empty string elements from vec (they come from delimiter use at beginning or end)
	for( ; ; )
	{
		StringVec::iterator iter = std::find(hostsVec.begin(), hostsVec.end(), "");
		if(iter == hostsVec.end() )
			break;

		hostsVec.erase(iter);
	}

	for(std::string& host : hostsVec)
	{
		std::size_t findRes = host.find(HOST_PORT_SEPARATOR);

		// add default port to hosts where port is not provided by user
		if(findRes == std::string::npos)
			host += HOST_PORT_SEPARATOR + std::to_string(servicePort);
	}

	if(hostsVec.empty() )
		throw ProgException("Hosts defined, but parsing resulted in an empty list: " + hostsStr);

	// check for duplicates
	std::set<std::string> hostsSet(hostsVec.begin(), hostsVec.end() );
	if(hostsSet.size() != hostsVec.size() )
		throw ProgException("List of hosts contains duplicates. "
			"Number of duplicates: " + std::to_string(hostsVec.size() - hostsSet.size() ) + "; "
			"List: " + hostsStr);
}

/**
 * Parse numa zones string to fill numaZonesVec. Do nothing if numa zones string is empty.
 * Also applies the given zones to the current thread.
 *
 * Note: We use libnuma in NumaTk, but we don't allow libnuma's NUMA string format to build
 * our vector that easily allows us to easily bind I/O workers round-robin to given zones.
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

	StringVec zonesStrVec; // temporary for split()

	boost::split(zonesStrVec, numaZonesStr, boost::is_any_of(ZONELIST_DELIMITERS),
			boost::token_compress_on);

	// delete empty string elements from vec (they come from delimiter use at beginning or end)
	for( ; ; )
	{
		StringVec::iterator iter = std::find(zonesStrVec.begin(), zonesStrVec.end(), "");
		if(iter == zonesStrVec.end() )
			break;

		zonesStrVec.erase(iter);
	}

	if(zonesStrVec.empty() )
		throw ProgException("NUMA zones defined, but parsing resulted in an empty list: " +
			numaZonesStr);

	// rebuild numaZonesStr clean from trimmed vec, because libnuma has probs with leading delimter
	numaZonesStr = "";
	for(unsigned i=0; i < zonesStrVec.size(); i++)
		numaZonesStr += (i ? "," : "") + zonesStrVec[i];

	// apply given zones to current thread
	NumaTk::bindToNumaZones(numaZonesStr);

	// convert from string vector to int vector
	for(std::string& zoneStr : zonesStrVec)
		numaZonesVec.push_back(std::stoi(zoneStr) );
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
	#endif

	StringVec gpuIDsStrVec; // temporary for split()

	boost::split(gpuIDsStrVec, gpuIDsStr, boost::is_any_of(GPULIST_DELIMITERS),
		boost::token_compress_on);

	// delete empty string elements from vec (they come from delimiter use at beginning or end)
	for( ; ; )
	{
		StringVec::iterator iter = std::find(gpuIDsStrVec.begin(), gpuIDsStrVec.end(), "");
		if(iter == gpuIDsStrVec.end() )
			break;

		gpuIDsStrVec.erase(iter);
	}

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
	#endif
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

	if(runCreateDirsPhase || runDeleteDirsPhase)
	{
		customTree.dirs.loadDirsFromFile(treeFilePath);
		customTree.dirs.sortByPathLen();
	}

	if(runCreateFilesPhase || runStatFilesPhase || runReadPhase || runDeleteFilesPhase)
	{
		// (note: fileShareSize is guaranteed to not be 0, thus the "fileShareSize-1" is ok)
		customTree.filesNonShared.setBlockSize(blockSize);
		customTree.filesNonShared.loadFilesFromFile(
			treeFilePath, 0, fileShareSize-1, treeRoundUpSize);
		customTree.filesNonShared.sortByFileSize();

		customTree.filesShared.setBlockSize(blockSize);
		customTree.filesShared.loadFilesFromFile(
			treeFilePath, fileShareSize, ~0ULL, treeRoundUpSize);
		customTree.filesNonShared.sortByFileSize();
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
		argsVariablesMap.count(ARG_HELPMULTIFILE_LONG) ||
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
	if(argsVariablesMap.count(ARG_HELPBLOCKDEV_LONG) )
		printHelpBlockDev();
	else
	if(argsVariablesMap.count(ARG_HELPMULTIFILE_LONG) )
		printHelpMultiFile();
	else
	if(argsVariablesMap.count(ARG_HELPDISTRIBUTED_LONG) )
		printHelpDistributed();
	else
	if(argsVariablesMap.count(ARG_HELPALLOPTIONS_LONG) )
		printHelpAllOptions();
}

void ProgArgs::printHelpOverview()
{
	std::cout <<
		EXE_NAME " - A distributed benchmark for file systems and block devices" ENDL
		std::endl <<
		"Version: " EXE_VERSION ENDL
		std::endl <<
		"Tests include throughput, IOPS and access latency. Live statistics show how the" ENDL
		"system behaves under load and whether it is worth waiting for the end result." ENDL
		std::endl <<
		"Get started by selecting what you want to test..." ENDL
		std::endl <<
		"Block devices or large shared files:" ENDL
		"  $ " EXE_NAME " --" ARG_HELPBLOCKDEV_LONG ENDL
		std::endl <<
		"Many files in different directories:" ENDL
		"  $ " EXE_NAME " --" ARG_HELPMULTIFILE_LONG ENDL
		std::endl <<
		"Multiple clients:" ENDL
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
		"  Test 4KiB block random read latency of device /dev/nvme0n1:" ENDL
		"    $ " EXE_NAME " -r -b 4K --lat --direct --rand /dev/nvme0n1" ENDL
		std::endl <<
		"  Test 4KiB multi-threaded write IOPS of devices /dev/nvme0n1 & /dev/nvme1n1:" ENDL
		"    $ " EXE_NAME " -w -b 4K -t 16 --iodepth 16 --direct --rand \\" ENDL
		"        /dev/nvme0n1 /dev/nvme1n1" ENDL
		std::endl <<
		"  Test 1MiB multi-threaded sequential read throughput of device /dev/nvme0n1:" ENDL
		"    $ " EXE_NAME " -r -b 1M -t 8 --iodepth 4 --direct /dev/nvme0n1" ENDL
		std::endl <<
		"  Create large file and test random read IOPS for max 20 seconds:" ENDL
		"    $ " EXE_NAME " -w -b 4M --direct --size 20g /mnt/myfs/file1" ENDL
		"    $ " EXE_NAME " -r -b 4k -t 16 --iodepth 16 --direct --rand --timelimit 20 \\" ENDL
		"        /mnt/myfs/file1" ENDL
#ifdef CUDA_SUPPORT
		std::endl <<
		"  Stream data from large file into memory of first 2 GPUs via CUDA:" ENDL
		"    $ " EXE_NAME " -r -b 1M -t 8 --gpuids 0,1 --cuhostbufreg \\" ENDL
		"        /mnt/myfs/file1" ENDL
#endif
#ifdef CUFILE_SUPPORT
		std::endl <<
		"  Stream data from large file into memory of first 2 GPUs via GPUDirect Storage:" ENDL
		"    $ " EXE_NAME " -r -b 1M -t 8 --gpuids 0,1 --cufile --gdsbufreg --direct \\" ENDL
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
		(ARG_CREATEFILES_LONG "," ARG_CREATEFILES_SHORT, bpo::bool_switch(&this->runCreateFilesPhase),
			"Write files. Create them if they don't exist.")
		(ARG_READ_LONG "," ARG_READ_SHORT, bpo::bool_switch(&this->runReadPhase),
			"Read files.")
		(ARG_STATFILES_LONG, bpo::bool_switch(&this->runStatFilesPhase),
			"Read file status attributes (file size, owner etc).")
		(ARG_DELETEFILES_LONG "," ARG_DELETEFILES_SHORT, bpo::bool_switch(&this->runDeleteFilesPhase),
			"Delete files.")
		(ARG_DELETEDIRS_LONG "," ARG_DELETEDIRS_SHORT, bpo::bool_switch(&this->runDeleteDirsPhase),
			"Delete directories.")
		(ARG_NUMTHREADS_LONG "," ARG_NUMTHREADS_SHORT, bpo::value(&this->numThreads),
			"Number of I/O worker threads. (Default: 1)")
		(ARG_NUMDIRS_LONG "," ARG_NUMDIRS_SHORT, bpo::value(&this->numDirs),
			"Number of directories per I/O worker thread. (Default: 1)")
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
		"    $ " EXE_NAME " -t 2 -d -n 3 -w -N 4 -s 1m -b 1m /data/testdir" ENDL
		std::endl <<
		"  Test 2 threads, each reading 4 1MB files from 3 directories in 128KiB blocks:" ENDL
		"    $ " EXE_NAME " -t 2 -n 3 -r -N 4 -s 1m -b 128k /data/testdir" ENDL
#ifdef CUDA_SUPPORT
		std::endl <<
		"  As above, but also copy data into memory of first 2 GPUs via CUDA:" ENDL
		"    $ " EXE_NAME " -t 2 -n 3 -r -N 4 -s 1m -b 128k \\" ENDL
		"        --gpuids 0,1 --cuhostbufreg /data/testdir" ENDL
#endif
#ifdef CUFILE_SUPPORT
		std::endl <<
		"  As above, but read data into memory of first 2 GPUs via GPUDirect Storage:" ENDL
		"    $ " EXE_NAME " -t 2 -n 3 -r -N 4 -s 1m -b 128k \\" ENDL
		"        --gpuids 0,1 --cufile --gdsbufreg --direct /data/testdir" ENDL
#endif
		std::endl <<
		"  Delete files and directories created by example above:" ENDL
		"    $ " EXE_NAME " -t 2 -n 3 -N 4 -F -D /data/testdir" <<
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
		(ARG_INTERRUPT_LONG, bpo::bool_switch(&this->interruptServices),
			"Interrupt current benchmark phase on given service mode hosts.")
		(ARG_FOREGROUNDSERVICE_LONG, bpo::bool_switch(&this->runServiceInForeground),
			"When running as service, stay in foreground and connected to console instead of "
			"detaching from console and daemonizing into backgorund.")
	;

    std::cout << argsDistributedMiscDescription << std::endl;

    std::cout <<
    	"Examples:" ENDL
		"  Run service on two different NUMA zones of host node001:" ENDL
		"    $ " EXE_NAME " --service --zone 0 --port 1611" ENDL
		"    $ " EXE_NAME " --service --zone 1 --port 1612" ENDL
		std::endl <<
		"  Run master to coordinate benchmarks on node001 services, using 4 threads per" ENDL
		"  service and creating 8 dirs per thread, each containing 16 1MiB files:" ENDL
		"    $ " EXE_NAME " --hosts node001:1611,node001:1612 \\" ENDL
		"        -t 4 -d -n 8 -w -N 16 -s 1M /data/testdir" ENDL
		std::endl <<
		"  Quit services on host node001:" ENDL
		"    $ " EXE_NAME " --hosts node001:1611,node001:1612 --quit" <<
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
 * Print version and included optional build features.
 */
void ProgArgs::printVersionAndBuildInfo()
{
	std::ostringstream includedStream; // included optional build features
	std::ostringstream notIncludedStream; // not included optional build features

	std::cout << EXE_NAME << std::endl;
	std::cout << "Version: " EXE_VERSION << std::endl;
	std::cout << "Net protocol version: " HTTP_PROTOCOLVERSION << std::endl;
	std::cout << "Build date: " __DATE__ << " " << __TIME__ << std::endl;

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

#if NO_BACKTRACE == 1 // no backtraces for musl-libc compatibility
	notIncludedStream << "backtrace ";
#else
	includedStream << "backtrace ";
#endif

	std::cout << "Included optional build features: " <<
		(includedStream.str().empty() ? "-" : includedStream.str() ) << std::endl;
	std::cout << "Excluded optional build features: " <<
		(notIncludedStream.str().empty() ? "-" : notIncludedStream.str() ) << std::endl;
}

/**
 * Sets the arguments that are relevant for a new benchmark in service mode.
 *
 * @throw ProgException if configuration error is detected or if bench dirs cannot be accessed.
 */
void ProgArgs::setFromPropertyTree(bpt::ptree& tree)
{
	benchPathStr = tree.get<std::string>(ARG_BENCHPATHS_LONG);
	numThreads = tree.get<size_t>(ARG_NUMTHREADS_LONG);
	numDirs = tree.get<size_t>(ARG_NUMDIRS_LONG);
	numFiles = tree.get<size_t>(ARG_NUMFILES_LONG);
	fileSize = tree.get<uint64_t>(ARG_FILESIZE_LONG);
	blockSize = tree.get<size_t>(ARG_BLOCK_LONG);
	useDirectIO = tree.get<bool>(ARG_DIRECTIO_LONG);
	ignoreDelErrors = tree.get<bool>(ARG_IGNOREDELERR_LONG);
	ignore0USecErrors = tree.get<bool>(ARG_IGNORE0USECERR_LONG);
	runCreateDirsPhase = tree.get<bool>(ARG_CREATEDIRS_LONG);
	runCreateFilesPhase = tree.get<bool>(ARG_CREATEFILES_LONG);
	runReadPhase = tree.get<bool>(ARG_READ_LONG);
	runDeleteFilesPhase = tree.get<bool>(ARG_DELETEFILES_LONG);
	runDeleteDirsPhase = tree.get<bool>(ARG_DELETEDIRS_LONG);
	useRandomOffsets = tree.get<bool>(ARG_RANDOMOFFSETS_LONG);
	useRandomAligned = tree.get<bool>(ARG_RANDOMALIGN_LONG);
	randomAmount = tree.get<size_t>(ARG_RANDOMAMOUNT_LONG);
	ioDepth = tree.get<size_t>(ARG_IODEPTH_LONG);
	doTruncate = tree.get<bool>(ARG_TRUNCATE_LONG);
	gpuIDsStr = tree.get<std::string>(ARG_GPUIDS_LONG);
	useCuFile = tree.get<bool>(ARG_CUFILE_LONG);
	useGDSBufReg = tree.get<bool>(ARG_GDSBUFREG_LONG);
	useCuFileDriverOpen = tree.get<bool>(ARG_CUFILEDRIVEROPEN_LONG);
	useCuHostBufReg = tree.get<bool>(ARG_CUHOSTBUFREG_LONG);
	integrityCheckSalt = tree.get<uint64_t>(ARG_INTEGRITYCHECK_LONG);
	runSyncPhase = tree.get<bool>(ARG_SYNCPHASE_LONG);
	runDropCachesPhase = tree.get<bool>(ARG_DROPCACHESPHASE_LONG);
	runStatFilesPhase = tree.get<bool>(ARG_STATFILES_LONG);
	doTruncToSize = tree.get<bool>(ARG_TRUNCTOSIZE_LONG);
	doPreallocFile = tree.get<bool>(ARG_PREALLOCFILE_LONG);
	doDirSharing = tree.get<bool>(ARG_DIRSHARING_LONG);
	doDirectVerify = tree.get<bool>(ARG_VERIFYDIRECT_LONG);
	blockVariancePercent = tree.get<unsigned>(ARG_BLOCKVARIANCE_LONG);
	rwMixPercent = tree.get<unsigned>(ARG_RWMIXPERCENT_LONG);
	blockVarianceAlgo = tree.get<std::string>(ARG_BLOCKVARIANCEALGO_LONG);
	randOffsetAlgo = tree.get<std::string>(ARG_RANDSEEKALGO_LONG);
	fileShareSize = tree.get<uint64_t>(ARG_FILESHARESIZE_LONG);
	useCustomTreeRandomize = tree.get<bool>(ARG_TREERANDOMIZE_LONG);
	treeRoundUpSize = tree.get<uint64_t>(ARG_TREEROUNDUP_LONG);

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

	// rebuild benchPathsVec/benchPathFDsVec and check if bench dirs are accessible
	parseAndCheckPaths();

	checkPathDependentArgs();

	// apply GPU IDs override if given
	if(!gpuIDsStr.empty() && !gpuIDsServiceOverride.empty() )
		gpuIDsStr = gpuIDsServiceOverride;

	parseGPUIDs();
}

/**
 * Gets the arguments that are relevant for a new benchmark in service mode.
 *
 * @workerRank to calc rankOffset for host (unless user requested all to work on same dataset)
 */
void ProgArgs::getAsPropertyTree(bpt::ptree& outTree, size_t workerRank) const
{
	outTree.put(ARG_BENCHPATHS_LONG, benchPathStr);
	outTree.put(ARG_NUMTHREADS_LONG, numThreads);
	outTree.put(ARG_NUMDIRS_LONG, numDirs);
	outTree.put(ARG_NUMFILES_LONG, numFiles);
	outTree.put(ARG_FILESIZE_LONG, fileSize);
	outTree.put(ARG_BLOCK_LONG, blockSize);
	outTree.put(ARG_DIRECTIO_LONG, useDirectIO);
	outTree.put(ARG_IGNOREDELERR_LONG, ignoreDelErrors);
	outTree.put(ARG_IGNORE0USECERR_LONG, ignore0USecErrors);
	outTree.put(ARG_CREATEDIRS_LONG, runCreateDirsPhase);
	outTree.put(ARG_CREATEFILES_LONG, runCreateFilesPhase);
	outTree.put(ARG_READ_LONG, runReadPhase);
	outTree.put(ARG_DELETEFILES_LONG, runDeleteFilesPhase);
	outTree.put(ARG_DELETEDIRS_LONG, runDeleteDirsPhase);
	outTree.put(ARG_RANDOMOFFSETS_LONG, useRandomOffsets);
	outTree.put(ARG_RANDOMALIGN_LONG, useRandomAligned);
	outTree.put(ARG_RANDOMAMOUNT_LONG, randomAmount);
	outTree.put(ARG_NUMDATASETTHREADS_LONG, numDataSetThreads);
	outTree.put(ARG_IODEPTH_LONG, ioDepth);
	outTree.put(ARG_TRUNCATE_LONG, doTruncate);
	outTree.put(ARG_CUFILE_LONG, useCuFile);
	outTree.put(ARG_GDSBUFREG_LONG, useGDSBufReg);
	outTree.put(ARG_CUFILEDRIVEROPEN_LONG, useCuFileDriverOpen);
	outTree.put(ARG_CUHOSTBUFREG_LONG, useCuHostBufReg);
	outTree.put(ARG_INTEGRITYCHECK_LONG, integrityCheckSalt);
	outTree.put(ARG_SYNCPHASE_LONG, runSyncPhase);
	outTree.put(ARG_DROPCACHESPHASE_LONG, runDropCachesPhase);
	outTree.put(ARG_STATFILES_LONG, runStatFilesPhase);
	outTree.put(ARG_TRUNCTOSIZE_LONG, doTruncToSize);
	outTree.put(ARG_PREALLOCFILE_LONG, doPreallocFile);
	outTree.put(ARG_DIRSHARING_LONG, doDirSharing);
	outTree.put(ARG_VERIFYDIRECT_LONG, doDirectVerify);
	outTree.put(ARG_BLOCKVARIANCE_LONG, blockVariancePercent);
	outTree.put(ARG_RWMIXPERCENT_LONG, rwMixPercent);
	outTree.put(ARG_BLOCKVARIANCEALGO_LONG, blockVarianceAlgo);
	outTree.put(ARG_RANDSEEKALGO_LONG, randOffsetAlgo);
	outTree.put(ARG_FILESHARESIZE_LONG, fileShareSize);
	outTree.put(ARG_TREERANDOMIZE_LONG, useCustomTreeRandomize);
	outTree.put(ARG_TREEROUNDUP_LONG, treeRoundUpSize);

	// dynamically calculated values for service hosts...

	size_t remoteRankOffset = getIsServicePathShared() ?
		rankOffset + (workerRank * numThreads) : rankOffset;

	outTree.put(ARG_RANKOFFSET_LONG, remoteRankOffset);

	outTree.put(ARG_TREEFILE_LONG, treeFilePath.empty() ? "" : SERVICE_UPLOAD_TREEFILE);

	if(!assignGPUPerService || gpuIDsVec.empty() )
		outTree.put(ARG_GPUIDS_LONG, gpuIDsStr);
	else
	{ // assign one GPU per service instance
		size_t gpuIndex = workerRank % gpuIDsVec.size();
		outTree.put(ARG_GPUIDS_LONG, std::to_string(gpuIDsVec[gpuIndex] ) );
	}

}

/**
 * Get configuration as vector of strings, so that it can be used e.g. for CSV output.
 */
void ProgArgs::getAsStringVec(StringVec& outLabelsVec, StringVec& outValuesVec) const
{
	outLabelsVec.push_back("path type");
	outValuesVec.push_back(TranslatorTk::benchPathTypeToStr(benchPathType) );

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

	outLabelsVec.push_back("random amount");
	outValuesVec.push_back(!useRandomOffsets ? "" : std::to_string(randomAmount) );

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
