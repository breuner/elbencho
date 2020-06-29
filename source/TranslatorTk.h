#ifndef TRANSLATORTK_H_
#define TRANSLATORTK_H_

#include <string>
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
		static std::string benchPhaseToPhaseEntryType(BenchPhase benchPhase);
};


#endif /* TRANSLATORTK_H_ */
