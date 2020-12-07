#ifndef CUFILEHANDLEDATA_H_
#define CUFILEHANDLEDATA_H_

#include <string>
#include "workers/WorkersSharedData.h"

#ifdef CUFILE_SUPPORT
	#include <cufile.h>
#endif


/**
 * Per-file handle for cuFile API
 */
struct CuFileHandleData
{
	#ifdef CUFILE_SUPPORT
		CUfileDescr_t cfr_descr;
		CUfileHandle_t cfr_handle;
		int fd{-1}; // hint for dereg: !=-1 if cuFileHandleReg was successful
	#endif

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
		#ifdef CUFILE_SUPPORT

			memset( (void*)&cfr_descr, 0, sizeof(CUfileDescr_t) );

			cfr_descr.handle.fd = fd;
			cfr_descr.type = CU_FILE_HANDLE_TYPE_OPAQUE_FD;

			CUfileError_t registerRes = cuFileHandleRegister(&cfr_handle,
				&cfr_descr);

			if(registerRes.err != CU_FILE_SUCCESS)
				throw EXCEPTION(std::string(
					"cuFile file handle registration via cuFileHandleRegister failed. ") +
					"cuFile Error: " + CUFILE_ERRSTR(registerRes.err) );

			this->fd = fd;

		#endif // CUFILE_SUPPORT
	}

	/**
	 * Counterpart to deregisterHandle to deregister a file handle. This is safe to call multiple
	 * times or even if registration was not called or if it failed, based on "this->fd == -1".
	 */
	void deregisterHandle()
	{
		#ifdef CUFILE_SUPPORT

			IF_UNLIKELY(fd == -1)
				return;

			cuFileHandleDeregister(cfr_handle);

			fd = -1;

		#endif // CUFILE_SUPPORT
	}
};



#endif /* CUFILEHANDLEDATA_H_ */
