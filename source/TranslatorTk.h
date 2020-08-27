#ifndef TRANSLATORTK_H_
#define TRANSLATORTK_H_

#include <string>
#include <vector>
#include "Common.h"

/**
 * A toolkit of static methods to translate from one thing to another.
 */
class TranslatorTk
{
	private:
		TranslatorTk() {}

	public:
		static std::string benchPhaseToPhaseName(BenchPhase benchPhase);
		static std::string benchPhaseToPhaseEntryType(BenchPhase benchPhase,
			bool firstToUpper=false);
		static std::string benchPathTypeToStr(BenchPathType pathType);
		static std::string stringVecToString(const StringVec& vec, std::string separator);
};


#endif /* TRANSLATORTK_H_ */
