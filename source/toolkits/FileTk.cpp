#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "Logger.h"
#include "ProgArgs.h"
#include "ProgException.h"
#include "toolkits/Base64Encoder.h"
#include "toolkits/FileTk.h"
#include "toolkits/StringTk.h"
#include "toolkits/UnitTk.h"
#include "toolkits/TerminalTk.h"
#include "workers/WorkerException.h"

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

/**
 * Call posix_fadvise() with all advices given in flags on the whole file range.
 *
 * NOTE: See definitions below this function to teach the linker about new template arguments.
 *
 * @fd file descriptor.
 * @progArgsFadvsiseFlags combination of ARG_FADVISE_FLAG_...
 * @path only used for error messages.
 *
 * @throw template EXCEPTION on error.
 */
template <class EXCEPTION>
void FileTk::fadvise(int fd, unsigned progArgsFadviseFlags, const char* path)
{
	int fadviseRes;

	if(!progArgsFadviseFlags)
		return; // nothing to do

    /* note: "dontneed"/"noreuse" is intentionally placed before others because it's sometimes
        combined with seq/rand to first drop cache and then start a new seq/rand read. */

    if(progArgsFadviseFlags & ARG_FADVISE_FLAG_DONTNEED)
    {
        #if defined(__APPLE__)
            fadviseRes = fcntl(fd, F_NOCACHE, 1); // disable caching
        #else // linux
            fadviseRes = posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
        #endif

        IF_UNLIKELY(fadviseRes) // this is special: returns errno instead of setting errno
            throw EXCEPTION(
                std::string("Unable to set POSIX fadvise. ") +
                "Advise: POSIX_FADV_DONTNEED; "
                "File: " + path + "; "
                "SysErr: " + strerror(fadviseRes) );
    }

    if(progArgsFadviseFlags & ARG_FADVISE_FLAG_NOREUSE)
    {
        #if defined(__APPLE__)
            throw EXCEPTION(
                std::string("The given POSIX fadvise is not available on this platform. ") +
                "Advise: POSIX_FADV_NOREUSE; "
                "File: " + path);
        #else // linux
            fadviseRes = posix_fadvise(fd, 0, 0, POSIX_FADV_NOREUSE);
        #endif

        IF_UNLIKELY(fadviseRes) // this is special: returns errno instead of setting errno
            throw EXCEPTION(
                std::string("Unable to set POSIX fadvise. ") +
                "Advise: POSIX_FADV_NOREUSE; "
                "File: " + path + "; "
                "SysErr: " + strerror(fadviseRes) );
    }

	if(progArgsFadviseFlags & ARG_FADVISE_FLAG_SEQ)
	{
        #if defined(__APPLE__)
            fadviseRes = fcntl(fd, F_RDAHEAD, 1); // enable read-ahead
        #else // linux
            fadviseRes = posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
        #endif

		IF_UNLIKELY(fadviseRes) // this is special: returns errno instead of setting errno
			throw EXCEPTION(
				std::string("Unable to set POSIX fadvise. ") +
				"Advise: POSIX_FADV_SEQUENTIAL; "
				"File: " + path + "; "
				"SysErr: " + strerror(fadviseRes) );
	}

	if(progArgsFadviseFlags & ARG_FADVISE_FLAG_RAND)
	{
        #if defined(__APPLE__)
            fadviseRes = fcntl(fd, F_RDAHEAD, 0); // disable read-ahead
        #else // linux
            fadviseRes = posix_fadvise(fd, 0, 0, POSIX_FADV_RANDOM);
        #endif

		IF_UNLIKELY(fadviseRes) // this is special: returns errno instead of setting errno
			throw EXCEPTION(
				std::string("Unable to set POSIX fadvise. ") +
				"Advise: POSIX_FADV_RANDOM; "
				"File: " + path + "; "
				"SysErr: " + strerror(fadviseRes) );
	}

	if(progArgsFadviseFlags & ARG_FADVISE_FLAG_WILLNEED)
	{
        #if defined(__APPLE__)
            throw EXCEPTION(
                std::string("The given POSIX fadvise is not available on this platform. ") +
                "Advise: POSIX_FADV_WILLNEED; "
                "File: " + path);
        #else // linux
            fadviseRes = posix_fadvise(fd, 0, 0, POSIX_FADV_WILLNEED);
        #endif

		IF_UNLIKELY(fadviseRes) // this is special: returns errno instead of setting errno
			throw EXCEPTION(
				std::string("Unable to set POSIX fadvise. ") +
				"Advise: POSIX_FADV_WILLNEED; "
				"File: " + path + "; "
				"SysErr: " + strerror(fadviseRes) );
	}
}

