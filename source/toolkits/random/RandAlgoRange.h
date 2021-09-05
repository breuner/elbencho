#ifndef TOOLKITS_RANDOM_RANDALGORANGE_H_
#define TOOLKITS_RANDOM_RANDALGORANGE_H_

#include <cstdint>
#include "ProgException.h"
#include "RandAlgoGoldenPrime.h"
#include "RandAlgoMT19937.h"
#include "RandAlgoXoshiro256ss.h"

/**
 * Wrapper for random algo that returns numbers only within a certain range.
 */
class RandAlgoRange
{
	public:
		/**
		 * Note: Max and min are allowed to be the same value, but max may not be smaller than min.
		 *
		 * @min minimum allowed value.
		 * @max maximum allowed value.
		 * @throw ProgException if max < min.
		 */
		RandAlgoRange(RandAlgoInterface& randAlgo, uint64_t min, uint64_t max) :
			randAlgo(randAlgo), start(min), lengthPlusOne(max-min+1)
		{
			if(max < min)
				throw ProgException("RandAlgoRange: \"max < min\" not allowed. "
					"Min: " + std::to_string(min) + "; " + "max: " + std::to_string(max) );
		}

	private:
		RandAlgoInterface& randAlgo;
		uint64_t start;
		uint64_t lengthPlusOne;

		// inliners
	public:
		void reset(uint64_t min, uint64_t max)
		{
			start = min;
			lengthPlusOne = max-min+1;

			if(max < min)
				throw ProgException("RandAlgoRange: \"max < min\" not allowed. "
					"Min: " + std::to_string(min) + "; " + "max: " + std::to_string(max) );
		}

		uint64_t next()
		{
			uint64_t nextVal = randAlgo.next() % lengthPlusOne;
			return nextVal + start;
		}
};

#endif /* TOOLKITS_RANDOM_RANDALGORANGE_H_ */
