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

/* command line args and config file options (sorted alphabetically by "ARG_..." column).
    note: keep length of "_LONG" argument names without parameters within a max length of 16 chars
        and "_LONG" arguments with parameters to a max of 12 chars, as the description column
        in the help output otherwise gets too small. */
#define ARG_ALTHTTPSERVER_LONG		"althttpsvc"
#define ARG_BENCHLABEL_LONG			"label"
#define ARG_BENCHPATHS_LONG			"path"
#define ARG_BLOCK_LONG	 			"block"
#define ARG_BLOCK_SHORT 			"b"
#define ARG_BLOCKVARIANCE_LONG		"blockvarpct"
#define ARG_BLOCKVARIANCEALGO_LONG	"blockvaralgo"
#define ARG_BRIEFLIVESTATS_LONG		"live1"
#define ARG_CLIENTS_LONG			"clients"
#define ARG_CLIENTSFILE_LONG		"clientsfile"
#define ARG_CONFIGFILE_LONG			"configfile"
#define ARG_CONFIGFILE_SHORT  		"c"
#define ARG_CPUCORES_LONG			"cores"
#define ARG_CPUUTIL_LONG			"cpu"
#define ARG_CREATEDIRS_LONG			"mkdirs"
#define ARG_CREATEDIRS_SHORT		"d"
#define ARG_CREATEFILES_LONG		"write"
#define ARG_CREATEFILES_SHORT		"w"
#define ARG_CSVFILE_LONG            "csvfile"
#define ARG_CSVLIVEFILE_LONG        "livecsv"
#define ARG_CSVLIVEEXTENDED_LONG    "livecsvex"
#define ARG_CUFILE_LONG				"cufile"
#define ARG_CUFILEDRIVEROPEN_LONG	"cufiledriveropen"
#define ARG_CUHOSTBUFREG_LONG		"cuhostbufreg"
#define ARG_DELETEDIRS_LONG			"deldirs"
#define ARG_DELETEDIRS_SHORT		"D"
#define ARG_DELETEFILES_LONG		"delfiles"
#define ARG_DELETEFILES_SHORT		"F"
#define ARG_DIRECTIO_LONG 			"direct"
#define ARG_DIRSHARING_LONG			"dirsharing"
#define ARG_DIRSTATS_LONG			"dirstats"
#define ARG_DROPCACHESPHASE_LONG	"dropcache"
#define ARG_DRYRUN_LONG				"dryrun"
#define ARG_FADVISE_LONG			"fadv"
#define ARG_FILESHARESIZE_LONG		"sharesize"
#define ARG_FILESIZE_LONG	 		"size"
#define ARG_FILESIZE_SHORT 			"s"
#define ARG_FLOCK_LONG              "flock"
#define ARG_FOREGROUNDSERVICE_LONG	"foreground"
#define ARG_GDSBUFREG_LONG			"gdsbufreg"
#define ARG_GPUDIRECTSSTORAGE_LONG	"gds"
#define ARG_GPUIDS_LONG				"gpuids"
#define ARG_GPUPERSERVICE_LONG		"gpuperservice"
#define ARG_HDFS_LONG				"hdfs"
#define ARG_HELP_LONG 				"help"
#define ARG_HELP_SHORT 				"h"
#define ARG_HELPALLOPTIONS_LONG	 	"help-all"
#define ARG_HELPBLOCKDEV_LONG 		"help-bdev" // for backwards compat (new: help-large)
#define ARG_HELPDISTRIBUTED_LONG 	"help-dist"
#define ARG_HELPLARGE_LONG 			"help-large"
#define ARG_HELPMULTIFILE_LONG 		"help-multi"
#define ARG_HELPS3_LONG 			"help-s3"
#define ARG_HOSTS_LONG				"hosts"
#define ARG_HOSTSFILE_LONG			"hostsfile"
#define ARG_IGNORE0USECERR_LONG		"no0usecerr"
#define ARG_IGNOREDELERR_LONG		"nodelerr"
#define ARG_INFINITEIOLOOP_LONG		"infloop"
#define ARG_INTEGRITYCHECK_LONG		"verify"
#define ARG_INTERRUPT_LONG			"interrupt"
#define ARG_IODEPTH_LONG			"iodepth"
#define ARG_ITERATIONS_LONG			"iterations"
#define ARG_ITERATIONS_SHORT		"i"
#define ARG_JSONFILE_LONG           "jsonfile"
#define ARG_JSONLIVEEXTENDED_LONG   "livejsonex"
#define ARG_JSONLIVEFILE_LONG       "livejson"
#define ARG_LATENCY_LONG			"lat"
#define ARG_LATENCYHISTOGRAM_LONG	"lathisto"
#define ARG_LATENCYPERCENT9S_LONG	"latpercent9s"
#define ARG_LATENCYPERCENTILES_LONG	"latpercent"
#define ARG_LIMITREAD_LONG			"limitread"
#define ARG_LIMITWRITE_LONG			"limitwrite"
#define ARG_LIVEINTERVAL_LONG		"liveint"
#define ARG_LIVESTATSNEWLINE_LONG	"live1n"
#define ARG_LOGLEVEL_LONG			"log"
#define ARG_MADVISE_LONG			"madv"
#define ARG_MMAP_LONG				"mmap"
#define ARG_NETBENCH_LONG			"netbench"
#define ARG_NETBENCHSERVERSSTR_LONG "netbenchservers" // internal (not set by user)
#define ARG_NETDEVS_LONG			"netdevs"
#define ARG_NOCSVLABELS_LONG		"nocsvlabels"
#define ARG_NODETACH_LONG			"nodetach"
#define ARG_NODIRECTIOCHECK_LONG	"nodiocheck"
#define ARG_NOFDSHARING_LONG		"nofdsharing"
#define ARG_NOLIVESTATS_LONG 		"nolive"
#define ARG_NOPATHEXPANSION_LONG	"nopathexp"
#define ARG_NORANDOMALIGN_LONG      "norandalign"
#define ARG_NOSVCPATHSHARE_LONG		"nosvcshare"
#define ARG_NUMAZONES_LONG			"zones"
#define ARG_NUMDATASETTHREADS_LONG	"datasetthreads" // internal (not set by user)
#define ARG_NUMDIRS_LONG 			"dirs"
#define ARG_NUMDIRS_SHORT 			"n"
#define ARG_NUMFILES_LONG	 		"files"
#define ARG_NUMFILES_SHORT 			"N"
#define ARG_NUMHOSTS_LONG			"numhosts"
#define ARG_NUMNETBENCHSERVERS_LONG	"numservers"
#define ARG_NUMTHREADS_LONG			"threads"
#define ARG_NUMTHREADS_SHORT 		"t"
#define ARG_OPSLOGLOCKING_LONG		"opsloglock"
#define ARG_OPSLOGPATH_LONG		 	"opslog"
#define ARG_PHASEDELAYTIME_LONG		"phasedelay"
#define ARG_PREALLOCFILE_LONG		"preallocfile"
#define ARG_QUIT_LONG				"quit"
#define ARG_RANDOMAMOUNT_LONG		"randamount"
#define ARG_RANDOMOFFSETS_LONG		"rand"
#define ARG_RANDSEEKALGO_LONG		"randalgo"
#define ARG_RANKOFFSET_LONG			"rankoffset"
#define ARG_READ_LONG				"read"
#define ARG_READ_SHORT				"r"
#define ARG_READINLINE_LONG			"readinline"
#define ARG_RECVBUFSIZE_LONG		"recvbuf"
#define ARG_RESPSIZE_LONG			"respsize"
#define ARG_RESULTSFILE_LONG		"resfile"
#define ARG_REVERSESEQOFFSETS_LONG	"backward"
#define ARG_ROTATEHOSTS_LONG		"rotatehosts"
#define ARG_RUNASSERVICE_LONG		"service"
#define ARG_RWMIXPERCENT_LONG		"rwmixpct"
#define ARG_RWMIXTHREADS_LONG		"rwmixthr"
#define ARG_RWMIXTHREADSPCT_LONG    "rwmixthrpct"
#define ARG_S3ACCESSKEY_LONG		"s3key"
#define ARG_S3ACCESSSECRET_LONG		"s3secret"
#define ARG_S3ACLGET_LONG			"s3aclget"
#define ARG_S3ACLGRANTEE_LONG		"s3aclgrantee"
#define ARG_S3ACLGRANTEETYPE_LONG	"s3aclgtype"
#define ARG_S3ACLGRANTS_LONG		"s3aclgrants"
#define ARG_S3ACLPUT_LONG			"s3aclput"
#define ARG_S3ACLPUTINLINE_LONG     "s3aclputinl"
#define ARG_S3ACLVERIFY_LONG		"s3aclverify"
#define ARG_S3BUCKETACLGET_LONG		"s3baclget"
#define ARG_S3BUCKETACLPUT_LONG		"s3baclput"
#define ARG_S3BUCKETTAG_LONG        "s3btag"
#define ARG_S3BUCKETTAGVERIFY_LONG  "s3btagverify"
#define ARG_S3BUCKETVER_LONG        "s3bversion"
#define ARG_S3BUCKETVERVERIFY_LONG  "s3bversionverify"
#define ARG_S3CREDFILE_LONG         "s3credfile"
#define ARG_S3CREDLIST_LONG         "s3credlist"
#define ARG_S3ENDPOINTS_LONG		"s3endpoints"
#define ARG_S3FASTGET_LONG			"s3fastget"
#define ARG_S3FASTPUT_LONG          "s3fastput"
#define ARG_S3IGNOREERRORS_LONG		"s3ignoreerrors"
#define ARG_S3LISTOBJ_LONG			"s3listobj"
#define ARG_S3LISTOBJPARALLEL_LONG	"s3listobjpar"
#define ARG_S3LISTOBJVERIFY_LONG	"s3listverify"
#define ARG_S3LOGFILEPREFIX_LONG	"s3logprefix"
#define ARG_S3LOGLEVEL_LONG			"s3log"
#define ARG_S3MULTIDELETE_LONG		"s3multidel"
#define ARG_S3MULTI_IGNORE_404      "s3multiignore404"
#define ARG_S3NOCOMPRESS_LONG       "s3nocompress"
#define ARG_S3NOMPCHECK_LONG		"s3nompcheck"
#define ARG_S3NOMPUCOMPLETION_LONG  "s3nompucompl"
#define ARG_S3OBJECTPREFIX_LONG		"s3objprefix"
#define ARG_S3OBJLOCKCFG_LONG       "s3olockcfg"
#define ARG_S3OBJLOCKCFGVERIFY_LONG "s3olockcfgverify"
#define ARG_S3OBJTAG_LONG           "s3otag"
#define ARG_S3OBJTAGVERIFY_LONG     "s3otagverify"
#define ARG_S3RANDOBJ_LONG			"s3randobj"
#define ARG_S3REGION_LONG			"s3region"
#define ARG_S3SESSION_TOKEN_LONG	"s3sessiontoken"
#define ARG_S3SIGNPAYLOAD_LONG		"s3sign"
#define ARG_S3SSE_LONG              "s3sse"
#define ARG_S3SSECKEY_LONG          "s3sseckey"
#define ARG_S3CHECKSUM_ALGO_2_LONG  "s3checksumalgo"  // compat alias (too long name)
#define ARG_S3CHECKSUM_ALGO_LONG    "s3chksumalgo"  // parameter for x-amz-sdk-checksum-algorithm
#define ARG_S3SSEKMSKEY_LONG        "s3ssekmskey"
#define ARG_S3STATDIRS_LONG         "s3statdirs"
#define ARG_SENDBUFSIZE_LONG		"sendbuf"
#define ARG_SERVERS_LONG			"servers"
#define ARG_SERVERSFILE_LONG		"serversfile"
#define ARG_SERVICEPORT_LONG		"port"
#define ARG_SHOWALLELAPSED_LONG		"allelapsed"
#define ARG_SHOWSVCELAPSED_LONG		"svcelapsed"
#define ARG_STARTTIME_LONG			"start"
#define ARG_STATFILES_LONG			"stat"
#define ARG_STATFILESINLINE_LONG	"statinline"
#define ARG_STRIDEDACCESS_LONG      "strided"
#define ARG_SVCPASSWORDFILE_LONG    "svcpwfile"
#define ARG_SVCUPDATEINTERVAL_LONG	"svcupint"
#define ARG_SVCREADYWAITSECS_LONG   "svcwait"
#define ARG_SYNCPHASE_LONG			"sync"
#define ARG_TIMELIMITSECS_LONG		"timelimit"
#define ARG_TREEFILE_LONG			"treefile"
#define ARG_TREERANDOMIZE_LONG		"treerand"
#define ARG_TREEROUNDUP_LONG		"treeroundup"
#define ARG_TREESCAN_LONG           "treescan"
#define ARG_TRUNCATE_LONG			"trunc"
#define ARG_TRUNCTOSIZE_LONG		"trunctosize"
#define ARG_VERIFYDIRECT_LONG		"verifydirect"
#define ARG_VERSION_LONG			"version"


