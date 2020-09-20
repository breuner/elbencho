#ifndef PROGARGS_H_
#define PROGARGS_H_

#include <boost/program_options.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <time.h>
#include "Common.h"
#include "CuFileHandleData.h"
#include "Logger.h"


namespace bpo = boost::program_options;


#define ARG_HELP_LONG 				"help"
#define ARG_HELP_SHORT 				"h"
#define ARG_HELPBLOCKDEV_LONG 		"help-bdev"
#define ARG_HELPMULTIFILE_LONG 		"help-multi"
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
#define ARG_DIRECTIO_LONG 			"direct"
#define ARG_PERTHREADSTATS_LONG 	"perthread"
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
#define ARG_INTERRUPT_LONG			"interrupt"
#define ARG_QUIT_LONG				"quit"
#define ARG_PATHNOTSHARED_LONG		"notshared"
#define ARG_RANKOFFSET_LONG			"rankoffset"
#define ARG_LOGLEVEL_LONG			"log"
#define ARG_NUMAZONES_LONG			"zones"
#define ARG_SHOWALLELAPSED_LONG		"allelapsed"
#define ARG_LIVESLEEPSEC_LONG		"refresh"
#define ARG_RANDOMOFFSETS_LONG		"rand"
#define ARG_RANDOMALIGN_LONG		"randalign"
#define ARG_RANDOMAMOUNT_LONG		"randamount"
#define ARG_NUMDATASETTHREADS_LONG	"datasetthreads"
#define ARG_IODEPTH_LONG			"iodepth"
#define ARG_LATENCY_LONG			"lat"
#define ARG_LATENCYPERCENTILES_LONG	"latpercent"
#define ARG_LATENCYHISTOGRAM_LONG	"lathisto"
#define ARG_TRUNCATE_LONG			"trunc"
#define ARG_RESULTSFILE_LONG		"resfile"
#define ARG_TIMELIMITSECS_LONG		"timelimit"
#define ARG_CSVFILE_LONG			"csvfile"
#define ARG_NOCSVLABELS_LONG		"nocsvlabels"
#define ARG_GPUIDS_LONG				"gpuids"
#define ARG_GPUPERSERVICE_LONG		"gpuperservice"
#define ARG_CUFILE_LONG				"cufile"
#define ARG_GPUBUFREG_LONG			"gpubufreg"
#define ARG_CUFILEDRIVEROPEN_LONG	"cufiledriveropen"
#define ARG_CUHOSTBUFREG_LONG		"cuhostbufreg"
#define ARG_INTEGRITYCHECK_LONG		"verify"
#define ARG_SYNCPHASE_LONG			"sync"
#define ARG_DROPCACHESPHASE_LONG	"dropcache"
#define ARG_STATFILES_LONG			"stat"


#define ARGDEFAULT_SERVICEPORT		1611
#define ARGDEFAULT_SERVICEPORT_STR	STRINGIZE(ARGDEFAULT_SERVICEPORT)


namespace bpt = boost::property_tree;


