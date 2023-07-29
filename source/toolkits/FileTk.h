#ifndef TOOLKITS_FILETK_H_
#define TOOLKITS_FILETK_H_

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "Common.h"

class FileTk
{
	public:
		static bool checkFileEmpty(std::string path);
		static bool checkFileSparseOrCompressed(struct stat& statBuf, off_t& outAllocatedSize);
		static int mkdiratBottomUp(int dirFD, const char* path, mode_t mode);
		template <class EXCEPTION>
		static void fadvise (int fd, unsigned progArgsFadviseFlags, const char* path);
		template <class EXCEPTION>
		static void* mmapAndMadvise(size_t length, int protect, int flags, int fd,
			unsigned progArgsMadviseFlags, const char* path);

	private:
		FileTk() {}
};



#endif /* TOOLKITS_FILETK_H_ */
