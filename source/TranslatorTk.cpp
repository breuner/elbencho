#include "ProgException.h"
#include "TranslatorTk.h"


/**
 * Get name of a phase from bench phase.
 *
 * @return PHASENAME_...
 * @throw ProgException on invalid benchPhase value
 */
std::string TranslatorTk::benchPhaseToPhaseName(BenchPhase benchPhase)
{
	switch(benchPhase)
	{
		case BenchPhase_IDLE: return PHASENAME_IDLE;
		case BenchPhase_TERMINATE: return PHASENAME_TERMINATE;
		case BenchPhase_CREATEDIRS: return PHASENAME_CREATEDIRS;
		case BenchPhase_DELETEDIRS: return PHASENAME_DELETEDIRS;
		case BenchPhase_CREATEFILES: return PHASENAME_CREATEFILES;
		case BenchPhase_READFILES: return PHASENAME_READFILES;
		case BenchPhase_DELETEFILES: return PHASENAME_DELETEFILES;
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
 * @return PHASEENTRYTYPE_...
 * @throw ProgException on invalid benchPhase value
 */
std::string TranslatorTk::benchPhaseToPhaseEntryType(BenchPhase benchPhase)
{
	switch(benchPhase)
	{
		case BenchPhase_CREATEDIRS:
		case BenchPhase_DELETEDIRS:
		{
			return PHASEENTRYTYPE_DIRS;
		} break;
		case BenchPhase_CREATEFILES:
		case BenchPhase_READFILES:
		case BenchPhase_DELETEFILES:
		{
			return PHASEENTRYTYPE_FILES;
		} break;
		default:
		{ // should never happen
			throw ProgException("Phase entry type requested for unknown/invalid phase type: " +
				std::to_string(benchPhase) );
		} break;
	}
}

/**
 * Get human-readable version of bench path type.
 */
std::string TranslatorTk::benchPathTypeToStr(BenchPathType pathType)
{
	switch(pathType)
	{
		case BenchPathType_DIR:
			return "dir";
		case BenchPathType_FILE:
			return "file";
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
