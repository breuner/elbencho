#include <boost/algorithm/string.hpp>
#include <iterator>
#include <regex>
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
			std::string phaseName;

			if(progArgs->getUseNetBench() )
				phaseName = TRANSLATORTK_PHASENAME_NETBENCH;
			else
			if(progArgs->hasUserSetRWMixReadThreads() )
				phaseName = TRANSLATORTK_PHASENAME_RWMIXTHR +
					std::to_string(progArgs->getNumRWMixReadThreads() );
			else
			if(progArgs->hasUserSetRWMixPercent() )
				phaseName = TRANSLATORTK_PHASENAME_RWMIXPCT +
					std::to_string(progArgs->getRWMixPercent() );
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
		case BenchPhase_DELETEFILES: return PHASENAME_DELETEFILES;
		case BenchPhase_SYNC: return PHASENAME_SYNC;
		case BenchPhase_DROPCACHES: return PHASENAME_DROPCACHES;
		case BenchPhase_STATFILES: return PHASENAME_STATFILES;
		case BenchPhase_PUTBUCKETACL: return PHASENAME_PUTBUCKETACL;
		case BenchPhase_PUTOBJACL: return PHASENAME_PUTOBJACL;
		case BenchPhase_GETOBJACL: return PHASENAME_GETOBJACL;
		case BenchPhase_GETBUCKETACL: return PHASENAME_GETBUCKETACL;
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
		case BenchPhase_PUTBUCKETACL:
		case BenchPhase_PUTOBJACL:
		case BenchPhase_GETOBJACL:
		case BenchPhase_GETBUCKETACL:
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

/**
 * Expand the first square brackets range or list spec in a string and store the resulting strings
 * in outStrVec.
 *
 * Examples:
 * * Multiple ranges: "myhost[1-4,6,8-10]-rack[1,2]"; note that only first square brackets pair
 * 		will be expanded.
 * * Zero fill "myhost[001-100]".
 *
 * @inputStr input string that potentially contains square brackets.
 * @outStrVec empty string if nothing found to expand; the contained elements might still contian
 * 		further ranges, so you should call this method again on them.
 *
 * 	@throw ProgException on parsing error, e.g. no matching closing bracket.
 */
