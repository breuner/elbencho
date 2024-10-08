#include <array>
#include <cctype>
#include <iomanip>
#include <iostream>
#include <sstream>
#include "ProgException.h"
#include "UnitTk.h"

/**
 * Convert a human byte string with unit at the end (e.g. "4K") to bytes.
 *
 * @param throwOnEmpty true to throw an exception if numHuman is empty, false to return 0 in this
 * 		case.
 */
uint64_t UnitTk::numHumanToBytesBinary(std::string numHuman, bool throwOnEmpty)
{
	if(numHuman.empty() )
	{
		if(throwOnEmpty)
			throw ProgException("Unable to parse empty string");
		else
			return 0;
	}

	// make sure number string does not contain "." character
	std::size_t findDotPos = numHuman.find(".");
	if(findDotPos != std::string::npos)
		throw ProgException("Unable to parse number string containing '.' character: " + numHuman);

	// make sure number string does not contain "," character
	std::size_t findCommaPos = numHuman.find(",");
	if(findCommaPos != std::string::npos)
		throw ProgException("Unable to parse number string containing ',' character: " + numHuman);

	// atoll ignores characters at the end
	uint64_t bytesRes = std::atoll(numHuman.c_str() );

	// take last character of given string to look at unit
	char lastChar = numHuman[numHuman.length() - 1];

	// check if last character is a number
	if( (lastChar >= '0') && (lastChar <= '9') )
		return bytesRes; // no unit found at end, so nothing to convert

	switch(std::toupper(lastChar) )
	{
		case 'K':
			return bytesRes * (1ULL << 10);
		case 'M':
			return bytesRes * (1ULL << 20);
		case 'G':
			return bytesRes * (1ULL << 30);
		case 'T':
			return bytesRes * (1ULL << 40);
		case 'P':
			return bytesRes * (1ULL << 50);
		case 'E':
			return bytesRes * (1ULL << 60);

		default: throw ProgException("Unable to parse string for unit conversion: " +
			numHuman);
	}
}

/**
 * Convert microseconds value to a human readable string with unit suffix.
 *
 * Output examples:
 * 123us
 * 1.23ms
 * 12.3ms
 * 123ms
 * 1.23s
 * 12.3s
 * 123s
 */
std::string UnitTk::latencyUsToHumanStr(uint64_t numMicroSec)
{
	if(numMicroSec < 1000) // range <1ms
		return std::to_string(numMicroSec) + "us";
	else
	if(numMicroSec < (10*1000) )
	{ // range 1ms - <10ms
		std::ostringstream resultStream;
		resultStream << std::fixed << std::setprecision(2) <<
			(numMicroSec / double(1000) ) << "ms";
		return resultStream.str();
	}
	else
	if(numMicroSec < (100*1000) )
	{ // range 10ms - <100ms
		std::ostringstream resultStream;
		resultStream << std::fixed << std::setprecision(1) <<
			(numMicroSec / double(1000) ) << "ms";
		return resultStream.str();
	}
	else
	if(numMicroSec < (1*1000*1000) )
	{ // range 100ms - <1000ms
		std::ostringstream resultStream;
		resultStream << std::fixed << std::setprecision(0) <<
			(numMicroSec / double(1000) ) << "ms";
		return resultStream.str();
	}
	else
	if(numMicroSec < (10*1000*1000) )
	{ // range 1s - <10s
		std::ostringstream resultStream;
		resultStream << std::fixed << std::setprecision(2) <<
			(numMicroSec / double(1000000) ) << "s";
		return resultStream.str();
	}
	else
	if(numMicroSec < (100*1000*1000) )
	{ // range 10s - <100s
		std::ostringstream resultStream;
		resultStream << std::fixed << std::setprecision(1) <<
			(numMicroSec / double(1000000) ) << "s";
		return resultStream.str();
	}
	else
	{ // range as of 100s
		std::ostringstream resultStream;
		resultStream << std::fixed << std::setprecision(0) <<
			(numMicroSec / double(1000000) ) << "s";
		return resultStream.str();
	}
}

/**
 * Convert elapsed time in seconds to humann readable string with suffix.
 *
 * Output examples:
 * 12s
 * 2m3s
 * 1h0m0s
 * 3h25m45s
 */
std::string UnitTk::elapsedSecToHumanStr(uint64_t elapsedSec)
{
	size_t numHours = elapsedSec / 3600;
	size_t numMin = (elapsedSec % 3600) / 60;
	size_t numSec = elapsedSec % 60;

	std::ostringstream resultStream;

	if(numHours)
		resultStream << numHours << "h" << numMin << "m" << numSec << "s";
	else
	if(numMin)
		resultStream << numMin << "m" << numSec << "s";
	else
		resultStream << numSec << "s";

	return resultStream.str();
}

/**
 * Convert elapsed time in milliseconds to humann readable string with suffix.
 *
 * Output examples:
 * 1ms
 * 1.001s
 * 2m3.456ms
 * 3h25m45s
 */
