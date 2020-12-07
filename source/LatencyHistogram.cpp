#include "LatencyHistogram.h"
#include "workers/RemoteWorker.h"

/**
 * @prefixStr prefix for element names (XFER_STATS_LAT_PREFIX_...)
 */
void LatencyHistogram::getAsPropertyTree(bpt::ptree& outTree, std::string prefixStr)
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
void LatencyHistogram::setFromPropertyTree(bpt::ptree& tree, std::string prefixStr)
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
