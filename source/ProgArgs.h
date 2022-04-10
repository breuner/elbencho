#ifndef PROGARGS_H_
#define PROGARGS_H_

#include <boost/program_options.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <time.h>
#include "Common.h"
#include "CuFileHandleData.h"
#include "Logger.h"
#include "PathStore.h"
#include "toolkits/random/RandAlgoSelectorTk.h"
#include "toolkits/SystemTk.h"


namespace bpo = boost::program_options;
namespace bpt = boost::property_tree;


#define ARG_HELP_LONG 				"help"
#define ARG_HELP_SHORT 				"h"
#define ARG_HELPBLOCKDEV_LONG 		"help-bdev" // for backwards compat (new: help-large)
#define ARG_HELPLARGE_LONG 			"help-large"
#define ARG_HELPMULTIFILE_LONG 		"help-multi"
#define ARG_HELPS3_LONG 			"help-s3"
#define ARG_HELPDISTRIBUTED_LONG 	"help-dist"
#define ARG_HELPALLOPTIONS_LONG	 	"help-all"
#define ARG_BENCHPATHS_LONG			"path"
#define ARG_NUMTHREADS_LONG			"threads"
#define ARG_NUMTHREADS_SHORT 		"t"
#define ARG_NUMDIRS_LONG 			"dirs"
#define ARG_NUMDIRS_SHORT 			"n"
#define ARG_NUMFILES_LONG	 		"files"
#define ARG_NUMFILES_SHORT 			"N"
#define ARG_FILESIZE_LONG	 		"size"
#define ARG_FILESIZE_SHORT 			"s"
#define ARG_BLOCK_LONG	 			"block"
#define ARG_BLOCK_SHORT 			"b"
#define ARG_CONFIGFILE_LONG			"configfile"
#define ARG_CONFIGFILE_SHORT  		"c"
#define ARG_DIRECTIO_LONG 			"direct"
#define ARG_NOLIVESTATS_LONG 		"nolive"
#define ARG_IGNOREDELERR_LONG		"nodelerr"
#define ARG_IGNORE0USECERR_LONG		"no0usecerr"
#define ARG_CREATEDIRS_LONG			"mkdirs"
#define ARG_CREATEDIRS_SHORT		"d"
#define ARG_CREATEFILES_LONG		"write"
#define ARG_CREATEFILES_SHORT		"w"
#define ARG_DELETEFILES_LONG		"delfiles"
#define ARG_DELETEFILES_SHORT		"F"
#define ARG_DELETEDIRS_LONG			"deldirs"
#define ARG_DELETEDIRS_SHORT		"D"
#define ARG_READ_LONG				"read"
#define ARG_READ_SHORT				"r"
#define ARG_STARTTIME_LONG			"start"
#define ARG_RUNASSERVICE_LONG		"service"
#define ARG_FOREGROUNDSERVICE_LONG	"foreground"
#define ARG_SERVICEPORT_LONG		"port"
#define ARG_NODETACH_LONG			"nodetach"
#define ARG_HOSTS_LONG				"hosts"
#define ARG_HOSTSFILE_LONG			"hostsfile"
#define ARG_NUMHOSTS_LONG			"numhosts"
#define ARG_INTERRUPT_LONG			"interrupt"
#define ARG_ITERATIONS_LONG			"iterations"
#define ARG_ITERATIONS_SHORT		"i"
#define ARG_QUIT_LONG				"quit"
#define ARG_NOSVCPATHSHARE_LONG		"nosvcshare"
#define ARG_RANKOFFSET_LONG			"rankoffset"
#define ARG_LOGLEVEL_LONG			"log"
#define ARG_NUMAZONES_LONG			"zones"
#define ARG_CPUCORES_LONG			"cores"
#define ARG_SHOWALLELAPSED_LONG		"allelapsed"
#define ARG_RANDOMOFFSETS_LONG		"rand"
#define ARG_RANDOMALIGN_LONG		"randalign"
#define ARG_RANDOMAMOUNT_LONG		"randamount"
#define ARG_NUMDATASETTHREADS_LONG	"datasetthreads" // internal (not set by user)
#define ARG_IODEPTH_LONG			"iodepth"
#define ARG_LATENCY_LONG			"lat"
#define ARG_LATENCYPERCENTILES_LONG	"latpercent"
#define ARG_LATENCYPERCENT9S_LONG	"latpercent9s"
#define ARG_LATENCYHISTOGRAM_LONG	"lathisto"
#define ARG_TRUNCATE_LONG			"trunc"
#define ARG_RESULTSFILE_LONG		"resfile"
#define ARG_TIMELIMITSECS_LONG		"timelimit"
#define ARG_CSVFILE_LONG			"csvfile"
#define ARG_CSVLIVEFILE_LONG		"livecsv"
#define ARG_CSVLIVEEXTENDED_LONG	"livecsvex"
#define ARG_LIVEINTERVAL_LONG		"liveint"
#define ARG_NOCSVLABELS_LONG		"nocsvlabels"
#define ARG_GPUIDS_LONG				"gpuids"
#define ARG_GPUPERSERVICE_LONG		"gpuperservice"
#define ARG_CUFILE_LONG				"cufile"
#define ARG_GDSBUFREG_LONG			"gdsbufreg"
#define ARG_CUFILEDRIVEROPEN_LONG	"cufiledriveropen"
#define ARG_CUHOSTBUFREG_LONG		"cuhostbufreg"
#define ARG_INTEGRITYCHECK_LONG		"verify"
#define ARG_SYNCPHASE_LONG			"sync"
#define ARG_DROPCACHESPHASE_LONG	"dropcache"
#define ARG_STATFILES_LONG			"stat"
#define ARG_CPUUTIL_LONG			"cpu"
#define ARG_SVCUPDATEINTERVAL_LONG	"svcupint"
#define ARG_VERSION_LONG			"version"
#define ARG_TRUNCTOSIZE_LONG		"trunctosize"
#define ARG_PREALLOCFILE_LONG		"preallocfile"
#define ARG_DIRSHARING_LONG			"dirsharing"
#define ARG_VERIFYDIRECT_LONG		"verifydirect"
#define ARG_BLOCKVARIANCE_LONG		"blockvarpct"
#define ARG_RWMIXPERCENT_LONG		"rwmixpct"
#define ARG_BLOCKVARIANCEALGO_LONG	"blockvaralgo"
#define ARG_RANDSEEKALGO_LONG		"randalgo"
#define ARG_TREEFILE_LONG			"treefile"
#define ARG_FILESHARESIZE_LONG		"sharesize"
#define ARG_TREERANDOMIZE_LONG		"treerand"
#define ARG_TREEROUNDUP_LONG		"treeroundup"
#define ARG_S3ENDPOINTS_LONG		"s3endpoints"
#define ARG_S3ACCESSKEY_LONG		"s3key"
#define ARG_S3ACCESSSECRET_LONG		"s3secret"
#define ARG_S3REGION_LONG			"s3region"
#define ARG_S3FASTGET_LONG			"s3fastget"
#define ARG_S3TRANSMAN_LONG			"s3transman"
#define ARG_S3LOGLEVEL_LONG			"s3log"
#define ARG_NODIRECTIOCHECK_LONG	"nodiocheck"
#define ARG_S3OBJECTPREFIX_LONG		"s3objprefix"
#define ARG_DRYRUN_LONG				"dryrun"
#define ARG_S3LISTOBJ_LONG			"s3listobj"
#define ARG_S3LISTOBJPARALLEL_LONG	"s3listobjpar"
#define ARG_S3LISTOBJVERIFY_LONG	"s3listverify"
#define ARG_REVERSESEQOFFSETS_LONG	"backward"
#define ARG_INFINITEIOLOOP_LONG		"infloop"
#define ARG_S3SIGNPAYLOAD_LONG		"s3sign"
#define ARG_S3RANDOBJ_LONG			"s3randobj"
#define ARG_RWMIXTHREADS_LONG		"rwmixthr"
#define ARG_BRIEFLIFESTATS_LONG		"live1"
#define ARG_GPUDIRECTSSTORAGE_LONG	"gds"
#define ARG_BENCHLABEL_LONG			"label"
#define ARG_NOFDSHARING_LONG		"nofdsharing"
#define ARG_LIMITREAD_LONG			"limitread"
#define ARG_LIMITWRITE_LONG			"limitwrite"
#define ARG_S3NOMPCHECK_LONG		"s3nompcheck"
#define ARG_DIRSTATS_LONG			"dirstats"


