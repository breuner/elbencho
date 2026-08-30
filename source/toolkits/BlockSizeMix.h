// SPDX-FileCopyrightText: 2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef TOOLKITS_BLOCKSIZEMIX_H_
#define TOOLKITS_BLOCKSIZEMIX_H_

#include <cstdint>
#include <string>
#include <vector>

#include "toolkits/random/RandAlgoInterface.h"

/**
 * Represents a (possibly single-entry) weighted mix of block sizes, as given by the user via
 * "-b"/"--block" using the syntax "size[:weight][,size[:weight]...]", e.g. "4k:3,64k:1".
 *
 * Weights default to 1 when omitted and do not need to sum up to 100 (or any other fixed total);
 * they are only relative to each other.
 */
class BlockSizeMix
{
    public:
        /**
         * Parses a comma-separated "size[:weight]" list, e.g. "4k:3,64k:1". throws ProgException
         * on invalid input.
         */
        static BlockSizeMix parse(const std::string& blockSizeStr);

        std::string toString() const;

    private:
        std::vector<std::pair<size_t, uint64_t> > sizesAndWeights; // size, weight
        std::vector<uint64_t> cumulativeWeights; // same order as sizesAndWeights
        uint64_t totalWeight{0};
        size_t maxSize{0};
        size_t minSize{0};

    public:
        // inliners

        bool isMix() const { return sizesAndWeights.size() > 1; }

        size_t getMaxSize() const { return maxSize; }
        size_t getMinSize() const { return minSize; }

        const std::vector<std::pair<size_t, uint64_t> >& getSizesAndWeights() const
            { return sizesAndWeights; }

        /**
         * Draws the next block size based on the configured weights.
         * (Inlined because called in hot I/O path.)
         */
        size_t getNextSize(RandAlgoInterface& randAlgo) const
        {
            if(!isMix() )
                return maxSize; // single-entry mix, no need for a random draw

            uint64_t drawnWeight = randAlgo.next() % totalWeight;

            for(size_t i = 0; i < cumulativeWeights.size(); i++)
            {
                if(drawnWeight < cumulativeWeights[i] )
                    return sizesAndWeights[i].first;
            }

            return sizesAndWeights.back().first; // should be unreachable
        }
};

#endif /* TOOLKITS_BLOCKSIZEMIX_H_ */