#define ARGDEFAULT_SERVICEPORT		1611
#define ARGDEFAULT_SERVICEPORT_STR	STRINGIZE(ARGDEFAULT_SERVICEPORT)


#define SERVICE_UPLOAD_BASEPATH(servicePort)	("/var/tmp/" EXE_NAME "_" + \
												SystemTk::getUsername() + "_" + \
												"p" + std::to_string(servicePort) )
#define SERVICE_UPLOAD_TREEFILE					"treefile.txt"
#define S3_IMPLICIT_TREEFILE_PATH				("/var/tmp/" EXE_NAME "_" + \
												SystemTk::getUsername() + "_" + \
												"treefile_implicit.txt")


// flags for fadvise
#define FADVISELIST_DELIMITERS				", \n\r" // delimiters for fadvise args string

#define ARG_FADVISE_FLAG_SEQ				1
#define ARG_FADVISE_FLAG_SEQ_NAME			"seq"
#define ARG_FADVISE_FLAG_RAND				2
#define ARG_FADVISE_FLAG_RAND_NAME			"rand"
#define ARG_FADVISE_FLAG_WILLNEED			4
#define ARG_FADVISE_FLAG_WILLNEED_NAME		"willneed"
#define ARG_FADVISE_FLAG_DONTNEED			8
#define ARG_FADVISE_FLAG_DONTNEED_NAME		"dontneed"
#define ARG_FADVISE_FLAG_NOREUSE			16
#define ARG_FADVISE_FLAG_NOREUSE_NAME		"noreuse"

