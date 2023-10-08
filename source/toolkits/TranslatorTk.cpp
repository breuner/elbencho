#include <boost/algorithm/string.hpp>
#include "ProgArgs.h"
#include "ProgException.h"
#include "TranslatorTk.h"

#define TRANSLATORTK_PHASENAME_RWMIXPCT	"RWMIX" // rwmix with read percentage
#define TRANSLATORTK_PHASENAME_RWMIXTHR	"MIX-T" // rwmix with separate reader threads
#define TRANSLATORTK_PHASENAME_NETBENCH "NET" // write/create phase name in netbench mode

/**
 * Get name of a phase from bench phase.
 *
 * @return PHASENAME_...
 * @throw ProgException on invalid benchPhase value
 */
std::string TranslatorTk::benchPhaseToPhaseName(BenchPhase benchPhase, const ProgArgs* progArgs)
{
	switch(benchPhase)
	{
		case BenchPhase_IDLE: return PHASENAME_IDLE;
		case BenchPhase_TERMINATE: return PHASENAME_TERMINATE;
		case BenchPhase_CREATEDIRS: return PHASENAME_CREATEDIRS;
		case BenchPhase_DELETEDIRS: return PHASENAME_DELETEDIRS;
		case BenchPhase_CREATEFILES:
		{
			if(progArgs->getUseNetBench() )
				return TRANSLATORTK_PHASENAME_NETBENCH;
			else
			if(progArgs->hasUserSetRWMixReadThreads() )
				return TRANSLATORTK_PHASENAME_RWMIXTHR +
					std::to_string(progArgs->getNumRWMixReadThreads() );
			else
			if(progArgs->hasUserSetRWMixPercent() )
				return TRANSLATORTK_PHASENAME_RWMIXPCT +
					std::to_string(progArgs->getRWMixPercent() );
			else
				return PHASENAME_CREATEFILES;
		}
		case BenchPhase_READFILES: return PHASENAME_READFILES;
		case BenchPhase_DELETEFILES: return PHASENAME_DELETEFILES;
		case BenchPhase_SYNC: return PHASENAME_SYNC;
		case BenchPhase_DROPCACHES: return PHASENAME_DROPCACHES;
		case BenchPhase_STATFILES: return PHASENAME_STATFILES;
		case BenchPhase_LISTOBJECTS: return PHASENAME_LISTOBJECTS;
		case BenchPhase_LISTOBJPARALLEL: return PHASENAME_LISTOBJPAR;
		case BenchPhase_MULTIDELOBJ: return PHASENAME_MULTIDELOBJ;
		default:
		{ // should never happen
			throw ProgException("Phase name requested for unknown/invalid phase type: " +
				std::to_string(benchPhase) );
		} break;
	}
}


/**
 * Get entry type from bench phase.
 *
 * @firstToUpper whether first character should be uppercase
 * @return PHASEENTRYTYPE_...
 * @throw ProgException on invalid benchPhase value
 */
std::string TranslatorTk::benchPhaseToPhaseEntryType(BenchPhase benchPhase, bool firstToUpper)
{
	std::string retVal;

	switch(benchPhase)
	{
		case BenchPhase_CREATEDIRS:
		case BenchPhase_DELETEDIRS:
		{
			retVal = PHASEENTRYTYPE_DIRS;
		} break;
		case BenchPhase_CREATEFILES:
		case BenchPhase_READFILES:
		case BenchPhase_DELETEFILES:
		case BenchPhase_SYNC:
		case BenchPhase_DROPCACHES:
		case BenchPhase_STATFILES:
		case BenchPhase_LISTOBJECTS:
		case BenchPhase_LISTOBJPARALLEL:
		case BenchPhase_MULTIDELOBJ:
		{
			retVal = PHASEENTRYTYPE_FILES;
		} break;
		default:
		{ // should never happen
			throw ProgException("Phase entry type requested for unknown/invalid phase type: " +
				std::to_string(benchPhase) );
		} break;
	}

	if(firstToUpper)
		retVal[0] = std::toupper(retVal[0]);

	return retVal;
}

/**
 * Get human-readable version of bench path type.
 */
std::string TranslatorTk::benchPathTypeToStr(BenchPathType pathType, const ProgArgs* progArgs)
{
	switch(pathType)
	{
		case BenchPathType_DIR:
			if(progArgs->getUseHDFS() )
				return "hdfs";
			else
			if(!progArgs->getS3EndpointsStr().empty() )
				return "bucket";
			else
				return "dir";

		case BenchPathType_FILE:
			return progArgs->getS3EndpointsStr().empty() ? "file" : "object";
		case BenchPathType_BLOCKDEV:
			return "blockdev";
		default:
		{ // should never happen
			throw ProgException("BenchPathType requested for unknown/invalid value: " +
				std::to_string(pathType) );
		} break;
	}
}

/**
 * Turn elements of a string vector into a single string where each element is separated by given
 * separator. This can be used e.g. to create CSV format from a StringVec.
 *
 * The separator will only be inserted between elements, not behind the last element.
 */
std::string TranslatorTk::stringVecToString(const StringVec& vec, std::string separator)
{
	std::string result;

	for(const std::string& elem : vec)
	{
		if(!result.empty() )
			result += separator; // this is not the first element, so add separator

		result += elem;
	}

	return result;
}