std::string UnitTk::elapsedMSToHumanStr(uint64_t elapsedMS)
{
	uint64_t elapsedSec = elapsedMS / 1000;

	size_t numHours = elapsedSec / 3600;
	size_t numMin = (elapsedSec % 3600) / 60;
	size_t numSec = elapsedSec % 60;
	size_t numMS = elapsedMS % 1000;

	std::ostringstream resultStream;

	if(numHours)
		resultStream << numHours << "h" << numMin << "m" << numSec << "s";
	else
	if(numMin)
		resultStream << numMin << "m" << numSec << "." <<
			std::setw(3) << std::setfill('0') << numMS << "s";
	else
	if(numSec)
		resultStream << numSec << "." << std::setw(3) << std::setfill('0') << numMS << "s";
	else
		resultStream << numMS << "ms";

	return resultStream.str();
}

/**
 * Convert a number to a string with base10 unit (e.g. K, M, G etc).
 *
 * @number the number to be converted.
 * @maxLen maximum string length to be returned; must be at least 4 (for 999 with the unit suffix).
 * @maxNumDecimalPlaces maxumum decimal places; can be 0; only makes sense for maxLen >= 6.
 * @return number with unit suffix if original number was longer than maxLen.
 */
std::string UnitTk::numToHumanStrBaseAny(const UnitPair baseAnyUnits[],
    const unsigned short numUnits, uint64_t number, unsigned short maxLen,
    unsigned maxNumDecimalPlaces)
{
    // fast path for numbers that fit already within maxLen

    std::string retVal = std::to_string(number);

    if(retVal.length() <= maxLen)
        return retVal;

    // number is large enough to require scaling...

    unsigned unitIndex = 0;
    short diffToMaxLen;

    do
    {
        uint64_t scaledNumber = number / baseAnyUnits[unitIndex].scaleFactor;

        retVal = std::to_string(scaledNumber);

        diffToMaxLen = (maxLen - 1) - retVal.length(); // (-1 for unit char)

        if(diffToMaxLen >= 0)
            break;

        unitIndex++;

    } while(unitIndex < numUnits);

    if(unitIndex >= numUnits)
        unitIndex = (numUnits-1);

    int numDecimalPlaces =
        std::min(diffToMaxLen - 1, (int)maxNumDecimalPlaces); // -1 for decimal separator dot

    if(numDecimalPlaces > 0)
    { // we can affort a decimal place
        std::ostringstream decimalStream;

        decimalStream << std::setprecision(numDecimalPlaces) << std::fixed <<
            (double)number / baseAnyUnits[unitIndex].scaleFactor;

        retVal = decimalStream.str();

        // remove trailing zeros after decimal point
        while( (retVal.back() == '0') || (retVal.back() == '.') )
        {
            if(retVal.back() == '.')
            { // decimal point, so we have to stop here
                retVal.pop_back();
                break;
            }

            // not the decimal point yet, so we can continue after removing last digit
            retVal.pop_back();
        }
    }

    retVal += baseAnyUnits[unitIndex].unitSuffix;

    return retVal;
}
/**
 * Convert a number to a string with base10 unit (e.g. K, M, G etc).
 *
 * @number the number to be converted.
 * @maxLen maximum string length to be returned; must be at least 4 (for 999 with the unit suffix).
 * @maxNumDecimalPlaces maxumum decimal places; can be 0; only makes sense for maxLen >= 6.
 * @return number with unit suffix if original number was longer than maxLen.
 */
std::string UnitTk::numToHumanStrBase10(uint64_t number, unsigned short maxLen,
    unsigned maxNumDecimalPlaces)
{
    const UnitPair base10Units[] =
    {
        { UINT64_C(1000), "K" },
        { UINT64_C(1000000), "M" },
        { UINT64_C(1000000000), "G" },
        { UINT64_C(1000000000000), "T" },
        { UINT64_C(1000000000000000), "P" },
        { UINT64_C(1000000000000000000), "E" }
    };

    const unsigned short numUnits = sizeof(base10Units) / sizeof(base10Units[0] );

    return numToHumanStrBaseAny(base10Units, numUnits, number, maxLen, maxNumDecimalPlaces);
}

/**
 * Convert a number to a string with base2 unit (e.g. K, M, G etc).
 *
 * @number the number to be converted.
 * @maxLen maximum string length to be returned; must be at least 4 (for 999 with the unit suffix).
 * @maxNumDecimalPlaces maxumum decimal places; can be 0; only makes sense for maxLen >= 6.
 * @return number with unit suffix if original number was longer than maxLen.
 */
std::string UnitTk::numToHumanStrBase2(uint64_t number, unsigned short maxLen,
    unsigned maxNumDecimalPlaces)
{
    const UnitPair base2Units[] =
    {
        { UINT64_C(1) << 10, "K" },
        { UINT64_C(1) << 20, "M" },
        { UINT64_C(1) << 30, "G" },
        { UINT64_C(1) << 40, "T" },
        { UINT64_C(1) << 50, "P" },
        { UINT64_C(1) << 60, "E" }
    };

    const unsigned short numUnits = sizeof(base2Units) / sizeof(base2Units[0] );

    return numToHumanStrBaseAny(base2Units, numUnits, number, maxLen, maxNumDecimalPlaces);
}
