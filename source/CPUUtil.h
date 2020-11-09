#ifndef CPUUTIL_H_
#define CPUUTIL_H_

#include <unistd.h>
#include "Common.h"

/**
 * Measure CPU utilization for time difference between two update() calls.
 * CPU utilization is everything in the cpu line of /proc/stat except idle and iowait time.
 */
class CPUUtil
{
	public:
		void update();

	private:
		size_t lastIdleCPUTime = 0;
		size_t lastTotalCPUTime = 0;
		size_t currentIdleCPUTime = 0;
		size_t currentTotalCPUTime = 0;

		// inliners
	public:
		/**
		 * @return CPU utilization percent in the interval between two update() calls.
		 */
		float getCPUUtilPercent() const
		{
			const float idleCPUTimeDelta = currentIdleCPUTime - lastIdleCPUTime;
			const float totalCPUTimeDelta = currentTotalCPUTime - lastTotalCPUTime;

			if(!totalCPUTimeDelta)
				return 0; // should not happen: no time passed, return 0 to avoid div by 0 below

			const float cpuUtilPercent = 100.0 * (1.0 - (idleCPUTimeDelta / totalCPUTimeDelta) );

			return cpuUtilPercent;
		}
};

#endif /* CPUUTIL_H_ */
