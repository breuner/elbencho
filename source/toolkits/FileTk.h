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

	private:
		FileTk() {}
};



#endif /* TOOLKITS_FILETK_H_ */
