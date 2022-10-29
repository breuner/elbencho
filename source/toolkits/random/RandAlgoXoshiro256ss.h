#ifndef TOOLKITS_RANDOM_RANDALGOXOSHIRO256SS_H_
#define TOOLKITS_RANDOM_RANDALGOXOSHIRO256SS_H_

#include <cstdint>
#include <cstring>
#include <random>
#include "RandAlgoInterface.h"

/**
 * xoshiro256ss general purpose random number generator with good balance between speed and
 * randomness.
 *
 * https://en.wikipedia.org/wiki/Xorshift#xoshiro256**
 * base algos and speed tests: https://prng.di.unimi.it/
 */
class RandAlgoXoshiro256ss : public RandAlgoInterface
{
	public:
		RandAlgoXoshiro256ss()
		{
			// init the 4 state values with random numbers
			// (32-shift because random_device returns only 32bit values)
			for(int i=0; i < 4; i++)
				state.s[i] =
					( (uint64_t)std::random_device()() << 32) | (uint32_t)std::random_device()();
		}

		virtual ~RandAlgoXoshiro256ss() {}

	private:
		struct xoshiro256ss_state
		{
			uint64_t s[4];
		} state;

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
			uint64_t *s = state.s;
			uint64_t const result = rol64(s[1] * 5, 7) * 9;
			uint64_t const t = s[1] << 17;

			s[2] ^= s[0];
			s[3] ^= s[1];
			s[1] ^= s[2];
			s[0] ^= s[3];

			s[2] ^= t;
			s[3] = rol64(s[3], 45);

			return result;
		}

		uint64_t rol64(uint64_t x, int k)
		{
			return (x << k) | (x >> (64 - k));
		}
};

#endif /* TOOLKITS_RANDOM_RANDALGOXOSHIRO256SS_H_ */
