#include <memory>
#include <string>
#include "ProgException.h"
#include "RandAlgoGoldenPrime.h"
#include "RandAlgoMT19937.h"
#include "RandAlgoSelectorTk.h"

#include "RandAlgoXoshiro256ppSIMD.h"
#include "RandAlgoXoshiro256ss.h"

/**
 * Convert random algo string name to corresponding enum RandAlgo.
 */
RandAlgoType RandAlgoSelectorTk::stringToEnum(std::string algoString)
{
	if(algoString == RANDALGO_STRONG_STR)
		return RandAlgo_MT19937;
	else
	if(algoString == RANDALGO_BALANCED_SIMD_STR)
		return RandAlgo_XOSHIRO256PP8XSIMD;
	else
	if(algoString == RANDALGO_BALANCED_SEQUENTIAL_STR)
		return RandAlgo_XOSHIRO256SS;
	else
	if(algoString == RANDALGO_FAST_STR)
		return RandAlgo_GOLDENRATIOPRIME;

	return RandAlgo_INVALID;
}

/**
 * Generate the random algo object for the given string.
 *
 * @outRandAlog pointer to newly allocated
 * @throw template ProgException if algoString invalid.
 */
std::unique_ptr<RandAlgoInterface> RandAlgoSelectorTk::stringToAlgo(std::string algoString)
{
	if(algoString == RANDALGO_STRONG_STR)
		return std::unique_ptr<RandAlgoInterface>(new RandAlgoMT19937() );
	else
	if(algoString == RANDALGO_BALANCED_SEQUENTIAL_STR)
		return std::unique_ptr<RandAlgoInterface>(new RandAlgoXoshiro256ss() );
	else
	if(algoString == RANDALGO_BALANCED_SIMD_STR)
		return std::unique_ptr<RandAlgoInterface>(new RandAlgoXoshiro256ppSIMD() );
	else
	if(algoString == RANDALGO_FAST_STR)
		return std::unique_ptr<RandAlgoInterface>(new RandAlgoGoldenPrime() );

	throw ProgException("Invalid random algo: " + algoString);
}
