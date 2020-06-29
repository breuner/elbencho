#ifndef LOCALWORKER_H_
#define LOCALWORKER_H_

#include <ctype.h>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <random>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "FileOffsetGenerator.h"
#include "Worker.h"

// io_prep_pwrite or io_prep_read from libaio
typedef void AIO_RW_PREPPER(struct iocb* iocb, int fd, void* buf, size_t count, long long offset);

// pread or pwrite (as in "man 2 pread")
typedef ssize_t POSITIONAL_RW(int __fd, void* __buf, size_t __nbytes, __off_t __offset);


/**
 * Each worker represents a single thread performing local I/O.
 */
class LocalWorker : public Worker
{
	public:
		explicit LocalWorker(WorkersSharedData* workersSharedData, size_t workerRank) :
			Worker(workersSharedData, workerRank) {}

		~LocalWorker();


	private:
	    std::mt19937_64 randGen{std::random_device()() }; // random_device is just for random seed

		void* ioBuf{NULL}; // buffer used for block-sized read()/write()

		virtual void run() override;

		void finishPhase();
		void allocIOBuffer();
		template <POSITIONAL_RW positional_rw>
		ssize_t rwBlockSized(int fd, FileOffsetGenerator& offsetGen);
		template <POSITIONAL_RW positional_rw>
			ssize_t rwBlockSized(int fd, size_t count, size_t offset);
		template <POSITIONAL_RW positional_rw>
			ssize_t rwRandomBlockSized(int fd, size_t len, size_t offset);
		template <POSITIONAL_RW positional_rw>
			ssize_t rwRandomAlignedBlockSized(int fd, size_t len, size_t offset);
		template <AIO_RW_PREPPER aio_rw_prepper>
			ssize_t aioBlockSized(int fd, FileOffsetGenerator& offsetGen);
		template <AIO_RW_PREPPER aio_rw_prepper>
			ssize_t aioBlockSized(int fd, size_t count, size_t offset);
		void iterateDirs();
		void dirModeIterateFiles();
		void fileModeIterateFiles();
		int getDirModeOpenFlags(BenchPhase benchPhase);

		// inliners
	private:
		/**
		 * Wrapper for member function templates to have the same signature for pread and pwrite.
		 */
		static ssize_t preadWrapper(int __fd, void* __buf, size_t __nbytes, __off_t __offset)
			{ return pread(__fd, __buf, __nbytes, __offset); }

		/**
		 * Wrapper for member function templates to have the same signature for pread and pwrite.
		 */
		static ssize_t pwriteWrapper(int __fd, void* __buf, size_t __nbytes, __off_t __offset)
			{ return pwrite(__fd, __buf, __nbytes, __offset); }

};


#endif /* LOCALWORKER_H_ */
