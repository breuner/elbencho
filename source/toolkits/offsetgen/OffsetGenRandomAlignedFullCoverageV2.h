// SPDX-FileCopyrightText: 2020-2025 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "OffsetGenerator.h"

/**
 * Only for indirect use via OffsetGenRandomAlignedFullCoverageV2.
 */
class CoveringRandomGenerator
{
    private:
        uint64_t min_value_;
        uint64_t max_value_;
        uint64_t range_size_;
        uint64_t current_count_; // How many numbers have been generated in the current cycle

        // LCG parameters and state
        uint64_t m_lcg_; // Modulus for LCG (a power of 2 >= range_size_)
        uint64_t a_lcg_; // Multiplier
        uint64_t c_lcg_; // Increment
        uint64_t current_lcg_state_; // Current X_n for LCG

    public:
        CoveringRandomGenerator(uint64_t min_val, uint64_t max_val) :
            min_value_(min_val), max_value_(max_val), current_count_(0)
        {
            IF_UNLIKELY(min_val > max_val)
            {
                throw WorkerException(
                    "OffsetGenFullCov: Minimum value cannot be greater than maximum value.");
            }

            range_size_ = (max_val - min_val) + 1;

            IF_UNLIKELY(range_size_ == 0)
            { // Should be caught by min_val > max_val, but defensively
                throw WorkerException("OffsetGenFullCov: Range cannot be empty.");
            }

            // Choose LCG parameters for a power-of-2 modulus
            // m = next power of 2 >= range_size_
            // a = 4*k + 1 (e.g., 6364136223846793005ULL for 64-bit, a common one)
            // c = odd (e.g., 1442695040888963407ULL or simply 1 if m allows)

            m_lcg_ = next_power_of_two(range_size_);

            if (m_lcg_ == 0 && range_size_ > 0)
            { // Overflow for next_power_of_two if range_size_ is too large
                // This happens if range_size_ > 2^63 for uint64_t.
                // If range_size_ is exactly 2^64 (max_value_ - min_value_ + 1 = 0 due to overflow
                // for uint64_t) this needs careful handling. For simplicity, assume range_size_
                // fits uint64_t and m_lcg_ doesn't overflow.
                // Or, if range_size_ is already a power of two, m_lcg_ will be range_size_.
                if (range_size_ > (1ULL << 63))
                { // A large power of 2
                    if ( (range_size_ & (range_size_ - 1)) == 0 )
                    { // is power of 2
                        m_lcg_ = range_size_;
                    }
                    else
                    {
                        // This case implies range_size_ is between 2^63 and 2^64-1, and next_power_of_two would overflow.
                        // For such huge ranges, this LCG approach might hit limitations or require 128-bit arithmetic.
                        // For now, we'll assume m_lcg_ fits in uint64_t.
                        // A practical limit might be ranges up to 2^63.
                        throw WorkerException(
                            "Range size too large for this LCG implementation with 64-bit modulus "
                            "selection.");
                    }
                }
                else
                {
                    // This case should not be reached if next_power_of_two is correct.
                    m_lcg_ = range_size_; // Fallback if something unexpected happened, may not
                                            // provide full cycle
                }
            }


            // LCG: X_n+1 = (a * X_n + c) mod m
            // For full period m (a power of 2):
            // c must be odd.
            // a must be of the form 4k+1.
            a_lcg_ = 6364136223846793005ULL; // A common 64-bit LCG multiplier (Knuth MMIX)
            c_lcg_ = 1442695040888963407ULL; // A common 64-bit LCG increment (must be odd)
                                            // Simpler: c_lcg_ = 1 works if m_lcg_ is a power of 2.
                                            // We'll use a large odd.

            // Seed the LCG's initial state (x0_lcg_)
            // We want the LCG to permute 0 to m_lcg_ - 1.
            // The seed can be any number. For reproducibility for a given generator instance,
            // we could make it fixed or allow user to seed. For "randomness" between
            // different runs of the program, use random_device.
            std::random_device rd;

            current_lcg_state_ = rd(); // Initial state for the LCG
            // Ensure seed is within [0, m_lcg_ - 1] if that's a requirement for the first value,
            // though LCG will wrap around anyway.
            current_lcg_state_ %= m_lcg_;

            // The state for our generator is how many numbers we've generated.
            // The LCG state itself evolves.
        }

        // Returns true if more numbers are available in the current cycle
        bool has_next() const
        {
            return current_count_ < range_size_;
        }

        // Generates the next unique random number in the range.
        // Throws std::runtime_error if all numbers in the range have been generated.
        uint64_t next()
        {
            if(!has_next() )
                reset(); // all numbers in the range have been generated

            uint64_t generated_val_in_lcg_range;
            // This loop implements "cycle walking" or rejection sampling.
            // It ensures that the output, when mapped to 0..range_size_-1, is unique
            // for each call to next() within a full cycle of range_size_ numbers.
            do
            {
                current_lcg_state_ = (a_lcg_ * current_lcg_state_ + c_lcg_) % m_lcg_;
                generated_val_in_lcg_range = current_lcg_state_;
            } while (generated_val_in_lcg_range >= range_size_);
            // The above loop guarantees that `generated_val_in_lcg_range` is in
            // `[0, range_size_ - 1]`.
            // Because the LCG has a full period `m_lcg_`, and `m_lcg_ >= range_size_`,
            // this process will eventually visit every number in `[0, range_size_ - 1]`
            // exactly once over `range_size_` successful (non-rejected) generations,
            // provided the sequence of LCG states `current_lcg_state_` covers `0..m_lcg_-1`
            // and we effectively "filter" this sequence.

            current_count_++;
            return min_value_ + generated_val_in_lcg_range;
        }

