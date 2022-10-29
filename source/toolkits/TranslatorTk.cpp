#include "ProgArgs.h"
#include "ProgException.h"
#include "TranslatorTk.h"

#define TRANSLATORTK_PHASENAME_RWMIXPCT	"RWMIX" // rwmix with read percentage
#define TRANSLATORTK_PHASENAME_RWMIXTHR	"MIX-T" // rwmix with separate reader threads

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
			return progArgs->getS3EndpointsStr().empty() ? "dir" : "bucket";
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
			result += separator; // not the first element, so prepend separator

		result += elem;
	}

	return result;
}
