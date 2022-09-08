#include <cstring>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include "FileTk.h"
#include "Logger.h"
#include "ProgException.h"


/**
 * Check if file is empty or not existing.
 *
 * @return true if not exists or empty, false if not empty.
 * @throw ProgException on file access error other than not exists.
 */
bool FileTk::checkFileEmpty(std::string path)
{
	struct stat statBuf;

	int statRes = stat(path.c_str(), &statBuf);
	if(statRes == -1)
	{
		if(errno == ENOENT)
			return true;

		throw ProgException("Getting file size failed. "
			"Path: " + path +
			"SysErr: " + strerror(errno) );
	}

	return (statBuf.st_size == 0);
}

/**
 * Check if a file seems to be sparse or compressed. It seems so when the number of reported
 * allocated blocks by stat() is smaller than it should be for the current file size.
 *
 * @outAllocatedSize the calculated allocated file size based on stat block info.
 * @return true if file seems sparse of compressed, false otherwise.
 */
bool FileTk::checkFileSparseOrCompressed(struct stat& statBuf, off_t& outAllocatedSize)
{
	/* note: st_blocks is not defined by posix, so different platforms may use different block
	 	 sizes. we start with 512, as defined in the linux stat() man page and then check for
	 	 defines that might lead to a different value. */
	off_t statBlockSize = 512;

	#ifdef DEV_BSIZE
		statBlockSize = DEV_BSIZE;
		LOGGER(Log_DEBUG, __func__ << ": " "DEV_BSIZE: " << DEV_BSIZE << std::endl);
	#endif
	#ifdef S_BLKSIZE
		statBlockSize = S_BLKSIZE;
		LOGGER(Log_DEBUG, __func__ << ": " "S_BLKSIZE: " << S_BLKSIZE << std::endl);
	#endif

	outAllocatedSize = statBuf.st_blocks * statBlockSize;

	off_t currentFileSize = statBuf.st_size;

	if(outAllocatedSize < currentFileSize)
		return true; // file seems sparse of compressed

	// file does not seem sparse or compressed
	return false;
}

