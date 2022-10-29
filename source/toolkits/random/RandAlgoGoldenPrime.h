#ifndef TOOLKITS_RANDOM_RANDALGOGOLDENPRIME_H_
#define TOOLKITS_RANDOM_RANDALGOGOLDENPRIME_H_

#include <cstdint>
#include <cstring>
#include "RandAlgoInterface.h"
#include "RandAlgoXoshiro256ss.h"

#define RANDALGO_GOLDEN_RATIO_PRIME		0x9e37fffffffc0001UL
#define RANDALGO_GOLDEN_RESEED_SIZE		(256*1024)

/**
 * Fast but weak random number generator based on simple multiplication and shifting of 64bits of
 * state. (The upper 3 bits are always zero in the generated values.)
 *
 * Since the random numbers are weak, this internally uses a stronger random number generator for
 * occasional reseeding on request through reseed() and at the beginning of each new buffer fill
 * via fillBuf().
 */
class RandAlgoGoldenPrime : public RandAlgoInterface
{
	public:
		RandAlgoGoldenPrime()
		{
			state = stateSeeder.next();
		}

		virtual ~RandAlgoGoldenPrime() {}

	private:
		RandAlgoXoshiro256ss stateSeeder; // for occasional reseed
		uint64_t state;

		// inliners
	public:
		virtual uint64_t next() override
		{
			return nextInternal();
		}

		virtual void fillBuf(char* buf, uint64_t bufLen) override
		{
			size_t numBytesDone = 0;
			uint64_t chunkLen;

			// loop to reseed with better random number every RANDALGO_GOLDEN_RESEED_SIZE
			while( (chunkLen = (bufLen - numBytesDone) ) >= (RANDALGO_GOLDEN_RESEED_SIZE) )
			{
				reseed();

				fillBufNoReseed(buf, chunkLen);

				buf += chunkLen;
				numBytesDone += chunkLen;
			}

			// reseed and fill remainder of less than RANDALGO_GOLDEN_RESEED_SIZE
			// (note: this is also relevant if bufLen was smaller than RANDALGO_GOLDEN_RESEED_SIZE)

			reseed();

			fillBufNoReseed(buf, bufLen - numBytesDone);
		}

		/**
		 * Fill buffer without any reseeds from stronger random generator. This is typically only
		 * useful with a limited buffer len and logic to reseed occasionally.
		 */
		void fillBufNoReseed(char* buf, uint64_t bufLen)
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

		/**
		 * Reseed with better random number.
		 */
		void reseed()
		{
			state = stateSeeder.next();
		};

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
			state *= RANDALGO_GOLDEN_RATIO_PRIME;
			state >>= 3;

			return state;
		}
};

#endif /* TOOLKITS_RANDOM_RANDALGOGOLDENPRIME_H_ */