#define ARGDEFAULT_SERVICEPORT		1611
#define ARGDEFAULT_SERVICEPORT_STR	STRINGIZE(ARGDEFAULT_SERVICEPORT)


#define SERVICE_UPLOAD_BASEPATH(servicePort)	("/var/tmp/" EXE_NAME "_" + \
												SystemTk::getUsername() + "_" + \
												"p" + std::to_string(servicePort) )
#define SERVICE_UPLOAD_TREEFILE					"treefile.txt"
#define S3_IMPLICIT_TREEFILE_PATH				("/var/tmp/" EXE_NAME "_" + \
												SystemTk::getUsername() + "_" + \
												"treefile_implicit.txt")


typedef std::vector<CuFileHandleData> CuFileHandleDataVec;
typedef std::vector<CuFileHandleData*> CuFileHandleDataPtrVec;


/**
 * Program arguments parser and central config store.
 */
class ProgArgs
{
	public:
		ProgArgs(int argc, char** argv);
		~ProgArgs();
		void defineAllowedArgs();
		bool hasUserRequestedHelp();
		void printHelp();
		bool hasUserRequestedVersion();
		bool hasUserRequestedDryRun();
		void printVersionAndBuildInfo();
		void printDryRunInfo();
		void setFromPropertyTreeForService(bpt::ptree& tree);
		void getAsPropertyTreeForService(bpt::ptree& outTree, size_t serviceRank) const;
		void getAsStringVec(StringVec& outLabelsVec, StringVec& outValuesVec) const;
		void resetBenchPath();
		void getBenchPathInfoTree(bpt::ptree& outTree);
		void checkServiceBenchPathInfos(BenchPathInfoVec& benchPathInfos);


