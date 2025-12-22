// SPDX-FileCopyrightText: 2020-2025 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "LatencyHistogram.h"
#include "workers/RemoteWorker.h"

/**
 * @prefixStr prefix for element names (XFER_STATS_LAT_PREFIX_...)
 */
void LatencyHistogram::getAsPropertyTreeForJSONFile(bpt::ptree& outTree,
    std::string subtreeKey) const
{
    const double log2BucketSize = 1.0 / LATHISTO_BUCKETFRACTION;

    bpt::ptree subtree;
    double bucketRangeStartMicroSec = 0;

    if(getHistogramExceeded() )
    { // can't show histogram
         subtree.put("histo_max_exceeded",
            "Histogram not available because highest latency value exceeded histogram size. "
            "Histogram max: " + std::to_string(pow(2, LATHISTO_NUMBUCKETS*log2BucketSize) ) + "us" );
         outTree.put_child(subtreeKey, subtree);
         return;
    }

    for(size_t bucketIndex = 0; bucketIndex < LATHISTO_NUMBUCKETS; bucketIndex++)
    {
        const uint64_t rangeMatchCount = buckets[bucketIndex];
        const double bucketRangeEndMicroSec = pow(2, (bucketIndex+1)*log2BucketSize);

        if(!rangeMatchCount)
        {
            // no values here => skip range add for brevity
            bucketRangeStartMicroSec = bucketRangeEndMicroSec;
            continue;
        }

        std::ostringstream streamRangeStart;
        std::ostringstream streamRangeEnd;


        streamRangeStart << std::fixed << std::setprecision(bucketRangeStartMicroSec < 10 ? 1 : 0) <<
            bucketRangeStartMicroSec;

        streamRangeEnd << std::fixed << std::setprecision(bucketRangeEndMicroSec < 10 ? 1 : 0) <<
            bucketRangeEndMicroSec;

        std::string rangeString = streamRangeStart.str() + "-" + streamRangeEnd.str() + "us";

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
 * @prefixStr prefix for element names (XFER_STATS_LAT_PREFIX_...)
 */
void LatencyHistogram::getAsPropertyTreeForService(bpt::ptree& outTree, std::string prefixStr) const
{
	outTree.put(prefixStr + XFER_STATS_LATNUMVALUES, numStoredValues);
	outTree.put(prefixStr + XFER_STATS_LATMICROSECTOTAL, numMicroSecTotal);
	outTree.put(prefixStr + XFER_STATS_LATMINMICROSEC, minMicroSecLat);
	outTree.put(prefixStr + XFER_STATS_LATMAXMICROSEC, maxMicroSecLat);

	// add histogram buckets
	for(size_t bucketIndex = 0; bucketIndex < LATHISTO_NUMBUCKETS; bucketIndex++)
		outTree.add(prefixStr + XFER_STATS_LATHISTOLIST_ITEM, buckets[bucketIndex] );
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

	// add histogram buckets
	size_t bucketIndex = 0;
	for(bpt::ptree::value_type& bucketItem : tree.get_child(prefixStr + XFER_STATS_LATHISTOLIST) )
	{
		buckets[bucketIndex] = bucketItem.second.get_value<size_t>();
		bucketIndex++;
	}
}
