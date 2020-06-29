#ifndef UNITTK_H_
#define UNITTK_H_

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
};

#endif /* SOURCE_UNITTK_H_ */