	private:
		bpo::options_description argsGenericDescription;
		bpo::options_description argsHiddenDescription;
		bpo::variables_map argsVariablesMap;

		bool isCuFileDriverOpen{false}; // to ensure cuFileDriverOpen/-Close is only called once

		int argc; // command line argument count (as in main(argc, argv) )
		char** argv; // command line arg vector (as in main(argc, argv) )

		struct // file and dir paths for custom tree mode
		{
			PathStore dirs; // contains only dirs
			PathStore filesNonShared; // file sizes < fileShareSize
			PathStore filesShared; // file sizes >= fileShareSize
		} customTree;

		std::string progPath; // absolute path to program binary
		std::string benchPathStr; // benchmark path(s), separated by BENCHPATH_DELIMITER
		StringVec benchPathsVec; // benchPathStr split into individual paths
		StringVec benchPathsServiceOverrideVec; // set in service mode to override bench paths
		IntVec benchPathFDsVec; // file descriptors to individual bench paths
		BenchPathType benchPathType; /* for local runs auto-detected based on benchPathStr;
										in master mode received from service hosts */
		size_t numThreads; // parallel I/O worker threads per instance
		size_t numDataSetThreads; /* global threads working on same dataset for service mode hosts
									based on isBenchPathShared; otherwise equal to numThreads */
		size_t numDirs; // directories per thread
		std::string numDirsOrigStr; // original numDirs str from user with unit
		size_t numFiles; // files per directory
		std::string numFilesOrigStr; // original numDirs str from user with unit
		uint64_t fileSize; // size per file
		std::string fileSizeOrigStr; // original fileSize str from user with unit
		size_t blockSize; // number of bytes to read/write in a single read()/write() call
		std::string blockSizeOrigStr; // original blockSize str from user with unit
		size_t iterations; // Number of iterations of the same benchmark
		bool useDirectIO; // open files with O_DIRECT
		bool disableLiveStats; // disable live stats
		bool ignoreDelErrors; // ignore ENOENT errors on file/dir deletion
		bool ignore0USecErrors; // ignore worker completion in less than 1 millisecond
		bool runCreateDirsPhase; // create directories
		bool runCreateFilesPhase; // create files
		bool runReadPhase; // read files
		bool runDeleteFilesPhase; // delete files
		bool runDeleteDirsPhase; // delete dirs
		time_t startTime; /* UTC start time to coordinate multiple benchmarks, in seconds since the,
								epoch. 0 means immediate start. */
		bool runAsService; // run as service for remote coordination by master
		bool runServiceInForeground; // true to not daemonize service process into background
		unsigned short servicePort; // HTTP/TCP port for service
		std::string hostsStr; // list of service hosts, element format is hostname[:port]
		std::string hostsFilePath; // path to file for service hosts
		StringVec hostsVec; // service hosts broken down into individual hostname[:port]
		int numHosts; // number of hosts to use from hostsStr/hostsFilePath ("-1" means "all")
		bool interruptServices; // send interrupt msg to given hosts to stop current phase
		bool quitServices; // send quit (via interrupt msg) to given hosts to exit service
		bool noSharedServicePath; // true if bench paths not shared between service instances
		size_t rankOffset; // offset for worker rank numbers
		unsigned short logLevel; // filter level for log messages (higher will not be logged)
		std::string numaZonesStr; // comma-separted numa zones that this process may run on
		IntVec numaZonesVec; // list from numaZoneStr broken down into individual elements
		std::string cpuCoresStr; // comma-separted cpu cores that this process may run on
		IntVec cpuCoresVec; // list from cpuCoresStr broken down into individual elements
		bool showAllElapsed; // print elapsed time of each I/O worker
		size_t liveStatsSleepMS; // interval between live stats console/csv updates
		bool useRandomOffsets; // use random offsets for file reads/writes
		bool useRandomAligned; // use block-aligned random offsets (when randomOffsets is used)
		uint64_t randomAmount; // random bytes to read/write per file (when randomOffsets is used)
		std::string randomAmountOrigStr; // original randomAmount str from user with unit
		size_t ioDepth; // depth of io queue per thread for libaio
		bool showLatency; // show min/avg/max latency
		bool showLatencyPercentiles; // show latency percentiles
		unsigned short numLatencyPercentile9s; // decimal 9s to show (0=99%, 1=99.9%, 2=99.99%, ...)
		bool showLatencyHistogram; // show latency histogram
		bool doTruncate; // truncate files to 0 size on open for writing
		std::string resFilePath; // results output file path (or empty for no results file)
		size_t timeLimitSecs; // time limit in seconds for each phase (0 to disable)
		std::string configFilePath; // Configuration input using a config file (empty for none)
		std::string csvFilePath; // phase results file path for csv format (or empty for none)
		std::string liveCSVFilePath; // live stats file path for csv format (or empty for none)
		bool useExtendedLiveCSV; // false for total/aggregate results only, true for per-worker
		bool noCSVLabels; // true to not print headline with labels to csv file
		std::string gpuIDsStr; // list of gpu IDs, separated by GPULIST_DELIMITERS
		IntVec gpuIDsVec; // gpuIDsStr broken down into individual GPU IDs
		std::string gpuIDsServiceOverride; // set in service mode to override gpu IDs
		bool assignGPUPerService; // assign GPUs from gpuIDsVec round robin per service
		bool useCuFile; // use cuFile API for reads/writes to/from GPU memory
		bool useGDSBufReg; // register GPU buffers for GPUDirect Storage (GDS) when using cuFile API
		CuFileHandleDataVec cuFileHandleDataVec; /* registered cuFile handles in file/bdev mode;
			vec will also be filled (with unreg'ed handles) if cuFile API is not selected to make
			things easier for localworkers */
		bool useCuFileDriverOpen; // true to call cuFileDriverOpen when using cuFile API
		bool useCuHostBufReg; // register/pin host buffer to speed up copy into GPU memory
		uint64_t integrityCheckSalt; // salt to add to data integrity checksum (0 disables check)
		bool runSyncPhase; // run the sync() phase to commit all dirty page cache buffers
		bool runDropCachesPhase; // run "echo 3>drop_caches" phase to drop kernel page cache
		bool runStatFilesPhase; // stat files
		bool showCPUUtilization; // show cpu utilization in phase stats results
		size_t svcUpdateIntervalMS; // update retrieval interval for service hosts in milliseconds
		bool doTruncToSize; // truncate files to size on creation via ftruncate()
		bool doPreallocFile; // prealloc file space on creation via posix_fallocate()
		bool doDirSharing; // workers use same dirs in dir mode (instead of unique dir per worker)
		bool doDirectVerify; // verify data integrity by reading immediately after write
		unsigned blockVariancePercent; // % of blocks that should differ between writes
		unsigned rwMixPercent; // % of blocks that should be read (the rest will be written)
		bool useRWMixPercent; // implicitly set in case of rwmixpct (even if ==0)
		std::string blockVarianceAlgo; // rand algo for buffer fill variance
		std::string randOffsetAlgo; // rand algo for random offsets
		std::string treeFilePath; // path to file containing custom tree (list of dirs and files)
		uint64_t fileShareSize; /* in custom tree mode, file size as of which to write/read shared.
									(default 0 means 32 times blockSize) */
		std::string fileShareSizeOrigStr; // original fileShareSize str from user with unit
		bool useCustomTreeRandomize; // randomize order of custom tree files
		uint64_t treeRoundUpSize; /* in treefile, round up file sizes to multiple of given size.
			(useful for directIO with its alignment reqs on some file systems. 0 disables this.) */
		std::string treeRoundUpSizeOrigStr; // original treeRoundUpSize str from user with unit
		std::string s3EndpointsStr; // user-given s3 endpoints; elem format: [http(s)://]host[:port]
		std::string s3EndpointsServiceOverrideStr; // override of s3EndpointStr in service mode
		StringVec s3EndpointsVec; // s3 endpoints broken down into individual elements
		std::string s3AccessKey; // s3 access key
		std::string s3AccessSecret; // s3 access secret
		std::string s3Region; // s3 region
		bool useS3FastRead; /* get objects to /dev/null instead of buffer (i.e. no post processing
									via buffer possible, such as GPU copy or data verification) */
		bool useS3TransferManager; // use AWS SDK TransferManager for object downloads
		unsigned short s3LogLevel; // log level for AWS SDK
		bool noDirectIOCheck; // ignore directIO alignment and sanity checks
		std::string s3ObjectPrefix; // object name/path prefix for s3 "directory mode"
		uint64_t runS3ListObjNum; // run seq list objects phase if >0, given number is listing limit
		bool runS3ListObjParallel; // multi-threaded object listing (requires "-n" / "-N")
		bool doS3ListObjVerify; // verify object listing (requires "-n" / "-N")
		bool doReverseSeqOffsets; // backwards sequential read/write
		bool doInfiniteIOLoop; // start I/O from the beginning when reaching the end
		unsigned short s3SignPolicy; // Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy
		bool useS3RandObjSelect; // random object selection for each read
		size_t numRWMixReadThreads; // number of rwmix read threads in file/bdev write phase
		bool useRWMixReadThreads; // implicitly set in case of rwmixthr (even if ==0)
		bool useBriefLiveStats; // single-line live stats
		std::string benchLabel; // user-defined label for benchmark run
		std::string benchLabelNoCommas; // implict based on benchLabel with commas removed for csv
		bool useNoFDSharing; // when true, each worker does its own file open in file/bdev mode
		uint64_t limitReadBps; // read limit per thread in bytes per sec
		std::string limitReadBpsOrigStr; // original limitReadBps str from user with unit
		uint64_t limitWriteBps; // write limit per thread in bytes per sec
		std::string limitWriteBpsOrigStr; // original limitWriteBps str from user with unit
		bool ignoreS3PartNum; // don't check for >10K parts in multi-part uploads
		bool showDirStats; // show processed dirs stats in file write/read phase of dir mode


