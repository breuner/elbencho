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
			randRange(randAlgo, offset, offset + len - blockSize),
			numBytesTotal(numBytesTotal), numBytesLeft(numBytesTotal),
			blockSize(blockSize)
		{ }

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
			numBytesTotal = len;
			numBytesLeft = len;

			randRange.reset(offset, offset + len - blockSize);
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
			randRange(randAlgo, 0, (len - blockSize) / blockSize),
			numBytesTotal(numBytesTotal), numBytesLeft(numBytesTotal),
			offset(offset),
			blockSize(blockSize)
		{ }

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
			this->numBytesTotal = len;
			this->numBytesLeft = len;
			this->offset = offset;

			randRange.reset(0, (len - blockSize) / blockSize);
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

#endif /* OFFSETGENERATOR_H_ */
