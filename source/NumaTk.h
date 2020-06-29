#ifndef NUMATK_H_
#define NUMATK_H_

#include <cstdarg>
#include <numa.h>
#include <string>
#include "Logger.h"
#include "ProgException.h"


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
		static bool isNumaInfoAvailable() { return (numa_available() != -1); }

		/**
		 * Bind calling thread to given zones.
		 *
		 * @zoneStr must be compatible with libnuma numa_parse_cpustring, but current method code
		 * 		relies on this being a single number.
		 * 		Valid libnuma examples: 1-5,7,10 !4-5 +0-3.
		 * 	@throw ProgException on error, e.g. zonesStr parsing failure.
		 */
		static void bindToNumaZone(std::string zonesStr)
		{
			numa_exit_on_error = 0; // libnuma setting
			numa_exit_on_warn = 0; // libnuma setting

			/* extra checks here because libnuma on Ubuntu 20.04 segfaults when desired node is
				higher than max available node. */

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
				ProgException("Parsing of NUMA zone string failed: " + zonesStr);

			// note: don't use numa_bind here, as it has no error return value

			int runRes = numa_run_on_node_mask(nodeMask);
			if(runRes == -1)
				ProgException("Applying NUMA zone node mask failed. "
					"Given zones: " + zonesStr + "; "
					"SysErr: " + strerror(errno) );

			numa_set_membind(nodeMask);

			numa_free_nodemask(nodeMask);
		}
};


#endif /* NUMATK_H_ */
