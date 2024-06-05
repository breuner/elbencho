#include "workers/WorkerException.h"
#include "StringTk.h"
#include "toolkits/random/RandAlgoGoldenPrime.h"


/**
 * Generates a string of given length consisting of random hex characters.
 *
 * @strLen length of the randomized string to generate.
 * @return string of given length consisting of random hex characters.
 */
std::string StringTk::generateRandomHexString(unsigned strLen)
{
	std::string outStr;
	RandAlgoGoldenPrime randGen;

	/* note on performance: if this would ever become performance critical then we could optimize
		this to get multiple random chars out of a single randGen value by dividing by
		HEX_ALPHABET_LEN each time. */

	for(unsigned i = 0; i < strLen; i++)
		outStr += ( (char*)HEX_ALPHABET)[randGen.next() % HEX_ALPHABET_LEN];

	return outStr;
}

/**
 * Generates a string checksum of given length consisting of hex characters from the given input
 * string.
 *
 * @str input string for which the checksum should be calculated.
 * @checkSumLen length of the checksum string that should be returned; reasonable values depend on
 * 		the input string length, but range 1 to 4 usually is a good choice.
 */
std::string StringTk::generateChecksumString(std::string str, unsigned checksumLen)
{
	unsigned checkSum = 0;

	// sum up the integer values of all string chars
	for(unsigned i = 0; i < str.length(); i++)
		checkSum += (unsigned)str[i];

	std::string checksumStr;

	checksumStr.reserve(checksumLen);

	for(unsigned i = 0; i < checksumLen; i++)
	{
		checksumStr += ( (char*)HEX_ALPHABET)[checkSum % HEX_ALPHABET_LEN];

		checkSum /= HEX_ALPHABET_LEN;
	}

	return checksumStr;
}

/**
 * Verify that a given string matches the given checksum. This is for checksum strings that were
 * generated via generateChecksumString().
 *
 * @str the string to verify to match the given checksum.
 * @checksum the checksum to verify against the given string.
 * @return true if the string matches the given checksum, false otherwise.
 */
bool StringTk::verifyChecksumString(std::string str, std::string checksumStr)
{
	std::string newChecksumStr = generateChecksumString(str, checksumStr.length() );

	return newChecksumStr == checksumStr;
}

/**
 * Generate a random S3 tag value of given length and with given checksum length. The checksum
 * value also includes the name of the object, so can only be validated later against the same
 * object name.
 *
 * @objectName name of the object to which this tag will get assigned.
 * @strLen length of the random S3 tag value including the length of the checksum.
 * @return random S3 tag value with checksum string appended.
 *
 * @throw WorkerException on error.
 */
std::string StringTk::generateRandomS3TagValue(std::string objectName, unsigned strLen,
	unsigned checksumLen)
{
	IF_UNLIKELY(strLen <= checksumLen)
		throw WorkerException("generateRandomS3Tag() was given a strLen that is not larger than "
			"checksumLen");

	std::string randomStr = generateRandomHexString(strLen - checksumLen);

	std::string checksumStr = generateChecksumString(objectName + randomStr, checksumLen);

	return (randomStr + checksumStr);
}

/**
 * Verify a random string including checksum that was generated via generateRandomS3Tag().
 *
 * @objectName the object name that was used to generate the random string.
 * @checkumLen the length of the checksum that was used for generateRandomS3Tag().
 * @return true if the checksum matches, false otherwise.
 */
bool StringTk::verifyRandomS3TagValue(std::string s3TagValue, std::string objectName,
	unsigned checksumLen)
{
	IF_UNLIKELY(s3TagValue.length() <= checksumLen)
		return false; // verification fails if tag is not longer than checksum

	std::string checksumStr = s3TagValue.substr(s3TagValue.length() - checksumLen);

	std::string tagValueWithoutChecksum = s3TagValue.substr(0, s3TagValue.length() - checksumLen);

	std::string newChecksumStr = generateChecksumString(objectName + tagValueWithoutChecksum,
        checksumLen);

	return (newChecksumStr == checksumStr);
}
