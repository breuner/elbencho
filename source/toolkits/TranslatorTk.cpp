// SPDX-FileCopyrightText: 2020-2025 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <algorithm>
#include <boost/algorithm/string.hpp>
#include <iterator>
#include <regex>
#include "Common.h"
#include "ProgArgs.h"
#include "ProgException.h"
#include "TranslatorTk.h"

#define TRANSLATORTK_PHASENAME_RWMIXPCT	"RWMIX" // rwmix with read percentage
#define TRANSLATORTK_PHASENAME_RWMIXTHR	"MIX-T" // rwmix with separate reader threads
#define TRANSLATORTK_PHASENAME_NETBENCH "NET" // write/create phase name in netbench mode


/**
 * Get human-readable name of a benchmark mode.
 *
 * @return name of benchmark mode.
 */
std::string TranslatorTk::benchModeToModeName(BenchMode benchMode)
{
    switch(benchMode)
    {
        case BenchMode_UNDEFINED: return "UNDEFINED";
        case BenchMode_POSIX: return "POSIX";
        case BenchMode_S3: return "S3";
        case BenchMode_HDFS: return "HDFS";
        case BenchMode_NETBENCH: return "NETBENCH";
		case BenchMode_SPDK: return "SPDK";

        default: return "UNKNOWN";
    }
}

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
		case BenchPhase_CREATEDIRS: return (progArgs->getBenchMode() == BenchMode_S3) ?
            PHASENAME_CREATEBUCKETS : PHASENAME_CREATEDIRS;
		case BenchPhase_DELETEDIRS: return (progArgs->getBenchMode() == BenchMode_S3) ?
            PHASENAME_DELETEBUCKETS : PHASENAME_DELETEDIRS;
		case BenchPhase_CREATEFILES:
		{
			std::string phaseName;

			if(progArgs->getBenchMode() == BenchMode_NETBENCH)
				phaseName = TRANSLATORTK_PHASENAME_NETBENCH;
			else
			if(progArgs->hasUserSetRWMixReadThreads() )
				phaseName = TRANSLATORTK_PHASENAME_RWMIXTHR +
					std::to_string(progArgs->getNumRWMixReadThreads() );
			else
			if(progArgs->hasUserSetRWMixPercent() )
				phaseName = TRANSLATORTK_PHASENAME_RWMIXPCT +
					std::to_string(progArgs->getRWMixReadPercent() );
			else
				phaseName = PHASENAME_CREATEFILES;

			if(progArgs->getBenchPathType() == BenchPathType_DIR)
			{
				if(progArgs->getDoStatInline() )
					phaseName += "+s";

				if(progArgs->getDoReadInline() )
					phaseName += "+r";
			}

			return phaseName;
		}
		case BenchPhase_READFILES:
		{
			std::string phaseName = PHASENAME_READFILES;

			if(progArgs->getBenchPathType() == BenchPathType_DIR)
			{
				if(progArgs->getDoStatInline() )
					phaseName += "+s";
			}

			return phaseName;
		}
		case BenchPhase_DELETEFILES: return (progArgs->getBenchMode() == BenchMode_S3) ?
            PHASENAME_DELETEOBJECTS : PHASENAME_DELETEFILES;
		case BenchPhase_SYNC: return PHASENAME_SYNC;
		case BenchPhase_DROPCACHES: return PHASENAME_DROPCACHES;
		case BenchPhase_STATFILES: return (progArgs->getBenchMode() == BenchMode_S3) ?
            PHASENAME_STATOBJECTS : PHASENAME_STATFILES;
		case BenchPhase_PUTBUCKETACL: return PHASENAME_PUTBUCKETACL;
		case BenchPhase_PUTOBJACL: return PHASENAME_PUTOBJACL;
		case BenchPhase_GETOBJACL: return PHASENAME_GETOBJACL;
		case BenchPhase_GETBUCKETACL: return PHASENAME_GETBUCKETACL;
		case BenchPhase_STATDIRS: return PHASENAME_STATDIRS;
		case BenchPhase_LISTOBJECTS: return PHASENAME_LISTOBJECTS;
		case BenchPhase_LISTOBJPARALLEL: return PHASENAME_LISTOBJPAR;
		case BenchPhase_MULTIDELOBJ: return PHASENAME_MULTIDELOBJ;
        case BenchPhase_GET_S3_OBJECT_MD: return PHASENAME_GETOBJECTMETADATA;
        case BenchPhase_PUT_S3_OBJECT_MD: return PHASENAME_PUTOBJECTMETADATA;
        case BenchPhase_DEL_S3_OBJECT_MD: return PHASENAME_DELOBJECTMETADATA;
        case BenchPhase_GET_S3_BUCKET_MD: return PHASENAME_GETBUCKETMETADATA;
        case BenchPhase_PUT_S3_BUCKET_MD: return PHASENAME_PUTBUCKETMETADATA;
        case BenchPhase_DEL_S3_BUCKET_MD: return PHASENAME_DELBUCKETMETADATA;
        case BenchPhase_S3MPUCOMPLETE: return PHASENAME_S3MPUCOMPLETE;
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
std::string TranslatorTk::benchPhaseToPhaseEntryType(BenchPhase benchPhase,
    const ProgArgs* progArgs, bool firstToUpper)
{
    std::string retVal;

    switch(benchPhase)
    {
        case BenchPhase_CREATEDIRS:
        case BenchPhase_DELETEDIRS:
        case BenchPhase_STATDIRS:
        case BenchPhase_PUTBUCKETACL:
        case BenchPhase_GETBUCKETACL:
        case BenchPhase_GET_S3_BUCKET_MD:
        case BenchPhase_PUT_S3_BUCKET_MD:
        case BenchPhase_DEL_S3_BUCKET_MD:
        {
            retVal = (progArgs->getBenchMode() == BenchMode_S3) ?
                PHASEENTRYTYPE_BUCKETS : PHASEENTRYTYPE_DIRS;
        } break;
        case BenchPhase_CREATEFILES:
        case BenchPhase_READFILES:
        case BenchPhase_DELETEFILES:
        case BenchPhase_SYNC:
        case BenchPhase_DROPCACHES:
        case BenchPhase_STATFILES:
        case BenchPhase_PUTOBJACL:
        case BenchPhase_GETOBJACL:
        case BenchPhase_LISTOBJECTS:
        case BenchPhase_LISTOBJPARALLEL:
        case BenchPhase_MULTIDELOBJ:
        case BenchPhase_GET_S3_OBJECT_MD:
        case BenchPhase_PUT_S3_OBJECT_MD:
        case BenchPhase_DEL_S3_OBJECT_MD:
        case BenchPhase_S3MPUCOMPLETE:
        {
            retVal = (progArgs->getBenchMode() == BenchMode_S3) ?
                PHASEENTRYTYPE_OBJECTS : PHASEENTRYTYPE_FILES;
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
            switch(progArgs->getBenchMode() )
            {
                case BenchMode_S3: return "bucket";
                case BenchMode_HDFS: return "hdfs";
                case BenchMode_NETBENCH: return "net";
                default: return "dir";
            } break;

        case BenchPathType_FILE:
            return (progArgs->getBenchMode() == BenchMode_S3) ? "object" : "file";
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
 * Turn ARG_FLOCK_x_NAME
 *
 * @return combined ARG_FADVISE_FLAG_x flags value.
 *
 * @throw ProgException in case of invalid string in fadviseArgsStr.
 */
unsigned short TranslatorTk::flockArgsStrToType(std::string flockArgsStr)
{
    if(flockArgsStr.empty() || (flockArgsStr == ARG_FLOCK_NONE_NAME) )
        return ARG_FLOCK_NONE;
    else
    if(flockArgsStr == ARG_FLOCK_RANGE_NAME)
        return ARG_FLOCK_RANGE;
    else
    if(flockArgsStr == ARG_FLOCK_FULL_NAME)
        return ARG_FLOCK_FULL;
    else
        throw ProgException("Invalid file locking value: " + flockArgsStr);
}

/**
 * Get a human-readable string from an IntVec. The result groups ranges and comma-separates
 * non-consecutive numbers, e.g. "2,6-31,983". Grouping relies on intVec being sorted.
 *
 * @return e.g. "2,6-31,983" or empty string if intVec is empty.
 */
std::string TranslatorTk::intVecToHumanStr(const IntVec& intVec)
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

/**
 * Expand the first square brackets range or list spec in a string and store the resulting strings
 * in outStrVec.
 *
 * Examples:
 * * Multiple ranges: "myhost[1-4,6,8-10]-rack[1,2]"; note that only first square brackets pair
 * 		will be expanded, so caller is responsible for calling this function again with the expanded
 * 		string to expand further brackets.
 * * Zero fill is supported: "myhost[001-100]".
 *
 * @inputStr input string that potentially contains square brackets.
 * @outStrVec empty string if nothing found to expand; the contained elements might still contian
 * 		further ranges, so call this method again on returned outStrVec elements.
 *
 * 	@throw ProgException on parsing error, e.g. no matching closing bracket.
 */
void TranslatorTk::expandSquareBracketsStr(std::string inputStr, StringVec& outStrVec)
{
    /* regex pattern to match square brackets with numbers, commas and dashes.
       * ignores empty bracket pairs and opening/closing brackets without their counterpart.
         * uses closest match, e.g. "[[1-3]" will match "[1-3]".
       * ignores brackets with colons because that could be an IPv6 address.
         * supports nested brackets within IPv6 addresses, but that would be hex and we use base10
           only here atm. */
    #if defined(__APPLE__)
        // Apple Clang libc++ is buggy for regex with lookaheads like (?!...), so we use a version
        // without that on macOS. Otherwise this throws an exception:
        // "The expression contained mismatched ( and )."
        std::regex bracketsPattern(R"(\[([0-9,\-]+)\])");
    #else // linux
        std::regex bracketsPattern(R"(\[(?![^]]*:)([0-9,\-]+)\])");
    #endif // linux

    std::smatch bracketsMatch;

    bool bracketsMatchFound = std::regex_search(inputStr, bracketsMatch, bracketsPattern);
    if(!bracketsMatchFound)
        return; // no matching square brackets pair => nothing to do

    // now we have an opening and a closing square bracket with at least one char in between...

    #if defined(__APPLE__)
        // because of buggy Apple Clang libc++ regex with lookaheads, we have to check here if we
        // have a colon between the square brackets so that this is an IPv6 address.
        if(bracketsMatch[1].str().find(':') != std::string::npos)
            return; // colon found, so we assume this as an IPv6 address => nothing to do
    #endif // linux

	// get the string between the brackets (not including the brackets)
    std::string bracketContentsStr = inputStr.substr(
        bracketsMatch.position() + 1, bracketsMatch.length() - 2); // ("-2" for '[' and ']')

	StringVec elementsVec;

	boost::split(elementsVec, bracketContentsStr, boost::is_any_of(","), boost::token_compress_on);

	// delete empty string elements from vec. (they come from delimiter use at beginning or end)
	for( ; ; )
	{
		StringVec::iterator iter = std::find(elementsVec.begin(), elementsVec.end(), "");

		if(iter == elementsVec.end() )
			break;

		elementsVec.erase(iter);
	}

	if(elementsVec.empty() )
		throw ProgException("No valid content between square brackets: \"" + inputStr + "\"");

	for(const std::string& currentElem : elementsVec)
	{
		std::size_t dashPos = currentElem.find("-");

		if(dashPos == std::string::npos)
		{ // we have a simple number element

			std::string newElemStr(inputStr);

			newElemStr.replace(bracketsMatch.position(), bracketsMatch.length(), currentElem);

			outStrVec.push_back(newElemStr);

			continue;
		}

		// we have a <start>-<end> case, possibly with leading zeros

		StringVec startAndEndVec;

		boost::split(startAndEndVec, currentElem, boost::is_any_of("-"), boost::token_compress_on);

		if(startAndEndVec.size() != 2)
			throw ProgException("Found invalid range definition in square brackets: "
				"Element: '" + currentElem + "'; "
				"String: '" + inputStr + "'");

		std::string& rangeStartStr = startAndEndVec[0];
		std::string& rangeEndStr = startAndEndVec[1];
		size_t zeroFillLen = rangeStartStr.size(); // all nums shall have length of first num

		// remove leading zeros to prevent interpretation as octal
		// (note: it's possible that the string is all zero, so string is empty after this)
		rangeStartStr.erase(0, rangeStartStr.find_first_not_of('0') );
		rangeEndStr.erase(0, rangeEndStr.find_first_not_of('0') );

		int currentRangeStart;
		int currentRangeEnd;

		try
		{
			// (note: extra empty() check here because of find_first_not_of('0') above)
			currentRangeStart = rangeStartStr.empty() ? 0 : std::stoi(rangeStartStr);
			currentRangeEnd = rangeEndStr.empty() ? 0 : std::stoi(rangeEndStr);
		}
		catch(std::exception& e)
		{
			throw ProgException("Number parsing for square brackets expansion failed: "
				"String: '" + inputStr + "'; " +
				"RangeStart: '" + rangeStartStr + "'; " +
				"RangeEnd: '" + rangeEndStr + "'; " +
				"System exception type (if available): '" + typeid(e).name() + "'; " +
				"System exception message (if available): " + e.what() );
		}

		for(int i = currentRangeStart; i <= currentRangeEnd; i++)
		{
			std::string currentNumStr = std::to_string(i);

			std::string currentFilledNumStr =
				std::string(zeroFillLen - std::min(zeroFillLen, currentNumStr.length() ), '0') +
				currentNumStr;

			std::string newElemStr(inputStr);

			newElemStr.replace(bracketsMatch.position(), bracketsMatch.length(),
			    currentFilledNumStr);

			outStrVec.push_back(newElemStr);

		} // end of single range expansion for-loop

	} // end of current brackets element for-loop

}

/**
 * Wrapper around TranslatorTk::expandSquareBracketsStr() to ensure that multiple ranges in strings
 * get expanded.
 *
 * @inoutStrVec vector of strings that potentially contain square brackets to expand; string
 * 		elements with brackets will be replaced by expanded strings.
 * @return true if something got expanded, false otherwise.
 */
bool TranslatorTk::expandSquareBrackets(StringVec& inoutStrVec)
{
	bool didExpand = false;

	// (note: this is index instead of iters because those get invalid in case of vector reallocs)

	for(unsigned i = 0; i != inoutStrVec.size(); )
	{
		StringVec expandedVec;
		expandSquareBracketsStr(inoutStrVec[i], expandedVec);

		if(expandedVec.size() )
		{ // current elem contained brackets
			didExpand = true;

			inoutStrVec.erase(inoutStrVec.begin() + i);
			inoutStrVec.insert(inoutStrVec.begin() + i, expandedVec.begin(), expandedVec.end() );

			// newly inserted elems might still have further brackets, so don't advance iter
			continue;
		}

		// expandedVec is empty, so nothing got expanded
		i++;
	}

	return didExpand;
}

/**
 * Replace all occurences of commas in inoutStr with replacementStr, but only if the occurences
 * are outside of square brackets.
 * This is useful for strings that are supposed to be split based on chars that might also occur in
 * square brackets, but with a different meaning if they are in square brackets. A typcal case
 * for this are comma-separted hosts that can also contain commas inside square brackets, e.g.
 * "--hosts myhost1,myhost[3,37]".
 *
 * @return true if something was replaced, false otherwise.
 */
bool TranslatorTk::replaceCommasOutsideOfSquareBrackets(std::string& inoutStr,
	std::string replacementStr)
{
    std::string replacedStr;
    replacedStr.reserve(inoutStr.size() + replacementStr.size() );

    bool didReplace = false;
    size_t bracketDepth = 0;

    // substite commas outside of square brackets (i.e. at bracket depth 0)
    /* note: this is a linear bracket-depth scan instead of a std::regex with negative lookahead,
        because the regex method overflows the stack if 1000s of comma-separated hosts are given via
        "--hosts"/"--hostsfile". */
    for(char currentChar : inoutStr)
    {
        switch(currentChar)
        {
            case '[':
                bracketDepth++;
                replacedStr += currentChar;
                break;

            case ']':
                if(bracketDepth > 0)
                    bracketDepth--;
                replacedStr += currentChar;
                break;

            case ',':
                if(bracketDepth == 0)
                { // comma outside of square brackets => replace it
                    replacedStr += replacementStr;
                    didReplace = true;
                }
                else // comma inside square brackets => keep it
                    replacedStr += currentChar;
                break;

            default:
                replacedStr += currentChar;
                break;
        }
    }

    inoutStr = replacedStr;

    return didReplace;
}

/**
 * Erase all occurences of commas from the given string and return the resulting string without
 * commas.
 * This is useful for split delimiters where strings might also contain commas in square brackets.
 */
std::string TranslatorTk::eraseCommas(const std::string& str)
{
    // erase all commas via a simple linear scan
    std::string replacedStr(str);

    replacedStr.erase(
        std::remove(replacedStr.begin(), replacedStr.end(), ','), replacedStr.end() );

    return replacedStr;
}

/**
 * Split the given str into outVec based on the given list of delimiter chars. Then expand the
 * square brackets of each outVec element.
 *
 * @delimiters must contain at least one char that is not a comma.
 *
 * @throw ProgException on error, e.g. parsing error between square brackets or missing comma
 * 		alternative in delimiters.
 */
void TranslatorTk::splitAndExpandStr(std::string str, std::string delimiters,
	StringVec& outVec)
{
	std::string nonCommaDelimiters = TranslatorTk::eraseCommas(delimiters);

	if(nonCommaDelimiters.empty() )
		throw("splitAndExpandStr is missing a comma alternative in delimiters list. "
			"str: '" + str + "'; "
			"delimiters: '" + delimiters + "'");

	std::string commaAlternativeSeparator = nonCommaDelimiters.substr(0, 1);

	TranslatorTk::replaceCommasOutsideOfSquareBrackets(str, commaAlternativeSeparator);

	boost::split(outVec, str, boost::is_any_of(nonCommaDelimiters), boost::token_compress_on);

	TranslatorTk::expandSquareBrackets(outVec);

	LOGGER(Log_DEBUG, __func__ << ": " <<
		"str: '" << str << "'; " <<
		"delimiters: '" << delimiters << "'; " <<
		"outVec: '" << TranslatorTk::stringVecToString(outVec, " ") << "'" << std::endl);
}

/**
 * Remove all empty strings from given inoutVec, including strings that consist only of spaces.
 */
void TranslatorTk::eraseEmptyStringsFromVec(StringVec& inoutVec)
{
	inoutVec.erase(
		std::remove_if(
			inoutVec.begin(),
			inoutVec.end(),
			[](std::string const& s)
			{
				return (s.empty() || (s.find_first_not_of(" ") == std::string::npos) );
			} ),
		inoutVec.end() );
}

/**
 * Remove all BENCHPATH_PREFIX_... prefixes from strings in given inoutVec.
 */
 void TranslatorTk::eraseBenchPathPrefixesFromVec(StringVec& inoutVec)
 {
     for(std::string& path : inoutVec)
     {
        if(path.find(BENCHPATH_PREFIX_POSIX) == 0)
            path.erase(0, strlen(BENCHPATH_PREFIX_POSIX) );
        else
        if(path.find(BENCHPATH_PREFIX_S3) == 0)
            path.erase(0, strlen(BENCHPATH_PREFIX_S3) );
     }
 }

/**
 * Convert an HTTP error code to a human-readable string.
 *
 * @param httpErrorCode the HTTP error code to convert.
 * @return the human-readable string for the given HTTP error code or "Unknown HTTP error code" if
 *      the HTTP error code is unknown.
 */
const char* TranslatorTk::httpErrorCodeToHumanStr(unsigned httpErrorCode)
{
    switch (httpErrorCode)
    {
        // 1xx Informational
        case 100: return "Continue";
        case 101: return "Switching Protocols";

        // 2xx Success
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";

        // 3xx Redirection
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 304: return "Not Modified";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";

        // 4xx Client Error
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 402: return "Payment Required";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 406: return "Not Acceptable";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 410: return "Gone";
        case 416: return "Range Not Satisfiable";
        case 418: return "I'm a teapot"; // RFC 2324
        case 422: return "Unprocessable Entity";
        case 429: return "Too Many Requests";

        // 5xx Server Error
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        case 505: return "HTTP Version Not Supported";

        default: return "Unknown HTTP error code";
    }
}