// teach the linker which template instantiation we need so that definition can be in cpp file
template void FileTk::fadvise<ProgException>(int fd, unsigned progArgsFadviseFlags,
	const char* path);
template void FileTk::fadvise<WorkerException>(int fd, unsigned progArgsFadviseFlags,
	const char* path);

/**
 * Call posix_fadvise() with all advices given in flags on the whole file range.
 *
 * NOTE: See definitions below this function to teach the linker about new template arguments.
 *
 * @length as in man mmap().
 * @protect  as in man mmap().
 * @flags as in man mmap().
 * @fd as in man mmap().
 * @progArgsFadvsiseFlags combination of ARG_MADVISE_FLAG_...
 * @path only used for error messages.
 * @return pointer to valid memory mapping; invalid will throw exception.
 *
 * @throw template EXCEPTION on error.
 */
template <class EXCEPTION>
void* FileTk::mmapAndMadvise(size_t length, int protect, int flags, int fd,
	unsigned progArgsMadviseFlags, const char* path)
{
	void* mmapRes = mmap(NULL, length, protect, MAP_SHARED, fd, 0);

	IF_UNLIKELY(mmapRes == MAP_FAILED)
		throw EXCEPTION(
			std::string("Unable to create file memory mapping. ") +
			"Path: " + path + "; "
			"Mapping Size: " + std::to_string(length) + "; "
			"SysErr: " + strerror(errno) );

	// set madvises...

	if(!progArgsMadviseFlags)
		return mmapRes;

	// (try-block for munmap on error)
	try
	{
		int madviseRes;

        /* note: "dontneed" is intentionally placed before others because it's sometimes combined
            with seq/rand to first drop cache and then start a new seq/rand read. */

        if(progArgsMadviseFlags & ARG_MADVISE_FLAG_DONTNEED)
        {
            madviseRes = posix_madvise(mmapRes, length, POSIX_MADV_DONTNEED);

            IF_UNLIKELY(madviseRes) // this is special: returns errno instead of setting errno
                throw EXCEPTION(
                    std::string("Unable to set madvise. ") +
                    "Advise: POSIX_MADV_DONTNEED; "
                    "File: " + path + "; "
                    "SysErr: " + strerror(madviseRes) );
        }

		if(progArgsMadviseFlags & ARG_MADVISE_FLAG_SEQ)
		{
			madviseRes = posix_madvise(mmapRes, length, POSIX_MADV_SEQUENTIAL);

			IF_UNLIKELY(madviseRes) // this is special: returns errno instead of setting errno
				throw EXCEPTION(
					std::string("Unable to set madvise. ") +
					"Advise: POSIX_MADV_SEQUENTIAL; "
					"File: " + path + "; "
					"SysErr: " + strerror(madviseRes) );
		}

		if(progArgsMadviseFlags & ARG_MADVISE_FLAG_RAND)
		{
			madviseRes = posix_madvise(mmapRes, length, POSIX_MADV_RANDOM);

			IF_UNLIKELY(madviseRes) // this is special: returns errno instead of setting errno
				throw EXCEPTION(
					std::string("Unable to set madvise. ") +
					"Advise: POSIX_MADV_RANDOM; "
					"File: " + path + "; "
					"SysErr: " + strerror(madviseRes) );
		}

		if(progArgsMadviseFlags & ARG_MADVISE_FLAG_WILLNEED)
		{
			madviseRes = posix_madvise(mmapRes, length, POSIX_MADV_WILLNEED);

			IF_UNLIKELY(madviseRes) // this is special: returns errno instead of setting errno
				throw EXCEPTION(
					std::string("Unable to set madvise. ") +
					"Advise: POSIX_MADV_WILLNEED; "
					"File: " + path + "; "
					"SysErr: " + strerror(madviseRes) );
		}

		if(progArgsMadviseFlags & ARG_MADVISE_FLAG_HUGEPAGE)
		{
			#ifndef MADV_HUGEPAGE
				throw EXCEPTION("MADV_HUGEPAGE not supported by platform.");
			#else // MADV_HUGEPAGE
				madviseRes = madvise(mmapRes, length, MADV_HUGEPAGE);

				IF_UNLIKELY(madviseRes == -1) // (madvise sets errno, in contrast to posix_madvise)
					throw EXCEPTION(
						std::string("Unable to set madvise. ") +
						"Advise: MADV_HUGEPAGE; "
						"File: " + path + "; "
						"SysErr: " + strerror(errno) );
			#endif // MADV_HUGEPAGE
		}

		if(progArgsMadviseFlags & ARG_MADVISE_FLAG_NOHUGEPAGE)
		{
			#ifndef MADV_HUGEPAGE
				throw EXCEPTION("MADV_NOHUGEPAGE not supported by platform.");
			#else // MADV_HUGEPAGE
				madviseRes = madvise(mmapRes, length, MADV_NOHUGEPAGE);

				IF_UNLIKELY(madviseRes == -1) // (madvise sets errno, in contrast to posix_madvise)
					throw EXCEPTION(
						std::string("Unable to set madvise. ") +
						"Advise: MADV_NOHUGEPAGE; "
						"File: " + path + "; "
						"SysErr: " + strerror(errno) );
			#endif // MADV_HUGEPAGE
		}

		return mmapRes;
	}
	catch(...)
	{
		munmap(mmapRes, length);

		throw;
	}
}

