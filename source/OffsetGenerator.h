#ifndef OFFSETGENERATOR_H_
#define OFFSETGENERATOR_H_

#include <ctype.h>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "ProgArgs.h"
#include "toolkits/random/RandAlgoRange.h"

#define OFFSETGEN_FULLCOV_PRIME      (2147483647)


/**
 * Generate sequential, random, and random block aligned offsets for LocalWorker's file read/write
 * methods.
 */
class OffsetGenerator
{
	public:
		virtual ~OffsetGenerator() {}

		virtual void reset() = 0; // reset for reuse of object with next file
		virtual void reset(uint64_t len, uint64_t offset) = 0; /* warning: for random generators,
													randAmount is always set to range len */
		virtual uint64_t getNextOffset() = 0;
		virtual size_t getBlockSize() const = 0; // tyically you want getNextBlockSizeToSubmit()
		virtual size_t getNextBlockSizeToSubmit() const = 0;
		virtual uint64_t getNumBytesTotal() const = 0;
		virtual uint64_t getNumBytesLeftToSubmit() const = 0;
		virtual void addBytesSubmitted(size_t numBytes) = 0;

	protected:
		OffsetGenerator() {};
};

/**
 * Generate simple sequential offsets.
 */
class OffsetGenSequential : public OffsetGenerator
{
	public:
		OffsetGenSequential(uint64_t len, uint64_t offset, size_t blockSize) :
			numBytesTotal(len), numBytesLeft(len), startOffset(offset), currentOffset(offset),
			blockSize(blockSize)
		{ }

		virtual ~OffsetGenSequential() {}

	protected:
		uint64_t numBytesTotal;
		uint64_t numBytesLeft;
		uint64_t startOffset;
		uint64_t currentOffset;
		const size_t blockSize;

	// inliners
	public:
		virtual void reset() override
		{
			numBytesLeft = numBytesTotal;
			currentOffset = startOffset;
		}

		virtual void reset(uint64_t len, uint64_t offset) override
		{
			numBytesTotal = len;
			numBytesLeft = len;
			startOffset = offset;
			currentOffset = offset;
		}

		virtual uint64_t getNextOffset() override
			{ return currentOffset; }

		virtual size_t getBlockSize() const override
			{ return blockSize; }

		virtual size_t getNextBlockSizeToSubmit() const override
			{ return std::min(numBytesLeft, blockSize); }

		virtual uint64_t getNumBytesTotal() const override
			{ return numBytesTotal; }

		virtual uint64_t getNumBytesLeftToSubmit() const override
			{ return numBytesLeft; }

		virtual void addBytesSubmitted(size_t numBytes) override
		{
			numBytesLeft -= numBytes;
			currentOffset += numBytes;
		}
};

/**
 * Generate reverse sequential offsets.
 */
class OffsetGenReverseSeq : public OffsetGenerator
{
	public:
		OffsetGenReverseSeq(uint64_t len, uint64_t offset, size_t blockSize) :
			numBytesTotal(len), numBytesLeft(len), startOffset(offset), currentOffset(offset),
			blockSize(blockSize)
		{
			reset();
		}

		virtual ~OffsetGenReverseSeq() {}

	protected:
		uint64_t numBytesTotal;
		uint64_t numBytesLeft;
		uint64_t startOffset;
		uint64_t currentOffset;
		const size_t blockSize;

	// inliners
	public:
		virtual void reset() override
		{
			numBytesLeft = numBytesTotal;

			// avoid division by 0 for blockSize
			IF_UNLIKELY(!numBytesTotal)
			{
				currentOffset = 0;
				return;
			}

			// check if last block is a full block or partial block
			size_t lastBlockRemainder = numBytesTotal % blockSize;

			if(lastBlockRemainder)
				currentOffset = startOffset + numBytesTotal - lastBlockRemainder;
			else
				currentOffset = startOffset + numBytesTotal - blockSize;
		}

		virtual void reset(uint64_t len, uint64_t offset) override
		{
			numBytesTotal = len;
			numBytesLeft = len;
			startOffset = offset;
			currentOffset = offset;

			reset();
		}

		virtual uint64_t getNextOffset() override
			{ return currentOffset; }

		virtual size_t getBlockSize() const override
			{ return blockSize; }

		virtual size_t getNextBlockSizeToSubmit() const override
			{ return std::min(startOffset + numBytesTotal - currentOffset, blockSize); }