/**
 * Parse a list of ARG_FADVISE_FLAG_x_NAME elements separated by FADVISELIST_DELIMITERS into
 * a ARG_FADVISE_FLAG_x flags value.
 *
 * @return combined ARG_FADVISE_FLAG_x flags value.
 *
 * @throw ProgException in case of invalid string in fadviseArgsStr.
 */
unsigned TranslatorTk::fadviseArgsStrToFlags(std::string fadviseArgsStr)
{
	StringVec fadviseStrVec;
	unsigned fadviseFlags = 0;

	boost::split(fadviseStrVec, fadviseArgsStr, boost::is_any_of(FADVISELIST_DELIMITERS),
		boost::token_compress_on);

	for(std::string currentFadviseArgStr : fadviseStrVec)
	{
		if(currentFadviseArgStr.empty() )
			continue;
		else
		if(currentFadviseArgStr == ARG_FADVISE_FLAG_SEQ_NAME)
			fadviseFlags |= ARG_FADVISE_FLAG_SEQ;
		else
		if(currentFadviseArgStr == ARG_FADVISE_FLAG_RAND_NAME)
			fadviseFlags |= ARG_FADVISE_FLAG_RAND;
		else
		if(currentFadviseArgStr == ARG_FADVISE_FLAG_WILLNEED_NAME)
			fadviseFlags |= ARG_FADVISE_FLAG_WILLNEED;
		else
		if(currentFadviseArgStr == ARG_FADVISE_FLAG_DONTNEED_NAME)
			fadviseFlags |= ARG_FADVISE_FLAG_DONTNEED;
		else
		if(currentFadviseArgStr == ARG_FADVISE_FLAG_NOREUSE_NAME)
			fadviseFlags |= ARG_FADVISE_FLAG_NOREUSE;
		else
			throw ProgException("Invalid fadvise: " + currentFadviseArgStr);
	}

	return fadviseFlags;
}

/**
 * Parse a list of ARG_MADVISE_FLAG_x_NAME elements separated by MADVISELIST_DELIMITERS into
 * a ARG_MADVISE_FLAG_x flags value.
 *
 * @return combined ARG_MADVISE_FLAG_x flags value.
 *
 * @throw ProgException in case of invalid string in madviseArgsStr.
 */
unsigned TranslatorTk::madviseArgsStrToFlags(std::string madviseArgsStr)
{
	StringVec madviseStrVec;
	unsigned madviseFlags = 0;

	boost::split(madviseStrVec, madviseArgsStr, boost::is_any_of(MADVISELIST_DELIMITERS),
		boost::token_compress_on);

	for(std::string currentMadviseArgStr : madviseStrVec)
	{
		if(currentMadviseArgStr.empty() )
			continue;
		else
		if(currentMadviseArgStr == ARG_MADVISE_FLAG_SEQ_NAME)
			madviseFlags |= ARG_MADVISE_FLAG_SEQ;
		else
		if(currentMadviseArgStr == ARG_MADVISE_FLAG_RAND_NAME)
			madviseFlags |= ARG_MADVISE_FLAG_RAND;
		else
		if(currentMadviseArgStr == ARG_MADVISE_FLAG_WILLNEED_NAME)
			madviseFlags |= ARG_MADVISE_FLAG_WILLNEED;
		else
		if(currentMadviseArgStr == ARG_MADVISE_FLAG_DONTNEED_NAME)
			madviseFlags |= ARG_MADVISE_FLAG_DONTNEED;
		else
		if(currentMadviseArgStr == ARG_MADVISE_FLAG_HUGEPAGE_NAME)
			madviseFlags |= ARG_MADVISE_FLAG_HUGEPAGE;
		else
		if(currentMadviseArgStr == ARG_MADVISE_FLAG_NOHUGEPAGE_NAME)
			madviseFlags |= ARG_MADVISE_FLAG_NOHUGEPAGE;
		else
			throw ProgException("Invalid madvise: " + currentMadviseArgStr);
	}

	return madviseFlags;
}

/**
 * Get a human-readable string from an IntVec. The result groups ranges and comma-separates
 * non-consecutive numbers, e.g. "2,6-31,983". Grouping relies on intVec being sorted.
 *
 * @return e.g. "2,6-31,983" or empty string if intVec is empty.
 */
std::string TranslatorTk::intVectoHumanStr(const IntVec& intVec)
{
	int rangeStart;
	int rangeLast;
	std::string resultStr;

	if(intVec.empty() )
		return resultStr;

	rangeStart = intVec[0];
	rangeLast = intVec[0];

	for(size_t i=1; i < intVec.size(); i++)
	{
		// existing range => check if next num is still consecutive
		if(intVec[i] == (rangeLast + 1) )
			rangeLast = intVec[i]; // consecutive number in current range
		else
		{ // non-consecutive number, so finish current range and start a new range
			if(rangeStart == rangeLast)
				resultStr += std::to_string(rangeStart) + ",";
			else
				resultStr += std::to_string(rangeStart) + "-" + std::to_string(rangeLast) + ",";

			rangeStart = intVec[i];
			rangeLast = intVec[i];
		}
	}

	// we still need to add the last range. (it cannot be empty, because we have an empty check)

	if(rangeStart == rangeLast)
		resultStr += std::to_string(rangeStart);
	else
		resultStr += std::to_string(rangeStart) + "-" + std::to_string(rangeLast);

	return resultStr;
}
