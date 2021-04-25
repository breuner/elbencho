#include <pwd.h>
#include <string>
#include <sys/file.h>
#include <sys/types.h>
#include <unistd.h>
#include "SystemTk.h"


/**
 * Get effective username of this process. If username can't be retrieved, then return "u" plus
 * numeric user ID (e.g. "u1000").
 *
 * @return username
 */
std::string SystemTk::getUsername()
{
	uid_t userID = geteuid();

	std::string username = "u" + std::to_string(userID); // numeric user ID as fallback

	// try to get username

	struct passwd passwdEntry;
	struct passwd* passwdResultPointer; // points to passwdEntry on success

	long bufSize = sysconf(_SC_GETPW_R_SIZE_MAX);
	if(bufSize == -1)
		bufSize = (16*1024); // fallback to 16KiB if no system default is given

	char* buffer = (char*)malloc(bufSize);
	if(!buffer)
		return username;

	getpwuid_r(userID, &passwdEntry, buffer, bufSize, &passwdResultPointer);
	if(!passwdResultPointer)
	{ // getting username failed
		free(buffer);
		return username;
	}

	username = passwdEntry.pw_name;

	free(buffer);

	return username;
}



