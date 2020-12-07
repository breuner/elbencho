#include <cctype>
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