void TranslatorTk::expandSquareBracketsStr(std::string inputStr, StringVec& outStrVec)
{
	std::size_t openBracketPos = inputStr.find("[");
	std::size_t closeBracketPos = inputStr.find("]");

	// check for square brackets
	if( (openBracketPos == std::string::npos) && (closeBracketPos == std::string::npos) )
		return; // no square brackets => nothing to do

	// check for existence of open and close square bracket
	if( ( (openBracketPos != std::string::npos) && (closeBracketPos == std::string::npos) ) ||
		( (openBracketPos == std::string::npos) && (closeBracketPos != std::string::npos) ) )
		throw ProgException("Missing matching opening or closing square bracket: " + inputStr);

	// check for right order of open and close square bracket
	if(closeBracketPos < openBracketPos)
		throw ProgException("Invalid closing square bracket before opening bracket: " + inputStr);

	// check if we have something between the brackets
	if(closeBracketPos == (openBracketPos+1) )
		throw ProgException("Invalid square brackets with empty content: " + inputStr);

	// now we have an opening and a closing square bracket with at least one char in between...

	// get the string between the brackets (not including the brackets)
	std::string bracketContentsStr = inputStr.substr(
		openBracketPos+1, closeBracketPos - openBracketPos - 1);

	std::size_t invalidCharPos = bracketContentsStr.find_first_not_of("0123456789,-");
	if(invalidCharPos != std::string::npos)
		throw ProgException("Found invalid character in square brackets: "
			"Char: '" + std::string().append(&bracketContentsStr[invalidCharPos], 1) + "'; "
			"Brackets: '" + bracketContentsStr + "'; "
			"String: '" + inputStr + "'");

	unsigned bracketsLen = closeBracketPos - openBracketPos + 1; // str len of the whole "[...]"
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

			newElemStr.replace(openBracketPos, bracketsLen, currentElem);

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

			newElemStr.replace(openBracketPos, bracketsLen, currentFilledNumStr);

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
 * square brackets, but with a different meaning if they are in square brackets.
 *
 * @return true if something was replaced, false otherwise.
 */
bool TranslatorTk::replaceCommasOutsideOfSquareBrackets(std::string& inoutStr,
	std::string replacementStr)
{
	// regular expression to idenfity find commas outside of square brackets
	std::regex regexCommasOutsideSquareBrackets(",(?![^\\[]*\\])");

	std::string replacedStr = std::regex_replace(
		inoutStr, regexCommasOutsideSquareBrackets, replacementStr);

	bool didReplace = (inoutStr != replacedStr);

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
	// regular expression to idenfity find commas outside of square brackets
	std::regex regexComma(",");

	// write the results to an output iterator
	std::string replacedStr = std::regex_replace(str, regexComma, "");

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

#ifdef S3_SUPPORT

/**
 * Convert grantee and grants given in progArgs into grants for S3 request.
 *
 * @outGrants will be filled based on user-defined s3 object acl grantee and permission values in
 * 		progArgs.
 * @throws WorkerException on error (e.g. unknown/invalid user-defined values found).
 */
void TranslatorTk::getS3ObjectAclGrants(const ProgArgs* progArgs, Aws::Vector<S3::Grant>& outGrants)
{
	Aws::S3::Model::Grantee grantee;

	// set grantee...

	if(progArgs->getS3AclGranteeType() == ARG_S3ACL_GRANTEE_TYPE_EMAIL)
	{
		grantee.SetType(S3::Type::AmazonCustomerByEmail);
		grantee.SetEmailAddress(progArgs->getS3AclGrantee());
	}
	else if(progArgs->getS3AclGranteeType() == ARG_S3ACL_GRANTEE_TYPE_ID)
	{
		grantee.SetType(S3::Type::CanonicalUser);
		grantee.SetID(progArgs->getS3AclGrantee());
	}
	else if(progArgs->getS3AclGranteeType() == ARG_S3ACL_GRANTEE_TYPE_URI)
	{
		grantee.SetType(S3::Type::CanonicalUser);
		grantee.SetURI(progArgs->getS3AclGrantee());
	}
	else if(progArgs->getS3AclGranteeType() == ARG_S3ACL_GRANTEE_TYPE_GROUP)
	{
		grantee.SetType(S3::Type::Group);
		grantee.SetURI(progArgs->getS3AclGrantee());
	}
	else
		throw WorkerException("Undefined/unknown S3 ACL grantee type: "
				"'" + progArgs->getS3AclGranteeType() + "'");


	// add grantee's permissions to outGrants...

	if(progArgs->getS3AclGranteePermissions().find(ARG_S3ACL_PERM_FULL_NAME) != std::string::npos)
	{
		S3::Grant grant;

		grant.SetGrantee(grantee);
		grant.SetPermission(S3::Permission::FULL_CONTROL);

		outGrants.push_back(grant);

		return;
	}

	if(progArgs->getS3AclGranteePermissions().find(ARG_S3ACL_PERM_NONE_NAME) != std::string::npos)
	{
		S3::Grant grant;

		grant.SetGrantee(grantee);
		grant.SetPermission(S3::Permission::NOT_SET);

		outGrants.push_back(grant);

		return;
	}

	if(progArgs->getS3AclGranteePermissions().find(ARG_S3ACL_PERM_FLAG_READ_NAME) !=
		std::string::npos)
	{
		S3::Grant grant;

		grant.SetGrantee(grantee);
		grant.SetPermission(S3::Permission::READ);

		outGrants.push_back(grant);
	}

	if(progArgs->getS3AclGranteePermissions().find(ARG_S3ACL_PERM_FLAG_WRITE_NAME) !=
		std::string::npos)
	{
		S3::Grant grant;

		grant.SetGrantee(grantee);
		grant.SetPermission(S3::Permission::WRITE);

		outGrants.push_back(grant);
	}

	if(progArgs->getS3AclGranteePermissions().find(ARG_S3ACL_PERM_FLAG_READACP_NAME) !=
		std::string::npos)
	{
		S3::Grant grant;

		grant.SetGrantee(grantee);
		grant.SetPermission(S3::Permission::READ_ACP);

		outGrants.push_back(grant);
	}

	if(progArgs->getS3AclGranteePermissions().find(ARG_S3ACL_PERM_FLAG_WRITEACP_NAME) !=
		std::string::npos)
	{
		S3::Grant grant;

		grant.SetGrantee(grantee);
		grant.SetPermission(S3::Permission::WRITE_ACP);

		outGrants.push_back(grant);
	}
}

std::string TranslatorTk::s3AclPermissionToStr(const S3::Permission& s3Permission)
{
	switch(s3Permission)
	{
		case S3::Permission::NOT_SET:
			return ARG_S3ACL_PERM_NONE_NAME;
		case S3::Permission::FULL_CONTROL:
			return ARG_S3ACL_PERM_FULL_NAME;
		case S3::Permission::WRITE:
			return ARG_S3ACL_PERM_FLAG_WRITE_NAME;
		case S3::Permission::WRITE_ACP:
			return ARG_S3ACL_PERM_FLAG_WRITEACP_NAME;
		case S3::Permission::READ:
			return ARG_S3ACL_PERM_FLAG_READ_NAME;
		case S3::Permission::READ_ACP:
			return ARG_S3ACL_PERM_FLAG_READACP_NAME;
		default:
			return "unknown";
	}
}


#endif // S3_SUPPORT