// flags for madvise
#define MADVISELIST_DELIMITERS				", \n\r" // delimiters for madvise args string

#define ARG_MADVISE_FLAG_SEQ				1
#define ARG_MADVISE_FLAG_SEQ_NAME			"seq"
#define ARG_MADVISE_FLAG_RAND				2
#define ARG_MADVISE_FLAG_RAND_NAME			"rand"
#define ARG_MADVISE_FLAG_WILLNEED			4
#define ARG_MADVISE_FLAG_WILLNEED_NAME		"willneed"
#define ARG_MADVISE_FLAG_DONTNEED			8
#define ARG_MADVISE_FLAG_DONTNEED_NAME		"dontneed"
#define ARG_MADVISE_FLAG_HUGEPAGE			16
#define ARG_MADVISE_FLAG_HUGEPAGE_NAME		"hugepage"
#define ARG_MADVISE_FLAG_NOHUGEPAGE			32
#define ARG_MADVISE_FLAG_NOHUGEPAGE_NAME	"nohugepage"

// values for file locking
#define ARG_FLOCK_NONE                      0
#define ARG_FLOCK_NONE_NAME                 ""
#define ARG_FLOCK_RANGE                     1
#define ARG_FLOCK_RANGE_NAME                "range" // lock only the specific range of each IO
#define ARG_FLOCK_FULL                      2
#define ARG_FLOCK_FULL_NAME                 "full" // lock entire file instead of only a range

/* permission flags for S3 ACLs.
	note: std::string::find() will be used with these, so make sure each name is unambiguous and not
	a substring of another name. */
#define ARG_S3ACL_PERM_NONE_NAME			"none"
#define ARG_S3ACL_PERM_FULL_NAME			"full"
#define ARG_S3ACL_PERM_FLAG_READ_NAME		"read"
#define ARG_S3ACL_PERM_FLAG_WRITE_NAME		"write"
#define ARG_S3ACL_PERM_FLAG_READACP_NAME	"racp"
#define ARG_S3ACL_PERM_FLAG_WRITEACP_NAME	"wacp"

// grantee type for S3 ACLs
#define ARG_S3ACL_GRANTEE_TYPE_ID			"id"
#define ARG_S3ACL_GRANTEE_TYPE_EMAIL		"email"
#define ARG_S3ACL_GRANTEE_TYPE_URI			"uri"
#define ARG_S3ACL_GRANTEE_TYPE_GROUP		"group"


#define RAND_PREFIX_MARK_CHAR				'%' // name prefix char to replace with random value
#define RAND_PREFIX_MARKS_SUBSTR			"%%%" // three times RAND_PREFIX_MARK_CHAR

#define ARG_LIVECSV_STDOUT                  "stdout" // special filename value to send to stdout


typedef std::vector<CuFileHandleData> CuFileHandleDataVec;
typedef std::vector<CuFileHandleData*> CuFileHandleDataPtrVec;

struct NetBenchServerAddr
{
	std::string host;
	unsigned short port;
};

