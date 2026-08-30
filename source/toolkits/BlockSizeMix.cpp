// SPDX-FileCopyrightText: 2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <algorithm>
#include <boost/algorithm/string.hpp>
#include <set>

#include "ProgException.h"
#include "toolkits/BlockSizeMix.h"
#include "toolkits/UnitTk.h"

/**
 * Parse a comma-separated "size[:weight]" list, e.g. "4k:3,64k:1". Weight defaults to 1 when
 * omitted.
 *
 * @throw ProgException if blockSizeStr is empty or malformed, or contains a duplicate size,
 * zero size or zero weight.
 */
BlockSizeMix BlockSizeMix::parse(const std::string& blockSizeStr)
{
    BlockSizeMix mix;

    std::vector<std::string> entryStrVec;
    boost::split(entryStrVec, blockSizeStr, boost::is_any_of(","), boost::token_compress_on);

    std::set<size_t> seenSizes;

    for(const std::string& entryStr : entryStrVec)
    {
        if(entryStr.empty() )
            continue;

        std::vector<std::string> sizeAndWeightVec;
        boost::split(sizeAndWeightVec, entryStr, boost::is_any_of(":"), boost::token_compress_on);

        if( (sizeAndWeightVec.size() != 1) && (sizeAndWeightVec.size() != 2) )
            throw ProgException("Invalid block size mix entry: \"" + entryStr + "\". "
                "Expected format: size[:weight]");

        size_t size = UnitTk::numHumanToBytesBinary(sizeAndWeightVec[0], true);

        uint64_t weight = 1;

        if(sizeAndWeightVec.size() == 2)
        {
            try
            {
                weight = std::stoull(sizeAndWeightVec[1] );
            }
            catch(std::exception& e)
            {
                throw ProgException("Invalid weight in block size mix entry: \"" + entryStr +
                    "\". Weight must be a positive integer.");
            }
        }

        if(!size)
            throw ProgException("Invalid block size mix entry: \"" + entryStr + "\". "
                "Block size must not be 0.");

        if(!weight)
            throw ProgException("Invalid block size mix entry: \"" + entryStr + "\". "
                "Weight must not be 0.");

        if(!seenSizes.insert(size).second)
            throw ProgException("Invalid block size mix: duplicate block size in \"" +
                blockSizeStr + "\": " + sizeAndWeightVec[0]);

        mix.sizesAndWeights.push_back(std::make_pair(size, weight) );
    }

    if(mix.sizesAndWeights.empty() )
        throw ProgException("Block size mix is empty after parsing: \"" + blockSizeStr + "\"");

    mix.totalWeight = 0;

    for(const auto& sizeAndWeight : mix.sizesAndWeights)
    {
        mix.totalWeight += sizeAndWeight.second;
        mix.cumulativeWeights.push_back(mix.totalWeight);
    }

    mix.maxSize = mix.sizesAndWeights.front().first;
    mix.minSize = mix.sizesAndWeights.front().first;

    for(const auto& sizeAndWeight : mix.sizesAndWeights)
    {
        mix.maxSize = std::max(mix.maxSize, sizeAndWeight.first);
        mix.minSize = std::min(mix.minSize, sizeAndWeight.first);
    }

    return mix;
}

/**
 * Canonical string representation, e.g. for logging or service-mode round-tripping.
 */
std::string BlockSizeMix::toString() const
{
    std::string result;

    for(size_t i = 0; i < sizesAndWeights.size(); i++)
    {
        if(i)
            result += ",";

        result += std::to_string(sizesAndWeights[i].first) + ":" +
            std::to_string(sizesAndWeights[i].second);
    }

    return result;
}