		void defineDefaults();
		void initImplicitValues();
		void convertUnitStrings();
		void checkArgs();
		void checkPathDependentArgs();
		void parseAndCheckPaths();
		void convertS3PathsToCustomTree();
		void prepareBenchPathFDsVec();
		void prepareCuFileHandleDataVec();
		void prepareFileSize(int fd, std::string& path);
		void parseHosts();
		void parseNumaZones();
		void parseCPUCores();
		void parseGPUIDs();
		void parseRandAlgos();
		void parseS3Endpoints();
		void loadCustomTreeFile();
		void splitCustomTreeForSharedS3Upload();
		std::string absolutePath(std::string pathStr);
		BenchPathType findBenchPathType(std::string pathStr);
		bool checkPathExists(std::string pathStr);
		void checkCSVFileCompatibility();

		void printHelpOverview();
		void printHelpAllOptions();
		void printHelpBlockDev();
		void printHelpMultiFile();
		void printHelpS3();
		void printHelpDistributed();


	// inliners
	public:
		int getProgArgCount() const { return argc; }
		char** getProgArgVec() const { return argv; }

		std::string getProgPath() const { return progPath; }
		std::string getBenchPathStr() const { return benchPathStr; }
		const StringVec& getBenchPaths() const { return benchPathsVec; }
		const StringVec& getBenchPathsServiceOverride() const
			{ return benchPathsServiceOverrideVec; }
		const IntVec& getBenchPathFDs() const { return benchPathFDsVec; }
		BenchPathType getBenchPathType() const { return benchPathType; }
		size_t getNumDirs() const { return numDirs; }
		size_t getNumFiles() const { return numFiles; }
		size_t getNumThreads() const { return numThreads; }
		size_t getNumDataSetThreads() const { return numDataSetThreads; }
		size_t getIterations() const { return iterations; } 
		uint64_t getFileSize() const { return fileSize; }
		size_t getBlockSize() const { return blockSize; }
		bool getUseDirectIO() const { return useDirectIO; }
		bool getDisableLiveStats() const { return disableLiveStats; }
		bool getIgnoreDelErrors() const { return ignoreDelErrors; }
		void setIgnoreDelErrors(bool ignoreDelErrors) { this->ignoreDelErrors = ignoreDelErrors; }
		bool getIgnore0USecErrors() const { return ignore0USecErrors; }
		bool getRunCreateDirsPhase() const { return runCreateDirsPhase; }
		bool getRunCreateFilesPhase() const { return runCreateFilesPhase; }
		bool getRunReadPhase() const { return runReadPhase; }
		bool getRunDeleteFilesPhase() const { return runDeleteFilesPhase; }
		bool getRunDeleteDirsPhase() const { return runDeleteDirsPhase; }
		time_t getStartTime() const { return startTime; }
		bool getRunAsService() const { return runAsService; }
		bool getRunServiceInForeground() const { return runServiceInForeground; }
		unsigned short getServicePort() const { return servicePort; }
		std::string getHostsStr() const { return hostsStr; }
		std::string getHostsFilePath() const { return hostsFilePath; }
		const StringVec& getHostsVec() const { return hostsVec; }
		int getNumHosts() const { return numHosts; }
		bool getInterruptServices() const { return interruptServices; }
		bool getQuitServices() const { return quitServices; }
		bool getIsServicePathShared() const { return !noSharedServicePath; }
		size_t getRankOffset() const { return rankOffset; }
		LogLevel getLogLevel() const { return (LogLevel)logLevel; }
		std::string getNumaZonesStr() const { return numaZonesStr; }
		const IntVec& getNumaZonesVec() const { return numaZonesVec; }
		std::string getCPUCoresStr() const { return cpuCoresStr; }
		const IntVec& getCPUCoresVec() const { return cpuCoresVec; }
		bool getShowAllElapsed() const { return showAllElapsed; }
		size_t getLiveStatsSleepMS() const { return liveStatsSleepMS; }
		bool getUseRandomOffsets() const { return useRandomOffsets; }
		bool getUseRandomAligned() const { return useRandomAligned; }
		uint64_t getRandomAmount() const { return randomAmount; }
		size_t getIODepth() const { return ioDepth; }
		bool getShowLatency() const { return showLatency; }
		bool getShowLatencyPercentiles() const { return showLatencyPercentiles; }
		unsigned short getNumLatencyPercentile9s() const { return numLatencyPercentile9s; }
		bool getShowLatencyHistogram() const { return showLatencyHistogram; }
		bool getDoTruncate() const { return doTruncate; }
		std::string getResFilePath() const { return resFilePath; }
		size_t getTimeLimitSecs() const { return timeLimitSecs; }
		void setTimeLimitSecs(size_t timeLimitSecs) { this->timeLimitSecs = timeLimitSecs; }
		std::string getCSVFilePath() const { return csvFilePath; }
		std::string getLiveCSVFilePath() const { return liveCSVFilePath; }
		bool getUseExtendedLiveCSV() const { return useExtendedLiveCSV; }
		std::string getConfigFilePath() const { return configFilePath; }
		bool getPrintCSVLabels() const { return !noCSVLabels; }
		std::string getGPUIDsStr() const { return gpuIDsStr; }
		const IntVec& getGPUIDsVec() const { return gpuIDsVec; }
		std::string getGPUIDsServiceOverride() const { return gpuIDsServiceOverride; }
		bool getAssignGPUPerService() const { return assignGPUPerService; }
		bool getUseCuFile() const { return useCuFile; }
		bool getUseGPUBufReg() const { return useGDSBufReg; }
		CuFileHandleDataVec& getCuFileHandleDataVec() { return cuFileHandleDataVec; }
		bool getUseCuFileDriverOpen() const { return useCuFileDriverOpen; }
		bool getUseCuHostBufReg() const { return useCuHostBufReg; }
		uint64_t getIntegrityCheckSalt() const { return integrityCheckSalt; }
		bool getRunSyncPhase() const { return runSyncPhase; }
		bool getRunDropCachesPhase() const { return runDropCachesPhase; }
		bool getRunStatFilesPhase() const { return runStatFilesPhase; }
		bool getShowCPUUtilization() const { return showCPUUtilization; }
		size_t getSvcUpdateIntervalMS() const { return svcUpdateIntervalMS; }
		bool getDoTruncToSize() const { return doTruncToSize; }
		bool getDoPreallocFile() const { return doPreallocFile; }
		bool getDoDirSharing() const { return doDirSharing; }
		bool getDoDirectVerify() const { return doDirectVerify; }
		unsigned getBlockVariancePercent() const { return blockVariancePercent; }
		unsigned getRWMixPercent() const { return rwMixPercent; }
		bool hasUserSetRWMixPercent() const { return useRWMixPercent; }
		std::string getBlockVarianceAlgo() const { return blockVarianceAlgo; }
		std::string getRandOffsetAlgo() const { return randOffsetAlgo; }
		std::string getTreeFilePath() const { return treeFilePath; }
		const PathStore& getCustomTreeDirs() const { return customTree.dirs; }
		const PathStore& getCustomTreeFilesNonShared() const { return customTree.filesNonShared; }
		const PathStore& getCustomTreeFilesShared() const { return customTree.filesShared; }
		uint64_t getFileShareSize() const { return fileShareSize; }
		bool getUseCustomTreeRandomize() const { return useCustomTreeRandomize; }
		uint64_t getTreeRoundUpSize() const { return treeRoundUpSize; }
		std::string getS3EndpointsStr() const { return s3EndpointsStr; }
		std::string getS3EndpointsServiceOverride() const { return s3EndpointsServiceOverrideStr; }
		const StringVec& getS3EndpointsVec() const { return s3EndpointsVec; }
		std::string getS3AccessKey() const { return s3AccessKey; }
		std::string getS3AccessSecret() const { return s3AccessSecret; }
		std::string getS3Region() const { return s3Region; }
		bool getUseS3FastRead() const { return useS3FastRead; }
		bool getUseS3TransferManager() const { return useS3TransferManager; }
		unsigned short getS3LogLevel() const { return s3LogLevel; }
		bool getNoDirectIOCheck() const { return noDirectIOCheck; }
		std::string getS3ObjectPrefix() const { return s3ObjectPrefix; }
		uint64_t getS3ListObjNum() const { return runS3ListObjNum; }
		bool getRunListObjPhase() const { return (runS3ListObjNum > 0); }
		bool getRunListObjParallelPhase() const { return runS3ListObjParallel; }
		bool getDoListObjVerify() const { return doS3ListObjVerify; }
		bool getDoReverseSeqOffsets() const { return doReverseSeqOffsets; }
		bool getDoInfiniteIOLoop() const { return doInfiniteIOLoop; }
		unsigned short getS3SignPolicy() const { return s3SignPolicy; }
		bool getUseS3RandObjSelect() const { return useS3RandObjSelect; }
		size_t getNumRWMixReadThreads() const { return numRWMixReadThreads; }
		bool hasUserSetRWMixReadThreads() const { return useRWMixReadThreads; }
		bool getUseBriefLiveStats() const { return useBriefLiveStats; }
		std::string getBenchLabel() const { return benchLabel; }
		const std::string& getBenchLabelNoCommas() const { return benchLabelNoCommas; }
		bool getUseNoFDSharing() const { return useNoFDSharing; }
		uint64_t getLimitReadBps() const { return limitReadBps; }
		uint64_t getLimitWriteBps() const { return limitWriteBps; }
		bool getIgnoreS3PartNum() const { return ignoreS3PartNum; }
		bool getShowDirStats() const { return showDirStats; }

};


#endif /* PROGARGS_H_ */