// teach the linker which template instantiation we need so that definition can be in cpp file
template void* FileTk::mmapAndMadvise<ProgException>(size_t length, int protect, int flags,
	int fd, unsigned progArgsMadviseFlags, const char* path);
template void* FileTk::mmapAndMadvise<WorkerException>(size_t length, int protect, int flags,
	int fd, unsigned progArgsMadviseFlags, const char* path);


/**
 *  List all entries under the given path and write them to the given output file in custom tree
 *  file format.
 *
 *  @scanPath path to scan.
 *  @outTreeFilePath path to output file in custom tree format.
 */
void FileTk::scanCustomTree(const ProgArgs& progArgs, std::string scanPath,
            std::string outTreeFilePath)
{
    const bool isLiveStatsDisabled = progArgs.getDisableLiveStats();
    const time_t consoleUpdateIntervalT = 2; // time_t, so unit is seconds
    const time_t startT = time(NULL); // seconds since the epoch (for elapsed time)
    time_t consoleLastUpdateT = startT; // seconds since the epoch (for console updates)

    size_t numFilesFound = 0;
    size_t numDirsFound = 0;
    size_t numBytesFound = 0;

    std::string currentEntry;
    fs::path basePath(scanPath);

    std::ofstream fileStream;

    fileStream.open(outTreeFilePath, std::ofstream::out | std::ofstream::trunc);

    if(!fileStream)
        throw ProgException("Opening tree scan results file failed: " + outTreeFilePath);

    // add base64 encoding header to file
    fileStream << TREEFILE_BASE64ENCODING_HEADER << std::endl;

    // try-block to clean-up console buffering on error
    try
    {
        isLiveStatsDisabled || TerminalTk::disableConsoleBuffering();

        // recursive_directory_iterator walks over entire subdir tree
        for(const fs::directory_entry& dirEntry : fs::recursive_directory_iterator(scanPath) )
        {
            // only write regular files and dirs to tree file, other dir entries get ignored...

            currentEntry = dirEntry.path().lexically_relative(basePath).string();

            if(dirEntry.is_regular_file() )
            {
                numFilesFound++;
                numBytesFound += dirEntry.file_size();

                fileStream << PATHSTORE_FILE_LINE_PREFIX " " << dirEntry.file_size() << " " <<
                    Base64Encoder::encode(currentEntry) << std::endl;
            }
            else
            if(dirEntry.is_directory() )
            {
                numDirsFound++;

                fileStream << PATHSTORE_DIR_LINE_PREFIX " " <<
                    Base64Encoder::encode(currentEntry) << std::endl;
            }

            if( (time(NULL) - consoleLastUpdateT) >= consoleUpdateIntervalT)
            { // time to update console live stats line
                StringTk::eraseControlChars(currentEntry); // avoid '\n' and such in console output

                std::ostringstream stream;
                stream << "Directory scan in progress... "
                    "Dirs: " << UnitTk::numToHumanStrBase10(numDirsFound) << "; "
                    "Files: " << UnitTk::numToHumanStrBase10(numFilesFound) << "; "
                    "Bytes: " << UnitTk::numToHumanStrBase2(numBytesFound) << "; "
                    "Elapsed: " << UnitTk::elapsedSecToHumanStr(time(NULL) - startT) << "; "
                    "Current: " << currentEntry;

                isLiveStatsDisabled || TerminalTk::rewriteConsoleLine(stream.str() );
                consoleLastUpdateT = time(NULL); // update time for next console update
            }
        }
    }
    catch(...)
    {
        isLiveStatsDisabled || TerminalTk::resetConsoleBuffering();
        throw;
    }

    isLiveStatsDisabled || TerminalTk::clearConsoleLine();
    isLiveStatsDisabled || TerminalTk::resetConsoleBuffering();

    std::cout << "NOTE: Directory scan finished. "
        "Files: " << numFilesFound << "; "
        "Dirs: " << numDirsFound << "; "
        "Elapsed: " << (time(NULL) - startT) << "s" << std::endl;
}
