#include <cstring>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include "FileTk.h"
#include "Logger.h"
#include "ProgException.h"

#if __has_include(<filesystem>)
  #include <filesystem>
  namespace fs = std::filesystem;
#elif __has_include(<experimental/filesystem>)
  #include <experimental/filesystem>
  namespace fs = std::experimental::filesystem;
#else
  error "Missing the <filesystem> header."
#endif

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

/**
 * Try to create the given dir. If this fails because the parents at higher levels don't exist,
 * then try to create the parents from deepest level bottom up to highest level. (Bottom up because
 * this is intended to be used in scenarios where multiple threads might call this, so the higher
 * the parent, the higher the chances that it already exists.)
 *
 * return 0 on success, "-1" and errno on error similar to mkdirat().
 */
int FileTk::mkdiratBottomUp(int dirFD, const char* path, mode_t mode)
{
	int mkdirRes = mkdirat(dirFD, path, mode);

	if(!mkdirRes || (errno == EEXIST) )
		return mkdirRes;

	if(errno == ENOENT)
	{ // parent doesn't exist yet
		fs::path pathObj(path);
		fs::path parentPathObj = pathObj.parent_path();
		if( (parentPathObj == pathObj) || (pathObj.string() == "") )
		{ // we reached the root
			errno = ENOENT;
			return -1;
		}

		// we haven't reached the root of the path yet, so try to create the next parent

		int mkdirParentRes = mkdiratBottomUp(dirFD, parentPathObj.string().c_str(), mode);
		if( (mkdirParentRes == -1) && (errno != EEXIST) )
			return mkdirParentRes; // parent creation failed

		// parent creation succeeded, so try again to create actual dir
		return mkdirat(dirFD, path, mode);
	}

	// any error that's not ENOENT and not EEXIST
	return mkdirRes;
}
