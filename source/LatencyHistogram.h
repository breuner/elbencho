// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef LATENCYHISTOGRAM_H_
#define LATENCYHISTOGRAM_H_

#include <atomic>
#include <cstdint>
#include <vector>

#include "ProgArgs.h"
#include "workers/WorkersSharedData.h"

#define LATHISTO_SUBBUCKET_BITS         7 // log2 of linear sub-buckets per octave
#define LATHISTO_SUBBUCKET_COUNT        (1u << LATHISTO_SUBBUCKET_BITS) // linear subbuckets/octave
#define LATHISTO_SUBBUCKET_HALFCOUNT    (LATHISTO_SUBBUCKET_COUNT / 2)
#define LATHISTO_MAXBUCKETIDX           38 /* number of octaves above bucket 0; max representable
                                            latency is ~1.1 years:
                                            (LATHISTO_SUBBUCKET_COUNT << LATHISTO_MAXBUCKETIDX)
                                            microsec */
#define LATHISTO_NUMBUCKETS             (LATHISTO_SUBBUCKET_COUNT + \
                                            (LATHISTO_MAXBUCKETIDX * LATHISTO_SUBBUCKET_HALFCOUNT) )
                                            /* total number of flat histogram slots */

/**
 * A histogram for I/O operation latency.
 *
 * Log-linear histogram in the style of Gil Tene's HdrHistogram: bucket 0 covers microsec values
 * [0, LATHISTO_SUBBUCKET_COUNT) linearly and exactly. Each higher octave "b" (1..MAXBUCKETIDX)
 * covers [LATHISTO_SUBBUCKET_HALFCOUNT*2^b, LATHISTO_SUBBUCKET_COUNT*2^b), split into
 * LATHISTO_SUBBUCKET_HALFCOUNT linear subbuckets of width 2^b. This bounds the per-slot relative
 * error to roughly 0.8%-1.6% at any magnitude. Values beyond the highest octave are clamped into
 * the last slot.
 *
 * Min/max/avg functions can always be used after latencies have been added. Histogram and
 * percentile functions can always be used too; if a result falls into the open-ended top slot,
 * it is only a lower bound (getPercentileStr()/getHistogramStr() flag this with a "≥" prefix).
 */
class LatencyHistogram
{
    public:
        LatencyHistogram() : buckets(LATHISTO_NUMBUCKETS, 0) {}

        void getAsPropertyTreeForJSONFile(bpt::ptree& outTree, std::string subtreeKey) const;
        void getAsPropertyTreeForService(bpt::ptree& outTree, std::string prefixStr) const;
        void setFromPropertyTreeForService(bpt::ptree& tree, std::string prefixStr);
        std::string getHistogramGroupedStr(size_t continuationIndent) const;

    private:
        uint64_t numStoredValues{0}; // number of all values stored in all buckets
        uint64_t numMicroSecTotal{0}; // sum of all values stored in all buckets in microseconds
        uint64_t minMicroSecLat{(size_t)~0}; // min measured lat val (~0 so any 1st val is smaller)
        uint64_t maxMicroSecLat{0}; // max measured latency value
        std::vector<uint64_t> buckets; // buckets represent counters for latency categories
        std::atomic_uint64_t numStoredValuesLive{0}; // for live stats
        std::atomic_uint64_t numMicroSecsTotalLive{0}; // for live stats

    public: // inliners

        void addLatency(uint64_t latencyMicroSec)
        {
            // note: live stats update is not atomic across the two vals, but that's negligible
            numStoredValuesLive++;
            numMicroSecsTotalLive += latencyMicroSec;

            numStoredValues++;
            numMicroSecTotal += latencyMicroSec;

            IF_UNLIKELY(latencyMicroSec < minMicroSecLat)
                minMicroSecLat = latencyMicroSec;

            IF_UNLIKELY(latencyMicroSec > maxMicroSecLat)
                maxMicroSecLat = latencyMicroSec;

            buckets[valueToIndex(latencyMicroSec)]++;
        }

        size_t getNumStoredValues() const { return numStoredValues; }
        size_t getMinMicroSecLat() const { return minMicroSecLat; }
        size_t getMaxMicroSecLat() const { return maxMicroSecLat; }

        void addAndResetAverageLiveMicroSec(
            uint64_t& outNumStoredValues, uint64_t& outNumMicroSecsTotal)
        {
            outNumStoredValues += numStoredValuesLive;
            outNumMicroSecsTotal += numMicroSecsTotalLive;

            numStoredValuesLive = 0;
            numMicroSecsTotalLive = 0;
        }

        size_t getAverageMicroSec() const
        {
            return numStoredValues ? (numMicroSecTotal / numStoredValues) : 0;
        }

        void reset()
        {
            for(size_t bucketIndex = 0; bucketIndex < LATHISTO_NUMBUCKETS; bucketIndex++)
                buckets[bucketIndex] = 0;

            numStoredValues = 0;
            numMicroSecTotal = 0;
            minMicroSecLat = ~0; // ~0 so that any 1st measured value is smaller
            maxMicroSecLat = 0;
        }

        std::string getHistogramStr() const
        {
            std::ostringstream stream;

            for(size_t bucketIndex = 0; bucketIndex < LATHISTO_NUMBUCKETS; bucketIndex++)
            {
                if(!buckets[bucketIndex] )
                    continue; // skip printing empty buckets

                uint64_t bucketMicroSec = indexToUpperBoundMicroSec(bucketIndex);
                bool isOpenEnded = (bucketIndex == LATHISTO_NUMBUCKETS-1);

                if(!stream.str().empty() )
                    stream << ", "; // add separator to previous element

                stream << (isOpenEnded ? "≥" : "") <<
                    std::fixed << std::setprecision(bucketMicroSec < 10 ? 1 : 0) <<
                    bucketMicroSec << ": ";
                stream << buckets[bucketIndex];
            }

            return stream.str();
        }

