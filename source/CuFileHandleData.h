#ifndef CUFILEHANDLEDATA_H_
#define CUFILEHANDLEDATA_H_

#include <string>
#include "WorkersSharedData.h"


/**
 * Per-file handle for cuFile API
 */
struct CuFileHandleData
{
	/**
	 * cuFile handle register as preparation for cuFileRead/Write. Call cuFileHandleDereg when done
	 * with file access.
	 *
	 * @fd posix file descriptor to register for cuFile access.
	 * @throw template EXCEPTION if registration fails (e.g. WorkerException or ProgException).
	 */
	template <class EXCEPTION>
	void registerHandle(int fd)
	{
	}

	/**
	 * Counterpart to deregisterHandle to deregister a file handle. This is safe to call multiple
	 * times or even if registration was not called or if it failed, based on "this->fd == -1".
	 */
	void deregisterHandle()
	{
	}
};



#endif /* CUFILEHANDLEDATA_H_ */
