#ifndef COMMON_H_
#define COMMON_H_

#include <boost/config.hpp> // for BOOST_LIKELY
#include <list>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

typedef std::list<std::string> StringList;
typedef std::set<std::string> StringSet;
typedef std::vector<std::string> StringVec;
typedef std::vector<int> IntVec;
typedef std::vector<char*> BufferVec;
typedef std::vector<size_t> SizeTVec;
typedef std::vector<uint64_t> UInt64Vec;


#define STRINGIZE(value)		_STRINGIZE(value) // 2 levels necessary for macro expansion
#define _STRINGIZE(value)		#value


// human-readable benchmark phase name

#define PHASENAME_IDLE			"IDLE"
#define PHASENAME_TERMINATE		"QUIT"
#define PHASENAME_CREATEDIRS	"MKDIRS"
#define PHASENAME_CREATEFILES	"WRITE"
#define PHASENAME_READFILES		"READ"
#define PHASENAME_DELETEFILES	"RMFILES"
#define PHASENAME_DELETEDIRS	"RMDIRS"
#define PHASENAME_SYNC			"SYNC"
#define PHASENAME_DROPCACHES	"DROPCACHE"
#define PHASENAME_STATFILES		"STAT"
#define PHASENAME_LISTOBJECTS	"LISTOBJ"
#define PHASENAME_LISTOBJPAR	"LISTOBJ_P"


// human-readable entry type in current benchmark phase

#define PHASEENTRYTYPE_DIRS		"dirs"
#define PHASEENTRYTYPE_FILES	"files"

/*
 * Messaging protocol version. Exchanged between master and services to check compatibility.
 * (Only exact matches are assumed to be compatible, that's why this can differ from the program
 * version.)
 */
#define HTTP_PROTOCOLVERSION	"2.0.14"

/**
 * Default access mode bits for new files.
 */
#define MKFILE_MODE				(S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH)

/**
 * Call delete() on an object pointer if it's not NULL and afterwards set it to NULL.
 */
#define SAFE_DELETE(objectPointer) \
	do \
	{ \
		if(objectPointer) \
		{ \
			delete(objectPointer); \
			objectPointer = NULL; \
		}  \
	} while(0)

/**
 * Call free on a pointer if it's not NULL and afterwards set it to NULL.
 */
#define SAFE_FREE(pointer) \
	do \
	{ \
		if(pointer) \
		{ \
			free(pointer); \
			pointer = NULL; \
		}  \
	} while(0)


#ifdef BOOST_UNLIKELY
	#define IF_UNLIKELY(condition)	if(BOOST_UNLIKELY(condition) )
#else // fallback for older boost versions
	#define IF_UNLIKELY(condition)	if(__builtin_expect(condition, 0) )
#endif


/**
 * Current benchmark phase.
 */
enum BenchPhase
{
	BenchPhase_IDLE = 0,
	BenchPhase_TERMINATE, // tells workers to self-terminate when all is done
	BenchPhase_CREATEDIRS,
	BenchPhase_DELETEDIRS,
	BenchPhase_CREATEFILES,
	BenchPhase_DELETEFILES,
	BenchPhase_READFILES,
	BenchPhase_SYNC,
	BenchPhase_DROPCACHES,
	BenchPhase_STATFILES,
	BenchPhase_LISTOBJECTS,
	BenchPhase_LISTOBJPARALLEL,
};


/**
 * Type of user-given benchmark paths.
 */
enum BenchPathType
{
	BenchPathType_DIR=0, // also used for s3
	BenchPathType_FILE=1,
	BenchPathType_BLOCKDEV=2,
};


/**
 * Retrieved by master from services as part of preparation phase.
 */
struct BenchPathInfo
{
	std::string benchPathStr;
	BenchPathType benchPathType;
	size_t numBenchPaths;
	uint64_t fileSize;
	uint64_t blockSize;
	uint64_t randomAmount;
};

typedef std::vector<BenchPathInfo> BenchPathInfoVec;


// http service transferred parameters (used as http GET parameters or in json document)

#define XFER_PREP_PROTCOLVERSION				"ProtocolVersion"
#define XFER_PREP_BENCHPATHTYPE					"BenchPathType"
#define XFER_PREP_ERRORHISTORY					"ErrorHistory"
#define XFER_PREP_NUMBENCHPATHS					"NumBenchPaths"
#define XFER_PREP_FILENAME						"FileName"

#define XFER_STATS_BENCHID 						"BenchID"
#define XFER_STATS_BENCHPHASENAME 				"PhaseName"
#define XFER_STATS_BENCHPHASECODE				"PhaseCode"
#define XFER_STATS_NUMWORKERSDONE				"NumWorkersDone"
#define XFER_STATS_NUMWORKERSDONEWITHERR		"NumWorkersDoneWithError"
#define XFER_STATS_NUMENTRIESDONE 				"NumEntriesDone"
#define XFER_STATS_NUMBYTESDONE 				"NumBytesDone"
#define XFER_STATS_NUMIOPSDONE 					"NumIOPSDone"
#define XFER_STATS_NUMENTRIESDONE_RWMIXREAD		"NumEntriesDoneRWMixRead"
#define XFER_STATS_NUMBYTESDONE_RWMIXREAD		"NumBytesDoneRWMixRead"
#define XFER_STATS_NUMIOPSDONE_RWMIXREAD		"NumIOPSDoneRWMixRead"
#define XFER_STATS_ELAPSEDUSECLIST				"ElapsedUSecList"
#define XFER_STATS_ELAPSEDUSECLIST_ITEM			"ElapsedUSecList.item"
#define XFER_STATS_ELAPSEDSECS 					"ElapsedSecs"
#define XFER_STATS_ERRORHISTORY					XFER_PREP_ERRORHISTORY
#define XFER_STATS_LAT_PREFIX_IOPS				"IOPS_"
#define XFER_STATS_LAT_PREFIX_ENTRIES			"Entries_"
#define XFER_STATS_LAT_PREFIX_IOPS_RWMIXREAD	"IOPSRWMixRead_"
#define XFER_STATS_LAT_PREFIX_ENTRIES_RWMIXREAD	"EntriesRWMixRead_"
#define XFER_STATS_LATMICROSECTOTAL				"LatMicroSecTotal"
#define XFER_STATS_LATNUMVALUES					"LatNumValues"
#define XFER_STATS_LATMINMICROSEC				"LatMinMicroSec"
#define XFER_STATS_LATMAXMICROSEC				"LatMaxMicroSec"
#define XFER_STATS_LATHISTOLIST					"LatHistoList"
#define XFER_STATS_LATHISTOLIST_ITEM			"LatHistoList.item"
#define XFER_STATS_CPUUTIL_STONEWALL			"CPUUtilStoneWall"
#define XFER_STATS_CPUUTIL						"CPUUtil"

#define XFER_START_BENCHID						XFER_STATS_BENCHID
#define XFER_START_BENCHPHASECODE				XFER_STATS_BENCHPHASECODE

#define XFER_INTERRUPT_QUIT						"quit"

#endif /* COMMON_H_ */
