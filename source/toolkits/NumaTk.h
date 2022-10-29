#ifndef TOOLKITS_NUMATK_H_
#define TOOLKITS_NUMATK_H_

#include <cstdarg>
#include <sched.h>
#include <string>
#include "Common.h"
#include "Logger.h"
#include "ProgException.h"

#ifdef LIBNUMA_SUPPORT
	#include <numa.h>
#endif


/**
 * Toolkit to bind calling thread to one or multiple NUMA zones.
 *
 * Make sure to call isNumaInfoAvailable() first and only use the other functions if this returns
 * true.
 *
 * This is based on libnuma. See "man 3 numa" for API or https://github.com/numactl/numactl
 */
class NumaTk
{
	private:
		NumaTk() {}

	public:
		/**
		 * Must be called before all other functions in this toolkit and other functions may only
		 * be used if this returns true.
		 */
		static bool isNumaInfoAvailable()
		{
		#ifndef LIBNUMA_SUPPORT
			return false;
		#else // LIBNUMA_SUPPORT
			return (numa_available() != -1);
		#endif // LIBNUMA_SUPPORT
		}

		/**
		 * Bind calling thread to given NUMA zone and set memory allocations to also prefer the
		 * given zone.
		 *
		 * @zoneStr must be compatible with libnuma numa_parse_cpustring, but current method code
		 * 		relies on this being a single number.
		 * 		Valid libnuma examples: 1-5,7,10 !4-5 +0-3.
		 * @throw ProgException on error, e.g. zonesStr parsing failure.
		 */
		static void bindToNumaZone(std::string zonesStr)
		{
		#ifndef LIBNUMA_SUPPORT

			throw ProgException("NUMA binding requested, but this executable was built without "
				"NUMA support.");

		#else // LIBNUMA_SUPPORT

			int maxNode = numa_max_node();
			int desiredNode = std::stoi(zonesStr);

			if(desiredNode > maxNode)
				throw ProgException("Desired NUMA zone is not available. "
					"Desired zone: " + zonesStr + "; "
					"Max zone: " + std::to_string(maxNode) );

			if(desiredNode < 0)
				throw ProgException("Desired NUMA zone may not be negative. "
					"Desired zone: " + zonesStr);

			struct bitmask* nodeMask = numa_parse_nodestring(zonesStr.c_str() );
			if(nodeMask == NULL)
				throw ProgException("Parsing of NUMA zone string failed: " + zonesStr);

			// note: don't use numa_bind here, as it has no error return value

			int runRes = numa_run_on_node_mask(nodeMask);

			numa_free_nodemask(nodeMask);

			if(runRes == -1)
				ProgException("Applying NUMA zone node mask failed. "
					"Given zones: " + zonesStr + "; "
					"SysErr: " + strerror(errno) );

			// note: only "preferred" instead of numa_set_membind to allow fallback to other zones

			numa_set_preferred(desiredNode); // set mem alloc to prefer desiredNode

		#endif // LIBNUMA_SUPPORT
		}

		/**
		 * Bind calling thread to given zones. This is intended to be called with possibly multiple
		 * zones, so it does not set memory binding to a certain preferred zone.
		 *
		 * @zoneStr must be compatible with libnuma numa_parse_cpustring.
		 * 		Valid libnuma examples: 1-5,7,10 !4-5 +0-3.
		 * @throw ProgException on error, e.g. zonesStr parsing failure.
		 */
		static void bindToNumaZones(std::string zonesStr)
		{
		#ifndef LIBNUMA_SUPPORT

			throw ProgException("NUMA binding requested, but this executable was built without "
				"NUMA support.");

		#else // LIBNUMA_SUPPORT

			struct bitmask* nodeMask = numa_parse_nodestring(zonesStr.c_str() );
			if(nodeMask == NULL)
				throw ProgException("Parsing of NUMA zone string failed: " + zonesStr);

			int runRes = numa_run_on_node_mask(nodeMask);

			numa_free_nodemask(nodeMask);

			if(runRes == -1)
				ProgException("Applying NUMA zone node mask failed. "
					"Given zones: " + zonesStr + "; "
					"SysErr: " + strerror(errno) );

		#endif // LIBNUMA_SUPPORT
		}

		/**
		 * Bind calling thread to given cpu cores. This is intended to be called with possibly
		 * multiple cores, so it does not set memory binding to a certain preferred zone.
		 *
		 * @throw ProgException on error
		 */
		static void bindToCPUCores(const IntVec& cpuCoresVec)
		{
		#ifndef COREBIND_SUPPORT

			throw ProgException("CPU core binding requested, but this executable was built without "
				"CPU core binding support.");

		#else // COREBIND_SUPPORT

			cpu_set_t cpuCoreSet;
			std::string coresStr; // just for error log message

			CPU_ZERO(&cpuCoreSet);

			for(int core : cpuCoresVec)
			{
				CPU_SET(core, &cpuCoreSet);
				coresStr += std::to_string(core) + " ";
			}

			int schedRes = sched_setaffinity(0, sizeof(cpuCoreSet), &cpuCoreSet);

			if(schedRes == -1)
				ProgException("Applying CPU core set failed. "
					"Given cores: " + coresStr + "; "
					"SysErr: " + strerror(errno) );

		#endif // COREBIND_SUPPORT
		}

		/**
		 * Bind calling thread to given cpu core.
		 *
		 * @throw ProgException on error
		 */
		static void bindToCPUCore(int cpuCore)
		{
		#ifndef COREBIND_SUPPORT

			throw ProgException("CPU core binding requested, but this executable was built without "
				"CPU core binding support.");

		#else // COREBIND_SUPPORT

			IntVec coreVec;

			coreVec.push_back(cpuCore);

			bindToCPUCores(coreVec);

		#endif // COREBIND_SUPPORT
		}
};


#endif /* TOOLKITS_NUMATK_H_ */
