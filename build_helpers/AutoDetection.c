#ifdef BACKTRACE_SUPPORT
	#include <boost/stacktrace.hpp>
#endif

#ifdef CUDA_SUPPORT
	#include <cuda_runtime.h>
	#include <curand.h>
#endif

#ifdef CUFILE_SUPPORT
	#include <cufile.h>
#endif

#ifdef HIP_SUPPORT
    #include <hip/hip_runtime_api.h>
    #include <hiprand/hiprand.h>
#endif

#ifdef HIPFILE_SUPPORT
    #include <hipfile.h>
#endif

/**
 * Minimal program to be built as part of auto detection of available system libs etc.
 */
int main(int argc, char** argv)
{
	return 0;
}
