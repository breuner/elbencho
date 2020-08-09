#ifdef CUDA_SUPPORT
	#include <cuda_runtime.h>
#endif

#ifdef CUFILE_SUPPORT
	#include <cufile.h>
#endif

/**
 * Minimal program to be built as part of auto detection of available system libs etc.
 */
int main(int argc, char** argv)
{
	return 0;
}