        // Resets the generator to start a new cycle of unique numbers.
        // The new cycle will likely be a different permutation if the seed/initial LCG state
        // changes.
        // For this implementation, to get the *same* sequence again, one would need to
        // re-initialize current_lcg_state_ to its original seed and current_count_ to 0.
        // This reset provides a new permutation based on a new random seed for the LCG.
        void reset()
        {
            current_count_ = 0;
            std::random_device rd;
            // Re-seed the LCG's initial state for a new permutation.
            // If you want the *same* permutation after reset, you'd need to store and reuse the
            // initial LCG seed.
            current_lcg_state_ = rd();
            current_lcg_state_ %= m_lcg_;
        }

        // Resets the generator to its initial state to produce the same sequence again.
        // Requires storing the initial seed.
        // For simplicity, this example's `reset()` gives a new sequence.
        // To add a "replayable reset", you'd store the initial `current_lcg_state_` set in
        // constructor.

        uint64_t count_generated() const
        {
            return current_count_;
        }

        uint64_t total_in_range() const
        {
            return range_size_;
        }

        uint64_t getMinValue() const
        {
            return min_value_;
        }

        uint64_t getMaxValue() const
        {
            return max_value_;
        }

    private:
        // Helper to find the next power of 2
        uint64_t next_power_of_two(uint64_t n)
        {
            IF_UNLIKELY(n == 0) return 1; // Or handle as an error, range size won't be 0
            n--;
            n |= n >> 1;
            n |= n >> 2;
            n |= n >> 4;
            n |= n >> 8;
            n |= n >> 16;
            n |= n >> 32;
            n++;
            return n;
        }

};


/**
 * Generate random offsets aligned to block size for maximum file blocks coverage.
 *
 * This random generator is specifically built to provide full coverage of all block offsets within
 * a file.
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
 *
 * WARNING: This class is much more complex than the V1 class, but V1 might generate simple
 * sequential offsets or simple reverse sequential offsets, which is why it should be avoided and
 * this V2 should be used instead.
 */
class OffsetGenRandomAlignedFullCoverageV2 : public OffsetGenerator
{
    public:
        /**
         * Note: This is currently only suitable for ranges that have start and length rounded to
         * full block sizes or a range that consists of a single possible partial block. Otherwise
         * the last block offset will be returned as the partial one, which might then be in the
         * middle of a file.
         */
        OffsetGenRandomAlignedFullCoverageV2(uint64_t numBytesTotal,
            uint64_t len, uint64_t offset, size_t blockSize) :
            numBytesTotal(numBytesTotal), numBytesLeft(numBytesTotal),
            rangeLen(len), blockSize(blockSize),
            randomGen(
                calcStartBlockIdx(offset, blockSize),
                calcEndBlockIdx(offset, len, blockSize) )
        {
            // all init in header
        }

        virtual ~OffsetGenRandomAlignedFullCoverageV2() {}

    private:
        uint64_t numBytesTotal;
        uint64_t numBytesLeft;
        uint64_t rangeLen; // byte length of range to randomly seek in from given offset

        const size_t blockSize;

        CoveringRandomGenerator randomGen;

        // inliners
    public:
        virtual void reset() override
        {
            numBytesLeft = numBytesTotal;

            randomGen.reset();
        }

        /**
         * Note: This sets numBytesTotal equal to len, so might not be appropriate for cases of
         * shared overlapping file writes where each thread possibly only writes a partial amount of
         * the file size.
         */
        virtual void reset(uint64_t len, uint64_t offset) override
        {
            this->numBytesTotal = len;
            this->numBytesLeft = len;
            this->rangeLen = len;

            randomGen = CoveringRandomGenerator(
                calcStartBlockIdx(offset, blockSize),
                calcEndBlockIdx(offset, len, blockSize) );
        }

        virtual uint64_t getNextOffset() override
            { return randomGen.next() * blockSize; }

        virtual size_t getBlockSize() const override
            { return blockSize; }

        virtual size_t getNextBlockSizeToSubmit() const override
            { return std::min(numBytesLeft, (uint64_t)blockSize); }

        virtual uint64_t getNumBytesTotal() const override
            { return numBytesTotal; }

        virtual uint64_t getNumBytesLeftToSubmit() const override
            { return numBytesLeft; }

        virtual void addBytesSubmitted(size_t numBytes) override
            { numBytesLeft -= numBytes; }

    private:
        /**
         * Calculate min offset in number of blocks from beginning of file.
         */
        static uint64_t calcStartBlockIdx(uint64_t offset, uint64_t blockSize)
        {
            return blockSize ? (offset / blockSize) : 0;
        }

        /**
         * Calculate max offset in number of blocks from beginning of file.
         */
        static uint64_t calcEndBlockIdx(uint64_t offset, uint64_t rangeLen, uint64_t blockSize)
        {
            return calcStartBlockIdx(offset, blockSize) +
                ( (blockSize && (rangeLen / blockSize) ) ? (rangeLen / blockSize) - 1 : 0);
        }

};
