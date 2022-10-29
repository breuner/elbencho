#ifndef TOOLKITS_TRANSLATORTK_H_
#define TOOLKITS_TRANSLATORTK_H_

#include <string>
#include <vector>
#include "Common.h"
#include "ProgArgs.h"

/**
 * A toolkit of static methods to translate from one thing to another.
 */
class TranslatorTk
{
	private:
		TranslatorTk() {}

	public:
		static std::string benchPhaseToPhaseName(BenchPhase benchPhase, const ProgArgs* progArgs);
		static std::string benchPhaseToPhaseEntryType(BenchPhase benchPhase,
			bool firstToUpper=false);
		static std::string benchPathTypeToStr(BenchPathType pathType, const ProgArgs* progArgs);
		static std::string stringVecToString(const StringVec& vec, std::string separator);
};


#endif /* TOOLKITS_TRANSLATORTK_H_ */
