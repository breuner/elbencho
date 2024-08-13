#ifndef TOOLKITS_UNITTK_H_
#define TOOLKITS_UNITTK_H_

#include <cstdint>
#include <string>


/**
 * Toolkit to convert units (e.g. bytes to kilobytes).
 */
class UnitTk
{
    struct UnitPair
    {
        uint64_t scaleFactor;
        std::string unitSuffix;
    };

	public:
		static uint64_t numHumanToBytesBinary(std::string numHuman, bool throwOnEmpty);
		static std::string latencyUsToHumanStr(uint64_t numMicroSec);
		static std::string elapsedSecToHumanStr(uint64_t elapsedSec);
		static std::string elapsedMSToHumanStr(uint64_t elapsedMS);
        static std::string numToHumanStrBase10(uint64_t number, unsigned short maxLen=6,
            unsigned maxNumDecimalPlaces=1);
        static std::string numToHumanStrBase2(uint64_t number, unsigned short maxLen=6,
            unsigned maxNumDecimalPlaces=1);

	private:
		UnitTk() {}

        static std::string numToHumanStrBaseAny(const UnitPair baseAnyUnits[],
            const unsigned short numUnits, uint64_t number, unsigned short maxLen,
            unsigned maxNumDecimalPlaces);

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