		virtual uint64_t getNumBytesTotal() const override
			{ return numBytesTotal; }

		virtual uint64_t getNumBytesLeftToSubmit() const override
			{ return numBytesLeft; }

		virtual void addBytesSubmitted(size_t numBytes) override
		{
			numBytesLeft -= numBytes;
			currentOffset -= blockSize;
		}
};

/**
 * Generate random unaligned offsets.
 *
 * offset and len in constructor define the range in which random offsets are selected. The amount
 * of data is defined by progArgs.getRandomAmount() div progArgs.getNumDataSetThreads().
 */
class OffsetGenRandom : public OffsetGenerator
{
	public:
		OffsetGenRandom(uint64_t numBytesTotal, RandAlgoInterface& randAlgo, uint64_t len,
			uint64_t offset, size_t blockSize) :
			randRange(randAlgo, offset, offset + len - std::min(blockSize, len) ),
			numBytesTotal(numBytesTotal), numBytesLeft(numBytesTotal),
			blockSize(blockSize)
		{
			/* note on "std::min(blockSize, len)": usually blockSize, but there are cases where we
				have custom tree slices that are smaller than blockSize */
		}

		virtual ~OffsetGenRandom() {}

	protected:
		RandAlgoRange randRange;

		uint64_t numBytesTotal;
		uint64_t numBytesLeft;
		const size_t blockSize;

	// inliners
	public:
		virtual void reset() override
			{ numBytesLeft = numBytesTotal; }

		virtual void reset(uint64_t len, uint64_t offset) override
		{
			size_t minLenAndBlockSize = std::min(blockSize, len); /* usually blockSize, but there
				are cases where we have custom tree slices that are smaller than blockSize */

			numBytesTotal = len;
			numBytesLeft = len;

			randRange.reset(offset, offset + len - minLenAndBlockSize);
		}

		virtual uint64_t getNextOffset() override
			{ return randRange.next(); }

		virtual size_t getBlockSize() const override
			{ return blockSize; }

		virtual size_t getNextBlockSizeToSubmit() const override
			{ return std::min(numBytesLeft, blockSize); }

		virtual uint64_t getNumBytesTotal() const override
			{ return numBytesTotal; }

		virtual uint64_t getNumBytesLeftToSubmit() const override
			{ return numBytesLeft; }

		virtual void addBytesSubmitted(size_t numBytes) override
			{ numBytesLeft -= numBytes; }
};

/**
 * Generate random offsets aligned to block size.
 *
 * offset and len in constructor define the range in which random offsets are selected. The amount
 * of data is defined by progArgs.getRandomAmount() div progArgs.getNumDataSetThreads().
 *
 * It's possible that the last IO is a partial block. Caller is responsible for setting
 * randomAmount to prevent this, if desired.
 */
class OffsetGenRandomAligned : public OffsetGenerator
{
	public:
		OffsetGenRandomAligned(uint64_t numBytesTotal, RandAlgoInterface& randAlgo, uint64_t len,
			uint64_t offset, size_t blockSize) :
			randRange(randAlgo, 0,
			    !std::min(blockSize, len) ? 0 : /* avoid div by zero */
			        (len - std::min(blockSize, len) ) / std::min(blockSize, len) ),
			numBytesTotal(numBytesTotal), numBytesLeft(numBytesTotal),
			offset(offset),
			blockSize(blockSize)
		{
			/* note on "std::min(blockSize, len)": usually blockSize, but there are cases where we
				have custom tree slices that are smaller than blockSize */
		}

		virtual ~OffsetGenRandomAligned() {}

	protected:
		RandAlgoRange randRange;

		uint64_t numBytesTotal;
		uint64_t numBytesLeft;
		uint64_t offset;
		const size_t blockSize;

		// inliners
	public:
		virtual void reset() override
			{ numBytesLeft = numBytesTotal; }

		virtual void reset(uint64_t len, uint64_t offset) override
		{
			size_t minLenAndBlockSize = std::min(blockSize, len); /* usually blockSize, but there
				are cases where we have custom tree slices that are smaller than blockSize */

			this->numBytesTotal = len;
			this->numBytesLeft = len;
			this->offset = offset;

			if(minLenAndBlockSize)
			    randRange.reset(0, (len - minLenAndBlockSize) / minLenAndBlockSize);
			else
			    randRange.reset(0, 0); // avoid div by zero
		}

		virtual uint64_t getNextOffset() override
			{ return offset + (randRange.next() * blockSize); }

