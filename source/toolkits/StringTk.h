#ifndef TOOLKITS_STRINGTK_H_
#define TOOLKITS_STRINGTK_H_

#include "Common.h"

#define HEX_ALPHABET					"0123456789ABCDEF"
#define HEX_ALPHABET_LEN				(sizeof(HEX_ALPHABET) - 1)


/**
 * A toolkit of static helper functions for string generation and string manipulation.
 */
class StringTk
{
	public:
		static std::string generateRandomHexString(unsigned strLen);
		static std::string generateChecksumString(std::string str, unsigned checksumLen);
		static bool verifyChecksumString(std::string str, std::string checksumStr);

		static std::string generateRandomS3TagValue(std::string objectName, unsigned strLen,
			unsigned checksumLen=2);
		static bool verifyRandomS3TagValue(std::string s3TagValue, std::string objectName,
			unsigned checksumLen=2);

	private:
		StringTk() {}
};



#endif /* TOOLKITS_STRINGTK_H_ */
