#ifndef FILEOFFSETGENERATOR_H_
#define FILEOFFSETGENERATOR_H_

#include <ctype.h>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "ProgArgs.h"

/**
 * Generate sequential, random, and random block aligned offsets for LocalWorker's file read/write
 * methods.
 */
class FileOffsetGenerator
{
	public:
		virtual ~FileOffsetGenerator() {}

		virtual void reset() = 0; // reset for reuse of object with next file
		virtual size_t getNextOffset() = 0;
		virtual size_t getNextBlockSizeToSubmit() const = 0;
		virtual size_t getNumBytesTotal() const = 0;
		virtual size_t getNumBytesLeftToSubmit() const = 0;
		virtual void addBytesSubmitted(size_t numBytes) = 0;

	protected:
		FileOffsetGenerator() {};

	protected:
};

/**
 * Generate simple sequential offsets.
 */
class FileOffsetGenSequential : public FileOffsetGenerator
{
	public:
		FileOffsetGenSequential(size_t len, size_t offset, size_t blockSize) :
			numBytesTotal(len), numBytesLeft(len), startOffset(offset), currentOffset(offset),
			blockSize(blockSize)
		{ }

		virtual ~FileOffsetGenSequential() {}

	protected:
		const size_t numBytesTotal;
		size_t numBytesLeft;
		const size_t startOffset;
		size_t currentOffset;
		const size_t blockSize;

	// inliners
	public:
		virtual void reset() override
		{
			numBytesLeft = numBytesTotal;
			currentOffset = startOffset;
		}

		virtual size_t getNextOffset() override
			{ return currentOffset; }

		virtual size_t getNextBlockSizeToSubmit() const override
			{ return std::min(numBytesLeft, blockSize); }

		virtual size_t getNumBytesTotal() const override
			{ return numBytesTotal; }

		virtual size_t getNumBytesLeftToSubmit() const override
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
class FileOffsetGenRandom : public FileOffsetGenerator
{
	public:
		FileOffsetGenRandom(const ProgArgs& progArgs, std::mt19937_64& randGen, size_t len,
			size_t offset, size_t blockSize) :
			progArgs(progArgs), randGen(randGen), randDist(offset, offset + len - blockSize),
			blockSize(blockSize)
		{
			this->numBytesTotal = progArgs.getRandomAmount() / progArgs.getNumDataSetThreads();
			this->numBytesLeft = this->numBytesTotal;
		}

		virtual ~FileOffsetGenRandom() {}

	protected:
		const ProgArgs& progArgs;
		std::mt19937_64& randGen;
		std::uniform_int_distribution<uint64_t> randDist;

		size_t numBytesTotal;
		size_t numBytesLeft;
		const size_t blockSize;

	// inliners
	public:
		virtual void reset() override
		{ numBytesLeft = numBytesTotal; }

		virtual size_t getNextOffset() override
			{ return randDist(randGen); }

		virtual size_t getNextBlockSizeToSubmit() const override
			{ return std::min(numBytesLeft, blockSize); }

		virtual size_t getNumBytesTotal() const override
			{ return numBytesTotal; }

		virtual size_t getNumBytesLeftToSubmit() const override
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
class FileOffsetGenRandomAligned : public FileOffsetGenerator
{
	public:
		FileOffsetGenRandomAligned(const ProgArgs& progArgs, std::mt19937_64& randGen, size_t len,
			size_t offset, size_t blockSize) :
			randGen(randGen), randDist(0, (offset + len - blockSize) / blockSize),
			blockSize(blockSize)
		{
			this->numBytesTotal = progArgs.getRandomAmount() / progArgs.getNumDataSetThreads();

			// "- (this->numBytesTotal % blockSize)" ensures that we don't submit partial blocks
			this->numBytesLeft = this->numBytesTotal - (this->numBytesTotal % blockSize);
		}

		virtual ~FileOffsetGenRandomAligned() {}

	protected:
		std::mt19937_64& randGen;
		std::uniform_int_distribution<uint64_t> randDist;

		size_t numBytesTotal;
		size_t numBytesLeft;
		const size_t blockSize;

		// inliners
	public:
		virtual void reset() override
		{
			// "- (numBytesTotal % blockSize)" ensures that we don't submit partial blocks
			numBytesLeft = numBytesTotal - (numBytesTotal % blockSize);
		}

		virtual size_t getNextOffset() override
			{ return randDist(randGen) * blockSize; }

		virtual size_t getNextBlockSizeToSubmit() const override
			{ return std::min(numBytesLeft, blockSize); }

		virtual size_t getNumBytesTotal() const override
			{ return numBytesTotal; }

		virtual size_t getNumBytesLeftToSubmit() const override
			{ return numBytesLeft; }

		virtual void addBytesSubmitted(size_t numBytes) override
			{ numBytesLeft -= numBytes; }

};

#endif /* FILEOFFSETGENERATOR_H_ */
