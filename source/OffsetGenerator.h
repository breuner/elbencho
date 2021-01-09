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
		virtual uint64_t getNextOffset() = 0;
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
		const uint64_t numBytesTotal;
		uint64_t numBytesLeft;
		const uint64_t startOffset;
		uint64_t currentOffset;
		const size_t blockSize;

	// inliners
	public:
		virtual void reset() override
		{
			numBytesLeft = numBytesTotal;
			currentOffset = startOffset;
		}

		virtual uint64_t getNextOffset() override
			{ return currentOffset; }

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
 * Generate random unaligned offsets.
 *
 * offset and len in constructor define that range in which random offsets are selected. The amount
 * of data is defined by progArgs.getRandomAmount() div progArgs.getNumDataSetThreads().
 */
class OffsetGenRandom : public OffsetGenerator
{
	public:
		OffsetGenRandom(const ProgArgs& progArgs, RandAlgoInterface& randAlgo, uint64_t len,
			uint64_t offset, size_t blockSize) :
			progArgs(progArgs), randRange(randAlgo, offset, offset + len - blockSize),
			blockSize(blockSize)
		{
			this->numBytesTotal = progArgs.getRandomAmount() / progArgs.getNumDataSetThreads();
			this->numBytesLeft = this->numBytesTotal;
		}

		virtual ~OffsetGenRandom() {}

	protected:
		const ProgArgs& progArgs;
		RandAlgoRange randRange;

		uint64_t numBytesTotal;
		uint64_t numBytesLeft;
		const size_t blockSize;

	// inliners
	public:
		virtual void reset() override
		{ numBytesLeft = numBytesTotal; }

		virtual uint64_t getNextOffset() override
			{ return randRange.next(); }

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
 * offset and len in constructor define that range in which random offsets are selected. The amount
 * of data is defined by progArgs.getRandomAmount() div progArgs.getNumDataSetThreads().
 */
class OffsetGenRandomAligned : public OffsetGenerator
{
	public:
		OffsetGenRandomAligned(const ProgArgs& progArgs, RandAlgoInterface& randAlgo, uint64_t len,
				uint64_t offset, size_t blockSize) :
			randRange(randAlgo, 0, (offset + len - blockSize) / blockSize),
			blockSize(blockSize)
		{
			this->numBytesTotal = progArgs.getRandomAmount() / progArgs.getNumDataSetThreads();

			// "- (this->numBytesTotal % blockSize)" ensures that we don't submit partial blocks
			this->numBytesLeft = this->numBytesTotal - (this->numBytesTotal % blockSize);
		}

		virtual ~OffsetGenRandomAligned() {}

	protected:
		RandAlgoRange randRange;

		uint64_t numBytesTotal;
		uint64_t numBytesLeft;
		const size_t blockSize;

		// inliners
	public:
		virtual void reset() override
		{
			// "- (numBytesTotal % blockSize)" ensures that we don't submit partial blocks
			numBytesLeft = numBytesTotal - (numBytesTotal % blockSize);
		}

		virtual uint64_t getNextOffset() override
			{ return randRange.next() * blockSize; }

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
