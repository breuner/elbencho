#ifndef TOOLKITS_UNITTK_H_
#define TOOLKITS_UNITTK_H_

#include <cstdint>
#include <string>

/**
 * Toolkit to convert units (e.g. bytes to kilobytes).
 */
class UnitTk
{
	public:
		static uint64_t numHumanToBytesBinary(std::string numHuman, bool throwOnEmpty);

	private:
		UnitTk() {}

		// inliners
	public:
		/**
		 * Calculate per second values from total values based on given elapsed time.
		 *
		 * @totalValue total value for which to calc per-sec value based on elapsedUSec.
		 * @elapsedUSec elapsed time as basis for per-sec calculation.
		 * @return per-sec value based on totalValue and elapsedUSec.
		 */
		static uint64_t getPerSecFromUSec(uint64_t totalValue, uint64_t elapsedUsec)
		{
			/* multiplying first by usecs per sec (1M) could easily overflow 64bit, dividing first
			   and afterwards multiplying by 1M would round too much, so use floating. */

			const double numUSecsPerSec = 1000000;

			return totalValue * (numUSecsPerSec / elapsedUsec);
		}

};

#endif /* SOURCE_TOOLKITS_UNITTK_H_ */
