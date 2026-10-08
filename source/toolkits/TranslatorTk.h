// SPDX-FileCopyrightText: 2020-2025 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef TOOLKITS_TRANSLATORTK_H_
#define TOOLKITS_TRANSLATORTK_H_

#include <string>
#include <vector>
#include "Common.h"
#include "ProgArgs.h"


/**
 * A toolkit of static methods to translate from one data structure into another.
 */
class TranslatorTk
{
	private:
		TranslatorTk() {}

		static void expandSquareBracketsStr(std::string rangeStr, StringVec& outStrVec);

	public:
        static std::string benchModeToModeName(BenchMode benchMode);
		static std::string benchPhaseToPhaseName(BenchPhase benchPhase, const ProgArgs* progArgs);
        static std::string benchPhaseToPhaseEntryType(BenchPhase benchPhase,
            const ProgArgs* progArgs, bool firstToUpper=false);
		static std::string benchPathTypeToStr(BenchPathType pathType, const ProgArgs* progArgs);
		static std::string stringVecToString(const StringVec& vec, std::string separator);
		static unsigned fadviseArgsStrToFlags(std::string fadviseArgsStr);
		static unsigned madviseArgsStrToFlags(std::string madviseArgsStr);
        static unsigned short flockArgsStrToType(std::string flockArgsStr);
		static std::string intVecToHumanStr(const IntVec& intVec);
		static bool expandSquareBrackets(StringVec& inoutStrVec);
		static bool replaceCommasOutsideOfSquareBrackets(std::string& inoutStr,
			std::string replacementStr);
		static std::string eraseCommas(const std::string& str);
		static void splitAndExpandStr(std::string str, std::string delimiters,
			StringVec& outVec);
		static void eraseEmptyStringsFromVec(StringVec& inoutVec);
        static void eraseBenchPathPrefixesFromVec(StringVec& inoutVec);
		static const char* httpErrorCodeToHumanStr(unsigned httpErrorCode);

};


#endif /* TOOLKITS_TRANSLATORTK_H_ */
