#ifndef TOOLKITS_TRANSLATORTK_H_
#define TOOLKITS_TRANSLATORTK_H_

#include <string>
#include <vector>
#include "Common.h"
#include "ProgArgs.h"

#ifdef S3_SUPPORT
	#include <aws/s3/model/PutObjectAclRequest.h>
#endif // S3_SUPPORT


/**
 * A toolkit of static methods to translate from one data structure into another.
 */
class TranslatorTk
{
	private:
		TranslatorTk() {}

		static void expandSquareBracketsStr(std::string rangeStr, StringVec& outStrVec);

	public:
		static std::string benchPhaseToPhaseName(BenchPhase benchPhase, const ProgArgs* progArgs);
		static std::string benchPhaseToPhaseEntryType(BenchPhase benchPhase,
			bool firstToUpper=false);
		static std::string benchPathTypeToStr(BenchPathType pathType, const ProgArgs* progArgs);
		static std::string stringVecToString(const StringVec& vec, std::string separator);
		static unsigned fadviseArgsStrToFlags(std::string fadviseArgsStr);
		static unsigned madviseArgsStrToFlags(std::string madviseArgsStr);
		static std::string intVectoHumanStr(const IntVec& intVec);
		static bool expandSquareBrackets(StringVec& inoutStrVec);
		static bool replaceCommasOutsideOfSquareBrackets(std::string& inoutStr,
			std::string replacementStr);
		static std::string eraseCommas(const std::string& str);
		static void splitAndExpandStr(std::string str, std::string delimiters,
			StringVec& outVec);
		static void eraseEmptyStringsFromVec(StringVec& inoutVec);

#ifdef S3_SUPPORT
		static void getS3ObjectAclGrants(const ProgArgs* progArgs,
			Aws::Vector<S3::Grant>& outGrants);
		static std::string s3AclPermissionToStr(const S3::Permission& s3Permission);
#endif // S3_SUPPORT

};


#endif /* TOOLKITS_TRANSLATORTK_H_ */
