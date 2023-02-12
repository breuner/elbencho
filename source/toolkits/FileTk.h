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

	private:
		FileTk() {}
};



#endif /* TOOLKITS_FILETK_H_ */
