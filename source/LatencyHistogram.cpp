// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "LatencyHistogram.h"
#include "workers/RemoteWorker.h"

/**
 * @prefixStr prefix for element names (XFER_STATS_LAT_PREFIX_...)
 */
void LatencyHistogram::getAsPropertyTreeForJSONFile(bpt::ptree& outTree,
    std::string subtreeKey) const
{
    bpt::ptree subtree;
    uint64_t bucketRangeStartMicroSec = 0;

    for(size_t bucketIndex = 0; bucketIndex < LATHISTO_NUMBUCKETS; bucketIndex++)
    {
        const uint64_t rangeMatchCount = buckets[bucketIndex];
        const uint64_t bucketRangeEndMicroSec = indexToUpperBoundMicroSec(bucketIndex);
        const bool isOpenEnded = (bucketIndex == LATHISTO_NUMBUCKETS-1);

        if(!rangeMatchCount)
        {
            // no values here => skip range add for brevity
            bucketRangeStartMicroSec = bucketRangeEndMicroSec;
            continue;
        }

        std::string rangeString = isOpenEnded ?
            (std::to_string(bucketRangeStartMicroSec) + "+us") :
            (std::to_string(bucketRangeStartMicroSec) + "-" +
                std::to_string(bucketRangeEndMicroSec) + "us");

        boost::property_tree::ptree rangeEntry;

        rangeEntry.put("range", rangeString);
        rangeEntry.put("count", rangeMatchCount);

        subtree.push_back(std::make_pair("", rangeEntry));

        // update range start for next round
        bucketRangeStartMicroSec = bucketRangeEndMicroSec;
    }

    outTree.put_child(subtreeKey + ".buckets", subtree);
}

/**
 * Sparse encoding: only non-zero buckets are sent, each as an (index, count) pair. This keeps
 * the transferred data small even though the histogram now spans thousands of buckets, since a
 * typical run's latencies cluster into a narrow band and most buckets stay empty.
 *
 * @prefixStr prefix for element names (XFER_STATS_LAT_PREFIX_...)
 */
void LatencyHistogram::getAsPropertyTreeForService(bpt::ptree& outTree, std::string prefixStr) const
{
    outTree.put(prefixStr + XFER_STATS_LATNUMVALUES, numStoredValues);
    outTree.put(prefixStr + XFER_STATS_LATMICROSECTOTAL, numMicroSecTotal);
    outTree.put(prefixStr + XFER_STATS_LATMINMICROSEC, minMicroSecLat);
    outTree.put(prefixStr + XFER_STATS_LATMAXMICROSEC, maxMicroSecLat);

    // add histogram buckets (sparse: only non-zero entries)
    for(size_t bucketIndex = 0; bucketIndex < LATHISTO_NUMBUCKETS; bucketIndex++)
    {
        if(!buckets[bucketIndex] )
            continue;

        bpt::ptree bucketEntry;
        bucketEntry.put(XFER_STATS_LATHISTOLIST_ITEM_IDX, bucketIndex);
        bucketEntry.put(XFER_STATS_LATHISTOLIST_ITEM_CNT, buckets[bucketIndex]);

        outTree.add_child(prefixStr + XFER_STATS_LATHISTOLIST_ITEM, bucketEntry);
    }
}

/**
 * @prefixStr prefix for element names (XFER_STATS_LAT_PREFIX_...)
 */
void LatencyHistogram::setFromPropertyTreeForService(bpt::ptree& tree, std::string prefixStr)
{
    numStoredValues = tree.get<size_t>(prefixStr + XFER_STATS_LATNUMVALUES);
    numMicroSecTotal = tree.get<size_t>(prefixStr + XFER_STATS_LATMICROSECTOTAL);
    minMicroSecLat = tree.get<size_t>(prefixStr + XFER_STATS_LATMINMICROSEC);
    maxMicroSecLat = tree.get<size_t>(prefixStr + XFER_STATS_LATMAXMICROSEC);

    // reset buckets, then apply the sparse (index, count) pairs received
    for(size_t bucketIndex = 0; bucketIndex < LATHISTO_NUMBUCKETS; bucketIndex++)
        buckets[bucketIndex] = 0;

    auto listTree = tree.get_child_optional(prefixStr + XFER_STATS_LATHISTOLIST);
    if(!listTree)
        return; // no bucket data (e.g. sender had no stored values)

    for(bpt::ptree::value_type& bucketItem : *listTree)
    {
        size_t bucketIndex = bucketItem.second.get<size_t>(XFER_STATS_LATHISTOLIST_ITEM_IDX);
        uint64_t bucketCount = bucketItem.second.get<uint64_t>(XFER_STATS_LATHISTOLIST_ITEM_CNT);

        /* bounds check: tolerate a sender compiled with a different LATHISTO_NUMBUCKETS instead
            of writing out of bounds */
        if(bucketIndex >= LATHISTO_NUMBUCKETS)
            continue;

        buckets[bucketIndex] = bucketCount;
    }
}
