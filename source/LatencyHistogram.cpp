// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <algorithm>

#include "LatencyHistogram.h"
#include "toolkits/UnitTk.h"
#include "workers/RemoteWorker.h"

namespace
{
    /**
     * Decimal order-of-magnitude ("decade") of a microsec value: 0 for [0,9], 1 for [10,99],
     * 2 for [100,999], and so on. Pure integer math, used by getHistogramGroupedStr().
     */
    unsigned decadeExponent(uint64_t v)
    {
        unsigned decadeExp = 0;

        for(uint64_t threshold = 10; v >= threshold; threshold *= 10)
            decadeExp++;

        return decadeExp;
    }

    uint64_t pow10(unsigned exponent)
    {
        uint64_t result = 1;

        for(unsigned i = 0; i < exponent; i++)
            result *= 10;

        return result;
    }

    /**
     * Maps a decade exponent (as returned by decadeExponent(), in microsec) to a display unit
     * ("us"/"ms"/"s") and the digit-decade within that unit (0 for "1-9", 1 for "10-99", and so
     * on; unit switches happen exactly at the same 1e3/1e6 microsec points as
     * UnitTk::latencyUsToHumanStr() ).
     */
    void decadeUnitAndDigits(unsigned decadeExp, std::string& outUnit, unsigned& outDigitDecade)
    {
        if(decadeExp < 3)
        {
            outUnit = "us";
            outDigitDecade = decadeExp;
        }
        else
        if(decadeExp < 6)
        {
            outUnit = "ms";
            outDigitDecade = decadeExp - 3;
        }
        else
        {
            outUnit = "s";
            outDigitDecade = decadeExp - 6;
        }
    }

    /**
     * E.g. "1-9ms" for the digit-decade covering [1,9]ms. The very first decade (decadeExp 0)
     * starts at 0 instead of 1, so it also captures exact 0us latencies.
     */
    std::string decadeRangeStr(unsigned decadeExp)
    {
        std::string unit;
        unsigned digitDecade;
        decadeUnitAndDigits(decadeExp, unit, digitDecade);

        uint64_t lowerBound = (decadeExp == 0) ? 0 : pow10(digitDecade);
        uint64_t upperBound = pow10(digitDecade + 1) - 1;

        return std::to_string(lowerBound) + "-" + std::to_string(upperBound) + unit;
    }

    /**
     * E.g. "1ms" for the digit-decade covering [1,9]ms, used for the open-ended top category
     * ("≥1ms" style) instead of decadeRangeStr()'s closed range.
     */
    std::string decadeLowerBoundStr(unsigned decadeExp)
    {
        std::string unit;
        unsigned digitDecade;
        decadeUnitAndDigits(decadeExp, unit, digitDecade);

        uint64_t lowerBound = (decadeExp == 0) ? 0 : pow10(digitDecade);

        return std::to_string(lowerBound) + unit;
    }
} // namespace

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

/**
 * Get a consolidated, human-readable summary of the histogram: one column-aligned line per
 * order-of-magnitude ("decade") latency range (e.g. "0-9us", "10-99us", "100-999us", "1-9ms",
 * ...), each showing the summed bucket count for that range and its percentage of all stored
 * values. Much shorter than getHistogramStr() when latencies have a lot of variance, at the cost
 * of losing the fine-grained per-bucket detail.
 *
 * @continuationIndent number of spaces to indent every line after the first (the first line is
 * expected to be appended right after a caller-printed opener, e.g. "[ ").
 */
std::string LatencyHistogram::getHistogramGroupedStr(size_t continuationIndent) const
{
    // one accumulator slot per decade of the full representable range is more than enough
    constexpr unsigned numDecades = 20;
    uint64_t categorySums[numDecades] = {};

    for(size_t bucketIndex = 0; bucketIndex < LATHISTO_NUMBUCKETS; bucketIndex++)
    {
        if(!buckets[bucketIndex] )
            continue;

        unsigned decade = decadeExponent(indexToUpperBoundMicroSec(bucketIndex) );
        categorySums[decade] += buckets[bucketIndex];
    }

    unsigned topCategoryDecade = decadeExponent(indexToUpperBoundMicroSec(LATHISTO_NUMBUCKETS-1) );
    bool isTopCategoryOpenEnded = (buckets[LATHISTO_NUMBUCKETS-1] > 0);

    // build pass: collect (rangeLabel, countStr, percentage) per non-empty decade and track the
    // widest label/count actually printed, so columns align without reserving unused space
    struct SummaryLine
    {
        std::string rangeLabel;
        size_t rangeLabelDisplayWidth; // may differ from rangeLabel.length() for multi-byte UTF-8
        std::string countStr; // pre-formatted with a base10 suffix (K/M/G/...) for large counts
        double percentage;
    };

    std::vector<SummaryLine> lines;
    size_t maxRangeLabelWidth = 0;
    size_t maxCountStrWidth = 0;

    for(unsigned decade = 0; decade < numDecades; decade++)
    {
        if(!categorySums[decade] )
            continue;

        SummaryLine line;
        line.countStr = UnitTk::numToHumanStrBase10(categorySums[decade]);
        line.percentage = 100.0 * categorySums[decade] / numStoredValues;

        if(isTopCategoryOpenEnded && (decade == topCategoryDecade) )
        {
            // "≥" is 1 terminal column, but 3 bytes in UTF-8, so its display width can't just be
            // std::string::length() (that would overcount and throw off setw() padding below)
            std::string lowerBoundStr = decadeLowerBoundStr(decade);
            line.rangeLabel = "≥" + lowerBoundStr;
            line.rangeLabelDisplayWidth = 1 + lowerBoundStr.length();
        }
        else
        {
            line.rangeLabel = decadeRangeStr(decade);
            line.rangeLabelDisplayWidth = line.rangeLabel.length();
        }

        maxRangeLabelWidth = std::max(maxRangeLabelWidth, line.rangeLabelDisplayWidth);
        maxCountStrWidth = std::max(maxCountStrWidth, line.countStr.length() );

        lines.push_back(std::move(line) );
    }

    // emit pass: pad every field to the widths collected above
    std::ostringstream stream;

    for(size_t i = 0; i < lines.size(); i++)
    {
        const SummaryLine& line = lines[i];

        if(i > 0)
            stream << "\n" << std::string(continuationIndent, ' ');

        // pad manually (rather than via std::setw() ) because rangeLabel's byte length can
        // differ from its display width for the "≥" case above
        stream << line.rangeLabel <<
            std::string(maxRangeLabelWidth - line.rangeLabelDisplayWidth, ' ') << ": " <<
            std::setw(maxCountStrWidth) << line.countStr <<
            " (" << std::fixed << std::setprecision(1) << std::setw(5) << line.percentage << "%)";
    }

    return stream.str();
}