        /**
         * Get the upper latency bound for a given percentage of values. If the percentile falls
         * into the open-ended top slot, the returned value is only a lower bound (some actual
         * latencies contributing to it may be higher); use getPercentileStr() for a display
         * string that flags this case.
         *
         * @return upper latency bound in microseconds.
         */
        uint64_t getPercentile(double percentage) const
        {
            return indexToUpperBoundMicroSec(findPercentileBucketIndex(percentage) );
        }

        /**
         * Get lat percentile as string with precision=1 for results < 10 and precision=0
         * otherwise. Prefixed with "≥" if the percentile falls into the open-ended top slot.
         */
        std::string getPercentileStr(double percentage) const
        {
            size_t bucketIndex = findPercentileBucketIndex(percentage);
            uint64_t percentile = indexToUpperBoundMicroSec(bucketIndex);
            bool isOpenEnded = (bucketIndex == LATHISTO_NUMBUCKETS-1);

            std::ostringstream stream;

            stream << (isOpenEnded ? "≥" : "") <<
                std::fixed << std::setprecision(percentile < 10 ? 1 : 0) << percentile;

            return stream.str();
        }

        /**
         * Return whether the open-ended top histogram slot holds any values, meaning at least
         * one measured latency was at or beyond the top of the representable range. Does not by
         * itself invalidate histogram/percentile/min/max/avg output: it only means a result that
         * falls into that particular slot is a lower-bound estimate rather than an exact value
         * (flagged with "≥" by getHistogramStr()/getPercentileStr() ).
         */
        bool getHistogramExceeded() const
        {
            return buckets[LATHISTO_NUMBUCKETS-1] ? true : false;
        }

        LatencyHistogram& operator+=(const LatencyHistogram& rhs)
        {
            for(size_t bucketIndex = 0; bucketIndex < LATHISTO_NUMBUCKETS; bucketIndex++)
                buckets[bucketIndex] += rhs.buckets[bucketIndex];

            numStoredValues += rhs.numStoredValues;
            numMicroSecTotal += rhs.numMicroSecTotal;

            if(rhs.minMicroSecLat < minMicroSecLat)
                minMicroSecLat = rhs.minMicroSecLat;

            if(rhs.maxMicroSecLat > maxMicroSecLat)
                maxMicroSecLat = rhs.maxMicroSecLat;

            return *this;
        }


    private: // inliners

        /**
         * Find the flat histogram slot at which the cumulative count first reaches the given
         * percentage of all stored values. Requires numStoredValues > 0.
         */
        size_t findPercentileBucketIndex(double percentage) const
        {
            size_t numValuesSoFar = 0;

            for(size_t bucketIndex = 0; bucketIndex < LATHISTO_NUMBUCKETS; bucketIndex++)
            {
                numValuesSoFar += buckets[bucketIndex];

                double percentileSoFar = (double)numValuesSoFar / numStoredValues;

                if(percentileSoFar >= (percentage/100) )
                    return bucketIndex;
            }

            return LATHISTO_NUMBUCKETS - 1; // should never happen if numStoredValues > 0
        }

        /**
         * Map a microsec latency value to its flat histogram slot index (HdrHistogram-style
         * log-linear bucketing). Pure integer math, O(1), no floating point, no loops.
         */
        static size_t valueToIndex(uint64_t latencyMicroSec)
        {
            IF_UNLIKELY(latencyMicroSec < LATHISTO_SUBBUCKET_COUNT)
                return latencyMicroSec; // bucket 0: exact linear index

            unsigned bucketIndex =
                (63 - __builtin_clzll(latencyMicroSec) ) - (LATHISTO_SUBBUCKET_BITS - 1);

            IF_UNLIKELY(bucketIndex > LATHISTO_MAXBUCKETIDX)
                bucketIndex = LATHISTO_MAXBUCKETIDX; // clamp pathological overflow into top bucket

            uint64_t subBucketIndex = latencyMicroSec >> bucketIndex; // normally in [64,128)

            IF_UNLIKELY(subBucketIndex >= LATHISTO_SUBBUCKET_COUNT)
                subBucketIndex = LATHISTO_SUBBUCKET_COUNT - 1; // clamp if value saturated bucketIndex

            return LATHISTO_SUBBUCKET_COUNT +
                ( (bucketIndex - 1) * LATHISTO_SUBBUCKET_HALFCOUNT) +
                (subBucketIndex - LATHISTO_SUBBUCKET_HALFCOUNT);
        }

        /**
         * Reverse mapping of valueToIndex(): returns the upper bound microsec value represented
         * by the given flat histogram slot index.
         */
        static uint64_t indexToUpperBoundMicroSec(size_t bucketIndex)
        {
            if(bucketIndex < LATHISTO_SUBBUCKET_COUNT)
                return bucketIndex + 1; // bucket 0: exact

            unsigned octave =
                1 + (bucketIndex - LATHISTO_SUBBUCKET_COUNT) / LATHISTO_SUBBUCKET_HALFCOUNT;
            uint64_t subBucketIndex = LATHISTO_SUBBUCKET_HALFCOUNT +
                ( (bucketIndex - LATHISTO_SUBBUCKET_COUNT) % LATHISTO_SUBBUCKET_HALFCOUNT);

            return (subBucketIndex + 1) << octave;
        }

};

#endif /* LATENCYHISTOGRAM_H_ */
