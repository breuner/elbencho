// SPDX-FileCopyrightText: 2020-2025 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef TOOLKITS_BLOCKRANGETK_H_
#define TOOLKITS_BLOCKRANGETK_H_

#include <algorithm>
#include <cstdint>

/**
 * Toolkit to split a contiguous range of blocks evenly over the worker threads.
 */
class BlockRangeTk
{
    private:
        BlockRangeTk() {}

    public:
        /**
         * Split numBlocksTotal contiguous blocks over numThreads workers. Each worker gets a
         * contiguous range, and the first (numBlocksTotal % numThreads) workers get one block more
         * than the others, so no worker has more than one block more than any other.
         *
         * @outStartBlock first block of this worker.
         * @outEndBlock end block of this worker (not inclusive); equals outStartBlock if the
         *      worker gets no block.
         */
        static void getWorkerBlockRange(uint64_t numBlocksTotal, uint64_t numThreads,
            uint64_t workerRank, uint64_t& outStartBlock, uint64_t& outEndBlock)
        {
            const uint64_t blocksPerThread = numBlocksTotal / numThreads;
            const uint64_t numExtraBlocks = numBlocksTotal % numThreads; // one each for low ranks

            outStartBlock = (workerRank * blocksPerThread) + std::min(workerRank, numExtraBlocks);
            outEndBlock = outStartBlock + blocksPerThread +
                ( (workerRank < numExtraBlocks) ? 1 : 0);
        }
};

#endif /* TOOLKITS_BLOCKRANGETK_H_ */
