#include <cstring>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include "FileTk.h"
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