typedef std::vector<NetBenchServerAddr> NetBenchServerAddrVec;


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
		void rotateHosts();
		void getBenchPathInfoTree(bpt::ptree& outTree);
		void checkServiceBenchPathInfos(BenchPathInfoVec& benchPathInfos);


	private:
		bpo::options_description argsGenericDescription;
		bpo::options_description argsHiddenDescription;
		bpo::variables_map argsVariablesMap;

		int argc; // command line argument count (as in main(argc, argv) )
		char** argv; // command line arg vector (as in main(argc, argv) )

		CustomTree customTree; // file and dir paths for custom tree mode

		std::string progPath; // absolute path to program binary
		std::string benchPathStr; // benchmark path(s), separated by BENCHPATH_DELIMITER
		StringVec benchPathsVec; // benchPathStr split into individual paths
		StringVec benchPathsServiceOverrideVec; // set in service mode to override bench paths
		IntVec benchPathFDsVec; // file descriptors to individual bench paths
		BenchPathType benchPathType; /* for local runs auto-detected based on benchPathStr;
										in master mode received from service hosts */

		bool isCuFileDriverOpen{false}; // to ensure cuFileDriverOpen/-Close is only called once
		CuFileHandleDataVec cuFileHandleDataVec; /* registered cuFile handles in file/bdev mode;
							vec will also be filled (with unreg'ed handles) if cuFile API is not
							selected to make things easier for localworkers */

		int stdoutDupFD; // dup of stdout file descriptor if overridden e.g. due to csv to stdout

        // config options in alphabetic order...

		bool assignGPUPerService; // assign GPUs from gpuIDsVec round robin per service
		std::string benchLabel; // user-defined label for benchmark run
		std::string benchLabelNoCommas; // implict based on benchLabel with commas removed for csv
		size_t blockSize; // number of bytes to read/write in a single read()/write() call
		std::string blockSizeOrigStr; // original blockSize str from user with unit
		unsigned blockVariancePercent; // % of blocks that should differ between writes
		std::string blockVarianceAlgo; // rand algo for buffer fill variance
		std::string clientsFilePath; // path to file for appended service hosts
		std::string clientsStr; // appended to hostsStr in netbench mode
		std::string configFilePath; // Configuration input using a config file (empty for none)
		std::string cpuCoresStr; // comma-separated cpu cores that this process may run on
		IntVec cpuCoresVec; // list from cpuCoresStr broken down into individual elements
		std::string csvFilePath; // phase results file path for csv format (or empty for none)
		bool disableLiveStats; // disable live stats
		bool disablePathBracketsExpansion; // true to disable square brackets expansion for paths
		bool doDirectVerify; // verify data integrity by reading immediately after write
		bool doDirSharing; // workers use same dirs in dir mode (instead of unique dir per worker)
		bool doInfiniteIOLoop; // let each thread loop on its phase work infinitely
		bool doPreallocFile; // prealloc file space on creation via posix_fallocate()
		bool doReadInline; // true to read immediately after creation while file still open
		bool doReverseSeqOffsets; // backwards sequential read/write
        bool doS3AclPutInline; // set object acl during PutObject
		bool doS3AclVerify; // verify that acl contains given grantee and permissions
		bool doS3ListObjVerify; // verify object listing (requires "-n" / "-N")
		bool doStatInline; // true to stat immediately after creation while file still open
        bool doS3BucketVersioning;  // allow to toggle bucket versioning
        bool doS3BucketVersioningVerify;  // verify that the correct versioning status was set
		bool doS3BucketTag; // add bucket tagging ops during different bucket operations
		bool doS3BucketTagVerify; // do bucket tagging verification.
        bool doS3ObjectTag; // add object tagging ops during different object operations
        bool doS3ObjectTagVerify; // do bucket tagging verification.
        bool doS3ObjectLockCfg; // do S3 object lock configuration
        bool doS3ObjectLockCfgVerify; // do S3 object lock configuration verification
		bool doTruncate; // truncate files to 0 size on open for writing
		bool doTruncToSize; // truncate files to size on creation via ftruncate()
		unsigned fadviseFlags; // flags for fadvise() (ARG_FADVISE_FLAG_x)
		std::string fadviseFlagsOrigStr; // flags for fadvise() (ARG_FADVISE_FLAG_x_NAME)
		uint64_t fileShareSize; /* in custom tree mode, file size as of which to write/read shared.
									(default 0 means 32 times blockSize) */
		std::string fileShareSizeOrigStr; // original fileShareSize str from user with unit
		uint64_t fileSize; // size per file
		std::string fileSizeOrigStr; // original fileSize str from user with unit
        unsigned short flockType; // internal type of file lock based on user string (ARG_FLOCK_x)
        std::string flockTypeOrigStr; // type of file lock on command line (ARG_FLOCK_x_NAME)
		std::string gpuIDsServiceOverride; // set in service mode to override gpu IDs
		std::string gpuIDsStr; // list of gpu IDs, separated by GPULIST_DELIMITERS
		IntVec gpuIDsVec; // gpuIDsStr broken down into individual GPU IDs
		std::string hostsFilePath; // path to file for service hosts
		StringVec hostsVec; // service hosts broken down into individual hostname[:port]
		std::string hostsStr; // list of service hosts, element format is hostname[:port]
		bool ignore0USecErrors; // ignore worker completion in less than 1 millisecond
		bool ignoreDelErrors; // ignore ENOENT errors on file/dir deletion
		bool ignoreS3Errors; // ignore S3 get/put errors, useful for stress-testing
		bool ignoreS3PartNum; // don't check for >10K parts in multi-part uploads
		size_t ioDepth; // depth of io queue per thread for libaio
		uint64_t integrityCheckSalt; // salt to add to data integrity checksum (0 disables check)
		bool interruptServices; // send interrupt msg to given hosts to stop current phase
		size_t iterations; // Number of iterations of the same benchmark
		std::string jsonFilePath; // phase results file path for json format (or empty for none)
		unsigned short logLevel; // filter level for log messages (higher will not be logged)
		uint64_t limitReadBps; // read limit per thread in bytes per sec
		std::string limitReadBpsOrigStr; // original limitReadBps str from user with unit
		uint64_t limitWriteBps; // write limit per thread in bytes per sec
		std::string limitWriteBpsOrigStr; // original limitWriteBps str from user with unit
		std::string liveCSVFilePath; // live stats file path for csv format (or empty for none)
		std::string liveJSONFilePath; // live stats file path for json format (or empty for none)
		size_t liveStatsSleepMS; // interval between live stats console/csv updates
		unsigned madviseFlags; // flags for madvise() (ARG_MADVISE_FLAG_x)
		std::string madviseFlagsOrigStr; // flags for madvise() (ARG_MADVISE_FLAG_x_NAME)
		BufferVec mmapVec; /* pointers to mmap regions if user selected mmap IO; number of
			entries and their order matches fdVec. only used in file/bdev random mode. */
		std::string netBenchRespSizeOrigStr; // original netBenchRespSize str from user with unit
		size_t netBenchRespSize; // server response length for each received client block
		std::string netBenchServersStr; // implictly inited from numNetBenchServers and hosts list
		NetBenchServerAddrVec netBenchServersVec; // set for services based on netBenchServersStr
		std::string netDevsStr; // user-given network devices for round-robin conn binding
		StringVec netDevsVec; // netDevsStr broken down into individual elements
		unsigned nextPhaseDelaySecs; // delay between bench phases in seconds
		bool noCSVLabels; // true to not print headline with labels to csv file
		bool noDirectIOCheck; // ignore directIO alignment and block size sanity checks
		bool noSharedServicePath; // true if bench paths not shared between service instances
		std::string numaZonesStr; // comma-separated numa zones that this process may run on
		IntVec numaZonesVec; // list from numaZoneStr broken down into individual elements
		int numHosts; // number of hosts to use from hostsStr/hostsFilePath ("-1" means "all")
		size_t numDataSetThreads; /* global threads working on same dataset for service mode hosts
									based on isBenchPathShared; otherwise equal to numThreads */
		size_t numDirs; // directories per thread
		std::string numDirsOrigStr; // original numDirs str from user with unit
		size_t numFiles; // files per directory
		std::string numFilesOrigStr; // original numDirs str from user with unit
		unsigned short numLatencyPercentile9s; // decimal 9s to show (0=99%, 1=99.9%, 2=99.99%, ...)
		unsigned numNetBenchServers; // number of servers in service hosts list for netbench mode
		size_t numRWMixReadThreads; // number of rwmix read threads in file/bdev write phase
		size_t numThreads; // parallel I/O worker threads per instance
		std::string opsLogPath; // path to operations log file (empty to disable)
		bool quitServices; // send quit (via interrupt msg) to given hosts to exit service
		uint64_t randomAmount; // random bytes to read/write per file (when randomOffsets is used)
		std::string randomAmountOrigStr; // original randomAmount str from user with unit
		std::string randOffsetAlgo; // rand algo for random offsets
		size_t rankOffset; // offset for worker rank numbers
		std::string resFilePath; // results output file path (or empty for no results file)
		unsigned short rotateHostsNum; // number by which to rotate hosts between phases
		bool runAsService; // run as service for remote coordination by master
		bool runCreateDirsPhase; // create directories
		bool runCreateFilesPhase; // create files
		bool runDeleteDirsPhase; // delete dirs
		bool runDeleteFilesPhase; // delete files
		bool runDropCachesPhase; // run "echo 3>drop_caches" phase to drop kernel page cache
		bool runReadPhase; // read files
		bool runS3AclGet; // retrieve object acl
		bool runS3AclPut; // change object acl
		bool runS3BucketAclGet; // retrieve bucket acl
		bool runS3BucketAclPut; // change bucket acl
        bool runS3StatDirs; // HeadBucket (and other bucket MD ops, goes well with doS3BucketTag)
		uint64_t runS3ListObjNum; // run seq list objects phase if >0, given number is listing limit
		bool runS3ListObjParallel; // multi-threaded object listing (requires "-n" / "-N")
		uint64_t runS3MultiDelObjNum; // run S3 multi del phase if >0; number is multi del limit
		bool runServiceInForeground; // true to not daemonize service process into background
		bool runStatFilesPhase; // stat files
		bool runSyncPhase; // run the sync() phase to commit all dirty page cache buffers
		unsigned rwMixReadPercent; // % of blocks that should be read (the rest will be written)
		unsigned rwMixThreadsReadPercent; // % of blocks to be read (the rest will be written)
		std::string s3AccessKey; // s3 access key
		std::string s3AccessSecret; // s3 access secret
		std::string s3AclGrantee; // s3 acl grantee
		std::string s3AclGranteeType; // s3 acl grantee type
		std::string s3AclGranteePermissions; // s3 acl grantee permission flags (ARG_S3_ACL_...)
		std::string s3CredentialsFile; // path to file containing multiple S3 credentials
        std::string s3CredentialsList; // comma-separated list of S3 credentials
		std::string s3EndpointsServiceOverrideStr; // override of s3EndpointStr in service mode
		StringVec s3EndpointsVec; // s3 endpoints broken down into individual elements
		std::string s3EndpointsStr; // user-given s3 endpoints; elem format: [http(s)://]host[:port]
        bool s3IgnoreMultipartUpload404; // Ignore 404 on retries of MPU completion
		std::string s3LogfilePrefix; // dir and name prefix of aws sdk log file
		unsigned short s3LogLevel; // log level for AWS SDK
		bool s3NoCompression; // disable request compression of aws sdk cpp
        bool s3NoMpuCompletion; // don't send finalizing multi-part upload completion message
		std::string s3ObjectPrefix; // object name/path prefix for s3 "directory mode"
		std::string s3Region; // s3 region
        std::string s3SessionToken; // s3 session token (same as secret token)
		unsigned short s3SignPolicy; /* Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy; note:
			"2=never" is ignored, because as of aws sdk cpp v1.11.486 signing is always done. */
        std::string s3SSECKey;  // S3 SSE-C key for encryption
        std::string s3SSEKMSKey;  // S3 SSE-KMS key for encryption
		unsigned short servicePort; // HTTP/TCP port for service
		std::string serversFilePath; // path to file for preprended service hosts
		std::string serversStr; // prepended to hostsStr in netbench mode
		bool showAllElapsed; // print elapsed time of each I/O worker thread
		bool showCPUUtilization; // show cpu utilization in phase stats results
		bool showDirStats; // show processed dirs stats in file write/read phase of dir mode
		bool showLatency; // show min/avg/max latency
		bool showLatencyHistogram; // show latency histogram
		bool showLatencyPercentiles; // show latency percentiles
		bool showServicesElapsed; // print elapsed time of each service by slowest thread
        int sockRecvBufSize; // custom netbench socket recv buf size (0 means no change)
        int sockSendBufSize; // custom netbench socket send buf size (0 means no change)
        std::string sockRecvBufSizeOrigStr; // original sockRecvBufSize str from user with unit
        std::string sockSendBufSizeOrigStr; // original sockSendBufSize str from user with unit
        time_t startTime; /* UTC start time to coordinate multiple benchmarks, in seconds since the
                            epoch. 0 means immediate start. */
        std::string svcPasswordFile; // protect against unauthorized service commands
        std::string svcPasswordHash; // implicitly set if svcPasswordFile is given, empty otherwise
        size_t svcReadyWaitSec; // startup wait time for services to be ready in seconds
        size_t svcUpdateIntervalMS; // update retrieval interval for service hosts in milliseconds
		std::string treeFilePath; // path to file containing custom tree (list of dirs and files)
		uint64_t treeRoundUpSize; /* in treefile, round up file sizes to multiple of given size.
			(useful for directIO with its alignment reqs on some file systems. 0 disables this.) */
		std::string treeRoundUpSizeOrigStr; // original treeRoundUpSize str from user with unit
        std::string treeScanPath; // path to dir/bucket to scan as custom tree
		size_t timeLimitSecs; // time limit in seconds for each phase (0 to disable)
		bool useAlternativeHTTPService; // use alternative http service implememtation
		bool useBriefLiveStats; // single-line live stats
		bool useBriefLiveStatsNewLine; /* newline instead of line erase on update. implicitly sets
											useBriefLiveStats=true */
		bool useCuFile; // use cuFile API for reads/writes to/from GPU memory
		bool useCuFileDriverOpen; // true to call cuFileDriverOpen when using cuFile API
		bool useCuHostBufReg; // register/pin host buffer to speed up copy into GPU memory
		bool useCustomTreeRandomize; // randomize order of custom tree files
		bool useDirectIO; // open files with O_DIRECT
		bool useExtendedLiveCSV; // false for total/aggregate results only, true for per-worker
		bool useExtendedLiveJSON; // false for total/aggregate results only, true for per-worker
		bool useGDSBufReg; // register GPU buffers for GPUDirect Storage (GDS) when using cuFile API
		bool useHDFS; // use Hadoop HDFS
		bool useMmap; // use memory mapped IO
		bool useNetBench; // run network benchmarking
		bool useNoFDSharing; // when true, each worker does its own file open in file/bdev mode
		bool useOpsLogLocking; // use file locking to sync opsLogPath writes
		bool useRandomUnaligned; // don't use block-aligned offsets for random IO
		bool useRandomOffsets; // use random offsets for file reads/writes
		bool useRWMixPercent; // implicitly set in case of rwmixpct (even if ==0)
		bool useRWMixReadThreads; // implicitly set in case of rwmixthr (even if ==0)
		bool useS3ObjectPrefixRand; // implicit based on RAND_PREFIX_MARKS_SUBSTR in s3ObjectPrefix
		bool useS3RandObjSelect; // random object selection for each read
		bool useS3FastRead; /* get objects to /dev/null instead of buffer (i.e. no post processing
								via buffer possible, such as GPU copy or data verification) */
        bool useS3SSE; // use SSE-S3 encryption method for S3
        bool useStridedAccess; // use strided file access pattern for shared files
		std::string s3ChecksumAlgoStr;  // Stores the S3 checksum algorithm value (e.g. "CRC32", "CRC32C", "SHA1", "SHA256")


		void defineDefaults();
		void initImplicitValues();
		void convertUnitStrings();
		void checkArgs();
		void checkPathDependentArgs();
		void parseAndCheckPaths();
		void convertS3PathsToCustomTree();
		void prepareBenchPathFDsVec();
		void prepareCuFileHandleDataVec();
		void prepareMmapVec();
		void prepareFileSize(int fd, std::string& path);
		void parseHosts();
		void parseNetBenchServersForService();
		void parseNumaZones();
		void parseCPUCores();
		void parseGPUIDs();
		void parseRandAlgos();
		void parseS3Endpoints();
		void parseNetDevs();
		void scanCustomTree();
		void loadCustomTreeFile();
		void loadServicePasswordFile();
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

        bool getS3BucketMetadataRequested() const {
            return doS3BucketTag || doS3ObjectLockCfg || doS3BucketVersioning;
        }
        bool getS3ObjectMetadataRequested() const { return doS3ObjectTag; }
        bool getRunS3GetObjectMetadata() const { return getS3ObjectMetadataRequested(); }
        bool getRunS3PutObjectMetadata() const
            { return getS3ObjectMetadataRequested() && runCreateFilesPhase; }
        bool getRunS3DelObjectMetadata() const
            { return getS3ObjectMetadataRequested() && runDeleteFilesPhase; }
        bool getRunS3GetBucketMetadata() const { return getS3BucketMetadataRequested(); }
        bool getRunS3PutBucketMetadata() const
            { return getS3BucketMetadataRequested() && runCreateDirsPhase; }
        bool getRunS3DelBucketMetadata() const
            { return getS3BucketMetadataRequested() && runDeleteDirsPhase; }

		// getters for config options in alphabetic order...

		bool getAssignGPUPerService() const { return assignGPUPerService; }
        unsigned getBlockVariancePercent() const { return blockVariancePercent; }
        std::string getBlockVarianceAlgo() const { return blockVarianceAlgo; }
        size_t getBlockSize() const { return blockSize; }
        std::string getBenchLabel() const { return benchLabel; }
        const std::string& getBenchLabelNoCommas() const { return benchLabelNoCommas; }
        CuFileHandleDataVec& getCuFileHandleDataVec() { return cuFileHandleDataVec; }
        std::string getConfigFilePath() const { return configFilePath; }
        const IntVec& getCPUCoresVec() const { return cpuCoresVec; }
        std::string getCPUCoresStr() const { return cpuCoresStr; }
		const PathStore& getCustomTreeDirs() const { return customTree.dirs; }
		const PathStore& getCustomTreeFilesNonShared() const { return customTree.filesNonShared; }
		const PathStore& getCustomTreeFilesShared() const { return customTree.filesShared; }
        std::string getCSVFilePath() const { return csvFilePath; }
        bool getDisableLiveStats() const { return disableLiveStats; }
        bool getDoDirSharing() const { return doDirSharing; }
        bool getDoDirectVerify() const { return doDirectVerify; }
        bool getDoInfiniteIOLoop() const { return doInfiniteIOLoop; }
        bool getDoPreallocFile() const { return doPreallocFile; }
        bool getDoReadInline() const { return doReadInline; }
        bool getDoReverseSeqOffsets() const { return doReverseSeqOffsets; }
        bool getDoStatInline() const { return doStatInline; }
        bool getDoS3BucketVersioning() const { return doS3BucketVersioning; }
        bool getDoS3BucketVersioningVerify() const { return doS3BucketVersioningVerify; }
        bool getDoS3BucketTagging() const { return doS3BucketTag; }
        bool getDoS3BucketTaggingVerify() const { return doS3BucketTagVerify; }
        bool getDoS3ObjectTagging() const { return doS3ObjectTag; }
        bool getDoS3ObjectTaggingVerify() const { return doS3ObjectTagVerify; }
        bool getDoS3ObjectLockConfiguration() const { return doS3ObjectLockCfg; }
        bool getDoS3ObjectLockConfigurationVerify() const { return doS3ObjectLockCfgVerify; }
        bool getDoS3AclPutInline() const { return doS3AclPutInline; }
        bool getDoS3AclVerify() const { return doS3AclVerify; }
        bool getDoTruncate() const { return doTruncate; }
        bool getDoTruncToSize() const { return doTruncToSize; }
        bool getDoListObjVerify() const { return doS3ListObjVerify; }
        unsigned getFadviseFlags() const { return fadviseFlags; }
        std::string getFadviseFlagsOrigStr() const { return fadviseFlagsOrigStr; }
        uint64_t getFileShareSize() const { return fileShareSize; }
        uint64_t getFileSize() const { return fileSize; }
        std::string getFileSizeOrigStr() const { return fileSizeOrigStr; }
        unsigned short getFLockType() const { return flockType; }
        std::string getFLockTypeOrigStr() const { return flockTypeOrigStr; }
		std::string getGPUIDsStr() const { return gpuIDsStr; }
		const IntVec& getGPUIDsVec() const { return gpuIDsVec; }
		std::string getGPUIDsServiceOverride() const { return gpuIDsServiceOverride; }
		std::string getHostsStr() const { return hostsStr; }
		std::string getHostsFilePath() const { return hostsFilePath; }
		const StringVec& getHostsVec() const { return hostsVec; }
        bool getIgnore0USecErrors() const { return ignore0USecErrors; }
        bool getIgnoreDelErrors() const { return ignoreDelErrors; }
        bool getIgnoreS3Errors() const { return ignoreS3Errors; }
        bool getIgnoreS3PartNum() const { return ignoreS3PartNum; }
        uint64_t getIntegrityCheckSalt() const { return integrityCheckSalt; }
        size_t getIODepth() const { return ioDepth; }
        bool getInterruptServices() const { return interruptServices; }
		bool getIsServicePathShared() const { return !noSharedServicePath; }
        size_t getIterations() const { return iterations; }
        std::string getJSONFilePath() const { return jsonFilePath; }
        uint64_t getLimitReadBps() const { return limitReadBps; }
        uint64_t getLimitWriteBps() const { return limitWriteBps; }
        std::string getLiveCSVFilePath() const { return liveCSVFilePath; }
        std::string getLiveJSONFilePath() const { return liveJSONFilePath; }
        size_t getLiveStatsSleepMS() const { return liveStatsSleepMS; }
        LogLevel getLogLevel() const { return (LogLevel)logLevel; }
		unsigned getMadviseFlags() const { return madviseFlags; };
		const BufferVec& getMmapVec() const { return mmapVec; }
		const NetBenchServerAddrVec& getNetBenchServers() const { return netBenchServersVec; }
		unsigned getNextPhaseDelaySecs() const { return nextPhaseDelaySecs; }
        std::string getNumaZonesStr() const { return numaZonesStr; }
        const IntVec& getNumaZonesVec() const { return numaZonesVec; }
        size_t getNumDirs() const { return numDirs; }
        size_t getNumDataSetThreads() const { return numDataSetThreads; }
        size_t getNumFiles() const { return numFiles; }
        int getNumHosts() const { return numHosts; }
        unsigned getNumLatencyPercentile9s() const { return numLatencyPercentile9s; }
        unsigned getNumNetBenchServers() const { return numNetBenchServers; }
        size_t getNumRWMixReadThreads() const { return numRWMixReadThreads; }
        size_t getNumThreads() const { return numThreads; }
        std::string getNetDevsStr() const { return netDevsStr; }
        const StringVec& getNetDevsVec() const { return netDevsVec; }
        size_t getNetBenchRespSize() const { return netBenchRespSize; }
        bool getNoDirectIOCheck() const { return noDirectIOCheck; }
		bool getPrintCSVLabels() const { return !noCSVLabels; }
		std::string getOpsLogPath() const { return opsLogPath; }
		bool getQuitServices() const { return quitServices; }
        std::string getRandOffsetAlgo() const { return randOffsetAlgo; }
		uint64_t getRandomAmount() const { return randomAmount; }
        size_t getRankOffset() const { return rankOffset; }
        std::string getResFilePath() const { return resFilePath; }
        unsigned getRotateHostsNum() const { return rotateHostsNum; }
        bool getRunAsService() const { return runAsService; }
        bool getRunCreateDirsPhase() const { return runCreateDirsPhase; }
        bool getRunCreateFilesPhase() const { return runCreateFilesPhase; }
        bool getRunDeleteDirsPhase() const { return runDeleteDirsPhase; }
        bool getRunDeleteFilesPhase() const { return runDeleteFilesPhase; }
        bool getRunDropCachesPhase() const { return runDropCachesPhase; }
        bool getRunListObjParallelPhase() const { return runS3ListObjParallel; }
        bool getRunListObjPhase() const { return (runS3ListObjNum > 0); }
        bool getRunMultiDelObjPhase() const { return (runS3MultiDelObjNum > 0); }
        bool getRunReadPhase() const { return runReadPhase; }
        bool getRunS3AclPut() const { return runS3AclPut; }
        bool getRunS3AclGet() const { return runS3AclGet; }
        bool getRunS3BucketAclPut() const { return runS3BucketAclPut; }
        bool getRunS3BucketAclGet() const { return runS3BucketAclGet; }
        bool getRunS3StatDirs() const { return runS3StatDirs; }
        bool getRunServiceInForeground() const { return runServiceInForeground; }
        bool getRunStatFilesPhase() const { return runStatFilesPhase; }
        bool getRunSyncPhase() const { return runSyncPhase; }
        unsigned getRWMixReadPercent() const { return rwMixReadPercent; }
        unsigned getRWMixThreadsReadPercent() const { return rwMixThreadsReadPercent; }
        std::string getS3AccessKey() const { return s3AccessKey; }
        std::string getS3AccessSecret() const { return s3AccessSecret; }
        std::string getS3AclGrantee() const { return s3AclGrantee; }
        std::string getS3AclGranteeType() const { return s3AclGranteeType; }
        std::string getS3AclGranteePermissions() const { return s3AclGranteePermissions; }
		std::string getS3CredentialsFile() const { return s3CredentialsFile; }
        std::string getS3CredentialsList() const { return s3CredentialsList; }
        std::string getS3EndpointsServiceOverride() const { return s3EndpointsServiceOverrideStr; }
        std::string getS3EndpointsStr() const { return s3EndpointsStr; }
        const StringVec& getS3EndpointsVec() const { return s3EndpointsVec; }
        bool getS3IgnoreMultipartUpload404() const { return s3IgnoreMultipartUpload404; }
        uint64_t getS3ListObjNum() const { return runS3ListObjNum; }
        unsigned short getS3LogLevel() const { return s3LogLevel; }
        std::string getS3LogfilePrefix() const { return s3LogfilePrefix; }
        bool getS3NoCompression() const { return s3NoCompression; };
        bool getS3NoMpuCompletion() const { return s3NoMpuCompletion; };
        uint64_t getS3MultiDelObjNum() const { return runS3MultiDelObjNum; }
        const std::string& getS3ObjectPrefix() const { return s3ObjectPrefix; }
        std::string getS3Region() const { return s3Region; }
        std::string getS3SessionToken() const { return s3SessionToken; }
        unsigned short getS3SignPolicy() const { return s3SignPolicy; }
        std::string getS3SSECKey() const { return s3SSECKey; }
        std::string getS3SSEKMSKey() const { return s3SSEKMSKey; }
        unsigned short getServicePort() const { return servicePort; }
        bool getShowAllElapsed() const { return showAllElapsed; }
        bool getShowCPUUtilization() const { return showCPUUtilization; }
        bool getShowDirStats() const { return showDirStats; }
        bool getShowLatency() const { return showLatency; }
        bool getShowLatencyHistogram() const { return showLatencyHistogram; }
        bool getShowLatencyPercentiles() const { return showLatencyPercentiles; }
        bool getShowServicesElapsed() const { return showServicesElapsed; }
        int getSockRecvBufSize() const { return sockRecvBufSize; }
        int getSockSendBufSize() const { return sockSendBufSize; }
        std::string getSvcPasswordFile() const { return svcPasswordFile; }
        std::string getSvcPasswordHash() const { return svcPasswordHash; }
        size_t getSvcReadyWaitSec() const { return svcReadyWaitSec; }
        size_t getSvcUpdateIntervalMS() const { return svcUpdateIntervalMS; }
        time_t getStartTime() const { return startTime; }
        int getStdoutDupFD() const { return stdoutDupFD; }
        std::string getTreeScanPath() const { return treeScanPath; }
        bool getUseAlternativeHTTPService() const { return useAlternativeHTTPService; }
        bool getUseBriefLiveStats() const { return useBriefLiveStats; }
        bool getUseBriefLiveStatsNewLine() const { return useBriefLiveStatsNewLine; }
        bool getUseCuFile() const { return useCuFile; }
        bool getUseCuFileDriverOpen() const { return useCuFileDriverOpen; }
        bool getUseCuHostBufReg() const { return useCuHostBufReg; }
        bool getUseCustomTreeRandomize() const { return useCustomTreeRandomize; }
        bool getUseDirectIO() const { return useDirectIO; }
        bool getUseExtendedLiveCSV() const { return useExtendedLiveCSV; }
        bool getUseExtendedLiveJSON() const { return useExtendedLiveJSON; }
        bool getUseGPUBufReg() const { return useGDSBufReg; }
        bool getUseHDFS() const { return useHDFS; }
        bool getUseMmap() const { return useMmap; }
        bool getUseNetBench() const { return useNetBench; }
        bool getUseNoFDSharing() const { return useNoFDSharing; }
        bool getUseOpsLogLocking() const { return useOpsLogLocking; }
        bool getUseRandomUnaligned() const { return useRandomUnaligned; }
        bool getUseRandomOffsets() const { return useRandomOffsets; }
        bool getUseS3FastRead() const { return useS3FastRead; }
        bool getUseS3ObjectPrefixRand() const { return useS3ObjectPrefixRand; }
        bool getUseS3RandObjSelect() const { return useS3RandObjSelect; }
        bool getUseS3SSE() const { return useS3SSE; }
        bool getUseStridedAccess() const { return useStridedAccess; }
		size_t getTimeLimitSecs() const { return timeLimitSecs; }
		std::string getTreeFilePath() const { return treeFilePath; }
        uint64_t getTreeRoundUpSize() const { return treeRoundUpSize; }
        bool hasUserSetRWMixPercent() const { return useRWMixPercent; }
        bool hasUserSetRWMixReadThreads() const { return useRWMixReadThreads; }
        std::string getS3ChecksumAlgo() const { return s3ChecksumAlgoStr; }

		// setters for config options in alphabetic order...

        void setIgnoreDelErrors(bool ignoreDelErrors) { this->ignoreDelErrors = ignoreDelErrors; }
        void setTimeLimitSecs(size_t timeLimitSecs) { this->timeLimitSecs = timeLimitSecs; }
};


#endif /* PROGARGS_H_ */
