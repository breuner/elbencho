#ifndef TOOLKITS_RANDOM_RANDALGOXOSHIRO256PPSIMD_H_
#define TOOLKITS_RANDOM_RANDALGOXOSHIRO256PPSIMD_H_

#include <cstdint>
#include <cstring>
#include <random>
#include "RandAlgoInterface.h"

/**
 * Optimized N-way SIMD xoshiro256++ 64-bit PRNG. General purpose random number generator with good
 * balance between speed and randomness.
 *
 * source: https://gist.github.com/CaffeineViking/dbb43bf2c388646629cb0d786195f4b7
 * base algos and speed tests: https://prng.di.unimi.it/
 *
 * @Nway N-way SIMD for random numbers, typically 8.
 */
template<int Nway=4>
class RandAlgoXoshiro256ppSIMD : public RandAlgoInterface
{
	public:
		RandAlgoXoshiro256ppSIMD()
		{
			// initialize N-way state

			// (32-shift because random_device returns only 32bit values)
			for (int i = 0; i < 4; i++)
				for (int j = 0; j < Nway; j++)
				state.s[i][j] =
					( (uint64_t) std::random_device()() << 32) | (uint32_t) std::random_device()();
		}

		virtual ~RandAlgoXoshiro256ppSIMD() {}

	private:
		struct xoshiro_simd_state
		{
			uint64_t s[4][Nway]; /* 1st dimension is the actual state (4x64bit), 2nd dim is the
									number of different states to be SIMD optimized */
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

			// N-way SIMD
			for(uint64_t i=0; i < (bufLen / (sizeof(uint64_t)*Nway) ); i++)
			{
				uint64_t* uint64Buf = (uint64_t*)buf;
				nextInternalNway<Nway>(uint64Buf);

				buf += (sizeof(uint64_t) * Nway);
				numBytesDone += (sizeof(uint64_t) * Nway);
			}

			if(numBytesDone == bufLen)
				return; // all done, complete buffer filled

			// 64bit numbers remainder without SIMD
			for(uint64_t i=(numBytesDone / sizeof(uint64_t) );
				i < (bufLen / sizeof(uint64_t) );
				i++)
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
		 *
		 * @NwayInternal n-way SIMD for random numbers, typically 8 (and 1 for remainder). This must
		 * 		not be larger than class template arg Nway.
		 */
		template<int NwayInternal>
		void nextInternalNway(uint64_t* r)
		{
			// optimized to use N-way SIMD instructions

		    uint64_t x[NwayInternal];
		    uint64_t t[NwayInternal];

		    for (int i = 0; i < NwayInternal; ++i)
		    	x[i] = state.s[0][i] + state.s[3][i];
		    for (int i = 0; i < NwayInternal; ++i)
		    	r[i] = ((x[i] << 23) | (x[i] >> 41)) + state.s[0][i];
		    for (int i = 0; i < NwayInternal; ++i)
		    	t[i] = state.s[1][i] << 17;

		    for (int i = 0; i < NwayInternal; ++i)
		    	state.s[2][i] ^= state.s[0][i];
		    for (int i = 0; i < NwayInternal; ++i)
		    	state.s[3][i] ^= state.s[1][i];
		    for (int i = 0; i < NwayInternal; ++i)
		    	state.s[1][i] ^= state.s[2][i];
		    for (int i = 0; i < NwayInternal; ++i)
		    	state.s[0][i] ^= state.s[3][i];
		    for (int i = 0; i < NwayInternal; ++i)
		    	state.s[2][i] ^= t[i];

		    for (int i = 0; i < NwayInternal; ++i)
		    	state.s[3][i]  = (state.s[3][i] << 45) | (state.s[3][i] >> 19);
		}

		uint64_t nextInternal()
		{
			uint64_t tmp;

			nextInternalNway<1>(&tmp);

			return tmp;
		}

		uint64_t rol64(uint64_t x, int k)
		{
			return (x << k) | (x >> (64 - k));
		}
};

#endif /* TOOLKITS_RANDOM_RANDALGOXOSHIRO256PPSIMD_H_ */