		virtual size_t getBlockSize() const override
			{ return blockSize; }

		virtual size_t getNextBlockSizeToSubmit() const override
			{ return std::min(numBytesLeft, blockSize); }

		virtual uint64_t getNumBytesTotal() const override
			{ return numBytesTotal; }

		virtual uint64_t getNumBytesLeftToSubmit() const override
			{ return numBytesLeft; }

		virtual void addBytesSubmitted(size_t numBytes) override
			{ numBytesLeft -= numBytes; }

};

/**
 * Generate random offsets aligned to block size for maximum file blocks coverage.
 *
 * This random generator is specifically built to provide full coverage of all block offsets within
 * a file. For that we multiply file indices by a special prime number.
 *
 * The seed here corresponds to the start offset within a file. Thus, if muliple workers do random
 * writes across the same file then the seed of a worker needs to be its rank index multiplied by
 * the number of blocks that each worker writes to ensure full coverage.
 *
 * Other random number generators typically only cover around 70% of the file blocks in a single
 * pass, which makes them less appropriate for cases where the file should also be read after
 * a single random write pass, because of the resulting amount of holes in a file.
 *
 * offset and len in constructor define the range in which random offsets are selected. Only full
 * blocks are taken into account within the given range.
 *
 * It's possible that the last IO is a partial block. Caller is responsible for setting
 * randomAmount to prevent this, if desired.
 */
class OffsetGenRandomAlignedFullCoverage : public OffsetGenerator
{
    public:
        OffsetGenRandomAlignedFullCoverage(uint64_t numBytesTotal,
            uint64_t len, uint64_t offset, size_t blockSize) :
            numBytesTotal(numBytesTotal), numBytesLeft(numBytesTotal),
            rangeOffset(offset), rangeLen(len), blockSize(blockSize)
        {
            initVirtualBlockIndex();
        }

        virtual ~OffsetGenRandomAlignedFullCoverage() {}

    private:
        uint64_t numBytesTotal;
        uint64_t numBytesLeft;
        uint64_t rangeOffset; // byte offset from beginning of file
        uint64_t rangeLen; // byte length of range to randomly seek in from given offset
        uint64_t virtualBlockIndex; // sequentially inc'ed to calculate next random block offset
        uint64_t numBlocksInRange; // number of blocks that can be randomly hit

        const size_t blockSize;

        // inliners
    public:
        virtual void reset() override
        {
            numBytesLeft = numBytesTotal;

            initVirtualBlockIndex();
        }

        /**
         * Note: This sets numBytesTotal equal to len, so might not appropriate for cases of shared
         * overlapping file writes where each thread possibly only writes a partial amount of the
         * file size.
         */
        virtual void reset(uint64_t len, uint64_t offset) override
        {
            this->numBytesTotal = len;
            this->numBytesLeft = len;
            this->rangeOffset = offset;
            this->rangeLen = len;

            initVirtualBlockIndex();
        }

        virtual uint64_t getNextOffset() override
        {
            uint64_t nextRandBlockIdx =
                (virtualBlockIndex * OFFSETGEN_FULLCOV_PRIME) % numBlocksInRange;

            uint64_t nextOffsetRes = rangeOffset + (nextRandBlockIdx * blockSize);

            virtualBlockIndex++;

            return nextOffsetRes;
        }

        virtual size_t getBlockSize() const override
            { return blockSize; }

        virtual size_t getNextBlockSizeToSubmit() const override
            { return std::min(numBytesLeft, blockSize); }

        virtual uint64_t getNumBytesTotal() const override
            { return numBytesTotal; }

        virtual uint64_t getNumBytesLeftToSubmit() const override
            { return numBytesLeft; }

        virtual void addBytesSubmitted(size_t numBytes) override
            { numBytesLeft -= numBytes; }


    private:
        void initVirtualBlockIndex()
        {
            IF_UNLIKELY(!blockSize)
            { // happens for empty files
                numBlocksInRange = 0;
                virtualBlockIndex = 0;
                return;
            }

            numBlocksInRange = rangeLen / blockSize;

            if(!numBlocksInRange)
                numBlocksInRange = 1; // to avoid div by zero

            /* note: this is key to ensure full coverage. we sequentially inc the virtual index
                and the multiplication with the special prime modulo the number of blocks results
                in full pseudo-random coverage of all blocks. */
            virtualBlockIndex = 0;
        }
};


#endif /* OFFSETGENERATOR_H_ */