typedef std::vector<CuFileHandleData> CuFileHandleDataVec;


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
		void setFromPropertyTree(bpt::ptree& tree);
		void getAsPropertyTree(bpt::ptree& outTree, size_t workerRank) const;
		void getAsStringVec(StringVec& outLabelsVec, StringVec& outValuesVec) const;
		void resetBenchPath();
		void setBenchPathType(BenchPathType benchPathType);


	private:
		bpo::options_description argsGenericDescription;
		bpo::options_description argsHiddenDescription;
		bpo::variables_map argsVariablesMap;

		bool isCuFileDriverOpen{false}; // to ensure cuFileDriverOpen/-Close is only called once

		int argc; // command line argument count (as in main(argc, argv) ).
		char** argv; // command line arg vector (as in main(argc, argv) ).

		std::string progPath; // absolute path to program binary
		std::string benchPathStr; // path(s) to benchmark base dirs separated by BENCHPATH_DELIMITER
		StringVec benchPathsVec; // benchPathStr split into individual paths
		IntVec benchPathFDsVec; // file descriptors to individual bench paths
		BenchPathType benchPathType; /* for local runs auto-detected based on benchPathStr;
										in master mode received from service hosts */
		size_t numThreads; // parallel I/O worker threads
		size_t numDataSetThreads; /* global threads working on same dataset for service mode hosts
									based on isBenchPathNotShared; otherwise equal to numThreads */
		size_t numDirs; // directories per thread
		std::string numDirsOrigStr; // original numDirs str from user with unit
		size_t numFiles; // files per directory
		std::string numFilesOrigStr; // original numDirs str from user with unit
		uint64_t fileSize; // size per file
		std::string fileSizeOrigStr; // original fileSize str from user with unit
		size_t blockSize; // number of bytes to read/write in a single read()/write() call
		std::string blockSizeOrigStr; // original blockSize str from user with unit
		bool useDirectIO; // open files with O_DIRECT
		bool showPerThreadStats; // show stats per thread instead of total status for all threads
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
		StringVec hostsVec; // service hosts broken down into individual hostname[:port]
		bool interruptServices; // send interrupt msg to given hosts to stop current phase
		bool quitServices; // send quit (via interrupt msg) to given hosts to exit service
		bool isBenchPathNotShared; // true if bench paths are not shared between different hosts
		size_t rankOffset; // offset for worker rank numbers
		unsigned short logLevel; // filter level for log messages (higher will not be logged)
		std::string numaZonesStr; // comma-separted numa zones that this process may run on
		IntVec numaZonesVec; // list from numaZoneStr broken down into individual elements
		bool showAllElapsed; // print elapsed time of each I/O worker
		size_t liveStatsSleepSec; // sleep interval between live stats console refresh
		bool useRandomOffsets; // use random offsets for file reads/writes
		bool useRandomAligned; // use block-aligned random offsets (when randomOffsets is used)
		size_t randomAmount; // random bytes to read/write per file (when randomOffsets is used)
		std::string randomAmountOrigStr; // original randomAmount str from user with unit
		size_t ioDepth; // depth of io queue per thread for libaio
		bool showLatency; // show min/avg/max latency
		bool showLatencyPercentiles; // show latency percentiles
		bool showLatencyHistogram; // show latency histogram
		bool doTruncate; // truncate files to 0 size on open for writing
		std::string resFilePath; // results output file path (or empty for no results file)
		size_t timeLimitSecs; // time limit in seconds for each phase (0 to disable)
		std::string csvFilePath; // results output file path for csv format (or empty for none)
		bool noCSVLabels; // true to not print headline with labels to csv file
		std::string gpuIDsStr; // list of gpu IDs, separated by GPULIST_DELIMITERS
		IntVec gpuIDsVec; // gpuIDsStr broken down into individual GPU IDs
		bool assignGPUPerService; // assign GPUs from gpuIDsVec round robin per service
		bool useCuFile; // use cuFile API for reads/writes to/from GPU memory
		bool useGPUBufReg; // register GPU buffers for GPUDirect Storage (GDS) when using cuFile API
		CuFileHandleDataVec cuFileHandleDataVec; /* registered cuFile handles in file/bdev mode;
			vec will also be filled (with unreg'ed handles) if cuFile API is not selected to make
			things easier for localworkers */
		bool useCuFileDriverOpen; // true to call cuFileDriverOpen when using cuFile API
		bool useCuHostBufReg; // register/pin host buffer to speed up copy into GPU memory
		uint64_t integrityCheckSalt; // salt to add to data integrity checksum (0 disables check)
		bool runSyncPhase; // run the sync() phase to commit all dirty page cache buffers
		bool runDropCachesPhase; // run "echo 3>drop_caches" phase to drop kernel page cache
		bool runStatFilesPhase; // stat files

		void defineDefaults();
		void convertUnitStrings();
		void checkArgs();
		void checkPathDependentArgs();
		void parseAndCheckPaths();
		void prepareBenchPathFDsVec();
		void prepareCuFileHandleDataVec();
		void prepareFileSize(int fd, std::string& path);
		void parseHosts();
		void parseNumaZones();
		void parseGPUIDs();
		std::string absolutePath(std::string pathStr);
		BenchPathType findBenchPathType(std::string pathStr);
		bool checkPathExists(std::string pathStr);

		void printHelpOverview();
		void printHelpAllOptions();
		void printHelpBlockDev();
		void printHelpMultiFile();
		void printHelpDistributed();


	// inliners
	public:
		int getProgArgCount() const { return argc; }
		char** getProgArgVec() const { return argv; }

		std::string getProgPath() const { return progPath; }
		std::string getBenchPathStr() const { return benchPathStr; }
		const StringVec& getBenchPaths() const { return benchPathsVec; }
		const IntVec& getBenchPathFDs() const { return benchPathFDsVec; }
		BenchPathType getBenchPathType() const { return benchPathType; }
		size_t getNumDirs() const { return numDirs; }
		size_t getNumFiles() const { return numFiles; }
		size_t getNumThreads() const { return numThreads; }
		size_t getNumDataSetThreads() const { return numDataSetThreads; }
		uint64_t getFileSize() const { return fileSize; }
		size_t getBlockSize() const { return blockSize; }
		bool getUseDirectIO() const { return useDirectIO; }
		bool getShowPerThreadStats() const { return showPerThreadStats; }
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
		const StringVec& getHostsVec() const { return hostsVec; }
		bool getInterruptServices() const { return interruptServices; }
		bool getQuitServices() const { return quitServices; }
		bool getIsBenchPathNotShared() const { return isBenchPathNotShared; }
		bool getIsBenchPathShared() const { return !isBenchPathNotShared; }
		size_t getRankOffset() const { return rankOffset; }
		LogLevel getLogLevel() const { return (LogLevel)logLevel; }
		std::string getNumaZonesStr() const { return numaZonesStr; }
		const IntVec& getNumaZonesVec() const { return numaZonesVec; }
		bool getShowAllElapsed() const { return showAllElapsed; }
		size_t getLiveStatsSleepSec() const { return liveStatsSleepSec; }
		bool getUseRandomOffsets() const { return useRandomOffsets; }
		bool getUseRandomAligned() const { return useRandomAligned; }
		size_t getRandomAmount() const { return randomAmount; }
		size_t getIODepth() const { return ioDepth; }
		bool getShowLatency() const { return showLatency; }
		bool getShowLatencyPercentiles() const { return showLatencyPercentiles; }
		bool getShowLatencyHistogram() const { return showLatencyHistogram; }
		bool getDoTruncate() const { return doTruncate; }
		std::string getResFilePath() const { return resFilePath; }
		size_t getTimeLimitSecs() const { return timeLimitSecs; }
		std::string getCSVFilePath() const { return csvFilePath; }
		bool getPrintCSVLabels() const { return !noCSVLabels; }
		std::string getGPUIDsStr() const { return gpuIDsStr; }
		const IntVec& getGPUIDsVec() const { return gpuIDsVec; }
		bool getAssignGPUPerService() const { return assignGPUPerService; }
		bool getUseCuFile() const { return useCuFile; }
		bool getUseGPUBufReg() const { return useGPUBufReg; }
		CuFileHandleDataVec& getCuFileHandleDataVec() { return cuFileHandleDataVec; }
		bool getUseCuFileDriverOpen() const { return useCuFileDriverOpen; }
		bool getUseCuHostBufReg() const { return useCuHostBufReg; }
		uint64_t getIntegrityCheckSalt() const { return integrityCheckSalt; }
		bool getRunSyncPhase() const { return runSyncPhase; }
		bool getRunDropCachesPhase() const { return runDropCachesPhase; }
		bool getRunStatFilesPhase() const { return runStatFilesPhase; }
};


#endif /* PROGARGS_H_ */
