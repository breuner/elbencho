#ifndef TOOLKITS_RANDOM_RANDALGOINTERFACE_H_
#define TOOLKITS_RANDOM_RANDALGOINTERFACE_H_

#include <cstdint>

/**
 * Interface for random number generation algorithms.
 */
class RandAlgoInterface
{
	public:
		virtual ~RandAlgoInterface() {}

		/**
		 * Generate the next 64bit random number.
		 */
		virtual uint64_t next() = 0;

		/**
		 * Fill the entire given buffer with random values.
		 *
		 * Note: The loop code for this is not implemented in RandAlgoInterface directly, because
		 * it's important to avoid the virtual function call overhead for each next 64bit value,
		 * so we have the loop code in each subclass.
		 */
		virtual void fillBuf(char* buf, uint64_t bufLen) = 0;

	protected:
		RandAlgoInterface() {}
};

#endif /* TOOLKITS_RANDOM_RANDALGOINTERFACE_H_ */
