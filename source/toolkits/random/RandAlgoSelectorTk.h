#ifndef TOOLKITS_RANDALGOSELECTORTK_H_
#define TOOLKITS_RANDALGOSELECTORTK_H_

#include <memory>
#include <string>
#include "RandAlgoInterface.h"

#define RANDALGO_STRONG_STR					"strong" // RandAlgo_MT19937
#define RANDALGO_BALANCED_SEQUENTIAL_STR	"balanced_single" // RandAlgo_XOSHIRO256SS
#define RANDALGO_BALANCED_SIMD_STR			"balanced" // RandAlgo_XOSHIRO256PP8XSIMD
#define RANDALGO_FAST_STR					"fast" // RandAlgo_GOLDENRATIOPRIME

enum RandAlgoType
{
	RandAlgo_INVALID = 0, // invalid/unknown
	RandAlgo_MT19937 = 1, // slower but high quality randomness
	RandAlgo_XOSHIRO256SS = 2, // balanced speed and quality (no SIMD)
	RandAlgo_XOSHIRO256PP8XSIMD = 3, // balanced speed and quality (8-way SIMD)
	RandAlgo_GOLDENRATIOPRIME = 4, // fast but lower quality
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
