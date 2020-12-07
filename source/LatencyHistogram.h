#ifndef LATENCYHISTOGRAM_H_
#define LATENCYHISTOGRAM_H_

#include <cmath>
#include <ctype.h>
#include <vector>
#include "ProgArgs.h"
#include "workers/WorkersSharedData.h"

#define LATHISTO_BUCKETFRACTION		4 // log2 1/n increments between buckets (4 means 1/4=0.25)
#define LATHISTO_MAXLOG2MICROSEC	28 /* n here means max microsec lat in histogram is
										(2^n)-(1/LATHISTO_BUCKETFRACTION) */
#define LATHISTO_NUMBUCKETS			(LATHISTO_MAXLOG2MICROSEC*LATHISTO_BUCKETFRACTION) /* number of
										available buckets */

/**
 * A histogram for operation latency.
 *
 * Organized in microsecond log2 buckets starting and log2(0) and using 1/LATHISTO_BUCKETFRACTION
 * increments between buckets. This means the precision is less for higher latencies. For 2^28
 * microsec (268seconds) buckets with 0.25 (1/4) increments we have 112(=28*(1/0.25) ) buckets.
 *
 * Min/max/avg functions can always be used after latencies have been added. Histogram and
 * percentile functions should only be used after checking getHistogramExceeded().
 */
class LatencyHistogram
{
	public:
		LatencyHistogram() : buckets(LATHISTO_NUMBUCKETS, 0) {}

		void getAsPropertyTree(bpt::ptree& outTree, std::string prefixStr);
		void setFromPropertyTree(bpt::ptree& tree, std::string prefixStr);

	private:
		uint64_t numStoredValues{0}; // number of all values stored in all buckets
		uint64_t numMicroSecTotal{0}; // sum of all values stored in all buckets in microseconds
		uint64_t minMicroSecLat{(size_t)~0}; // min measured lat val (~0 so any 1st val is smaller)
		uint64_t maxMicroSecLat{0}; // max measured latency value
		std::vector<uint64_t> buckets; // buckets represent counters for latency categories

		// inliners
	public:
		void addLatency(uint64_t latencyMicroSec)
		{
			numStoredValues++;
			numMicroSecTotal += latencyMicroSec;

			IF_UNLIKELY(latencyMicroSec < minMicroSecLat)
				minMicroSecLat = latencyMicroSec;

			IF_UNLIKELY(latencyMicroSec > maxMicroSecLat)
				maxMicroSecLat = latencyMicroSec;

			size_t bucketIndex;

			// log2(0) does not exist, so special case
			IF_UNLIKELY(!latencyMicroSec)
				bucketIndex = 0;
			else
				bucketIndex = std::log2(latencyMicroSec) * LATHISTO_BUCKETFRACTION;

			IF_UNLIKELY(bucketIndex >= LATHISTO_NUMBUCKETS)
				bucketIndex = LATHISTO_NUMBUCKETS-1;

			buckets[bucketIndex]++;
		}

		size_t getNumStoredValues() const { return numStoredValues; }
		size_t getMinMicroSecLat() const { return minMicroSecLat; }
		size_t getMaxMicroSecLat() const { return maxMicroSecLat; }

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
			if(getHistogramExceeded() )
				return "Histogram size exceeded";

			std::ostringstream stream;
			double log2BucketSize = 1.0 / LATHISTO_BUCKETFRACTION;

			for(size_t bucketIndex = 0; bucketIndex < LATHISTO_NUMBUCKETS; bucketIndex++)
			{
				if(!buckets[bucketIndex] )
					continue; // skip printing empty buckets

				double bucketMicroSec = pow(2, (bucketIndex+1)*log2BucketSize);

				if(!stream.str().empty() )
					stream << ", "; // add separator to previous element

				stream << std::fixed << std::setprecision(bucketMicroSec < 10 ? 1 : 0) <<
					bucketMicroSec << ": ";
				stream << buckets[bucketIndex];
			}

			return stream.str();
		}

		/**
		 * Get the upper latency bound for a given percentage of values.
		 *
		 * @return upper latency bound in microseconds.
		 */
		double getPercentile(double percentage) const
		{
			size_t numValuesSoFar = 0;
			double log2BucketSize = 1.0 / LATHISTO_BUCKETFRACTION;

			for(size_t bucketIndex = 0; bucketIndex < LATHISTO_NUMBUCKETS; bucketIndex++)
			{
				numValuesSoFar += buckets[bucketIndex];

				double percentileSoFar = (double)numValuesSoFar / numStoredValues;

				if(percentileSoFar >= (percentage/100) )
				{ // we added enough buckets for given percentage
					double bucketMicroSec = pow(2, (bucketIndex+1)*log2BucketSize);
					return bucketMicroSec;
				}
			}

			return 0; // should never happen
		}

		/**
		 * Get lat percentile as string with precision=1 for results < 10 and precision=0 otherwise.
		 */
		std::string getPercentileStr(double percentage) const
		{
			double percentile = getPercentile(percentage);

			std::ostringstream stream;

			stream << std::fixed << std::setprecision(percentile < 10 ? 1 : 0) << percentile;

			return stream.str();
		}

		/**
		 * Return whether we have potentially latencies measured that exceeded the histogram size.
		 * In this case min/max/avg are ok to use, but histogram and percentile functions should not
		 * be used.
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
};

#endif /* LATENCYHISTOGRAM_H_ */
