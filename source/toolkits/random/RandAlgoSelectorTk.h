#ifndef TOOLKITS_RANDALGOSELECTORTK_H_
#define TOOLKITS_RANDALGOSELECTORTK_H_

#include <memory>
#include <string>
#include "RandAlgoInterface.h"

#define RANDALGO_STRONG_STR		"strong" // RandAlgo_MT19937
#define RANDALGO_BALANCED_STR	"balanced" // RandAlgo_XOSHIRO256SS
#define RANDALGO_FAST_STR		"fast" // RandAlgo_GOLDENRATIOPRIME

enum RandAlgoType
{
	RandAlgo_INVALID = 0, // invalid/unknown
	RandAlgo_MT19937 = 1, // slower but high quality randomness
	RandAlgo_XOSHIRO256SS = 2, // balanced speed and quality
	RandAlgo_GOLDENRATIOPRIME = 3, // fast but lower quality
};

/**
 * Simple conversion from string to enum RandAlgo.
 */
class RandAlgoSelectorTk
{
	public:
		static RandAlgoType stringToEnum(std::string algoString);
		static std::unique_ptr<RandAlgoInterface> stringToAlgo(std::string algoString);

	private:
		RandAlgoSelectorTk() {}
};

#endif /* TOOLKITS_RANDALGOSELECTORTK_H_ */
