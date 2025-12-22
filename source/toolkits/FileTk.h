// SPDX-FileCopyrightText: 2020-2025 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef TOOLKITS_FILETK_H_
#define TOOLKITS_FILETK_H_

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "Common.h"

class ProgArgs; // forward declaration


class FileTk
{
	public:
		static bool checkFileEmpty(std::string path);
		static bool checkFileSparseOrCompressed(struct stat& statBuf, off_t& outAllocatedSize);
		static int mkdiratBottomUp(int dirFD, const char* path, mode_t mode);
		template <class EXCEPTION>
		static void fadvise(int fd, unsigned progArgsFadviseFlags, const char* path);
		template <class EXCEPTION>
		static void* mmapAndMadvise(size_t length, int protect, int flags, int fd,
			unsigned progArgsMadviseFlags, const char* path);
		static void scanCustomTree(const ProgArgs& progArgs, std::string scanPath,
		    std::string outTreeFilePath);

	private:
		FileTk() {}


    // inliners
	public:
		/**
		 * Call posix_fadvise() with all advices given in flags on the whole file range.
		 *
		 * @fd file descriptor.
		 * @progArgsFLockType ARG_FLOCK_x.
		 * @offset offset of next read/write operation.
		 * @len length of next read/write operation.
		 * @isWrite true if next op is a write operation; value ignored for isUnlock==true.
		 * @isUnlock true if op under lock is done, so we need to unlock.
		 * @path only used for error messages.
		 *
		 * @throw template EXCEPTION on error.
		 */
        template <class EXCEPTION>
        static void flock(const int fd, const unsigned progArgsFLockType, const uint64_t offset,
            const uint64_t len, const bool isWrite, const bool isUnlock, const char* path)
        {
            switch(progArgsFLockType)
            {
                default:
                case ARG_FLOCK_NONE:
                {
                    // nothing to do, no file locking requested
                } return;

                case ARG_FLOCK_RANGE:
                {
                    /* note: no memset(0) needed for struct flock according to example here:
                       https://pubs.opengroup.org/onlinepubs/9699919799/functions/fcntl.html */
                    struct flock flockDetails;

                    flockDetails.l_type = isUnlock ? F_UNLCK : (isWrite ? F_WRLCK : F_RDLCK);
                    flockDetails.l_whence = SEEK_SET;
                    flockDetails.l_start = offset;
                    flockDetails.l_len = len;

                    int fcntlRes = fcntl(fd, F_SETLKW, &flockDetails);

                    IF_UNLIKELY(fcntlRes == -1)
                        throw EXCEPTION(
                            std::string("File range lock operation failed. ") +
                            "FD: " + std::to_string(fd) + "; "
                            "Offset: " + std::to_string(offset) + "; "
                            "Length: " + std::to_string(len) + "; "
                            "LockType: " + (isUnlock ? "unlock" :
                                (isWrite ? "write" : "read") ) + "; "
                            "File: " + (path ? std::string(path) : std::string("<unknown>") ) + "; "
                            "SysErr: " + strerror(errno) );
                } return;

                case ARG_FLOCK_FULL:
                {
                    /* note: no memset(0) needed for struct flock according to example here:
                       https://pubs.opengroup.org/onlinepubs/9699919799/functions/fcntl.html */
                    struct flock flockDetails;

                    flockDetails.l_type = isUnlock ? F_UNLCK : (isWrite ? F_WRLCK : F_RDLCK);
                    flockDetails.l_whence = SEEK_SET;
                    flockDetails.l_start = 0;
                    flockDetails.l_len = 0; // len 0 has special meaning to lock whole file

                    int fcntlRes = fcntl(fd, F_SETLKW, &flockDetails);

                    IF_UNLIKELY(fcntlRes == -1)
                        throw EXCEPTION(
                            std::string("Full file lock operation failed. ") +
                            "FD: " + std::to_string(fd) + "; "
                            "LockType: " + (isUnlock ? "unlock" :
                                (isWrite ? "write" : "read") ) + "; "
                            "File: " + (path ? std::string(path) : std::string("<unknown>") ) + "; "
                            "SysErr: " + strerror(errno) );
                } return;
            }
        }

};



#endif /* TOOLKITS_FILETK_H_ */
