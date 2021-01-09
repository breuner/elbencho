#ifndef TOOLKITS_RANDOM_RANDALGOMT19937_H_
#define TOOLKITS_RANDOM_RANDALGOMT19937_H_

#include <cstdint>
#include <cstring>
#include <random>
#include "RandAlgoInterface.h"

/**
 * Slower but strong random number generator based on Mersenne Twister with 19937 bits of state.
 */
class RandAlgoMT19937 : public RandAlgoInterface
{
	public:
		RandAlgoMT19937() {}
		virtual ~RandAlgoMT19937() {}

	private:
		std::mt19937_64 randGen{std::random_device()() };

		// inliners
	public:
		virtual uint64_t next() override
		{
			return nextInternal();
		}

		virtual void fillBuf(char* buf, uint64_t bufLen) override
		{
			size_t numBytesDone = 0;

			for(uint64_t i=0; i < (bufLen / sizeof(uint64_t) ); i++)
			{
				uint64_t* uint64Buf = (uint64_t*)buf;
				*uint64Buf = nextInternal();

				buf += sizeof(uint64_t);
				numBytesDone += sizeof(uint64_t);
			}

			if(numBytesDone == bufLen)
				return; // all done, complete buffer filled

			// we have a remainder to fill, which can only be smaller than sizeof(uint64_t)
			uint64_t randUint64 = nextInternal();

			memcpy(buf, &randUint64, bufLen - numBytesDone);
		}

		// inliners
	private:
		/**
		 * Generate the next random number.
		 *
		 * Note: This code exists in a spearate function to avoid the virtual function call overhead
		 * of next() for fillBuf().
		 */
		uint64_t nextInternal()
		{
			return randGen();
		}
};

#endif /* TOOLKITS_RANDOM_RANDALGOMT19937_H_ */
