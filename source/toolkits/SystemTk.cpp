// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <chrono>
#include <iomanip>
#include <pwd.h>
#include <sstream>
#include <string>
#include <sys/file.h>
#include <sys/types.h>
#include <unistd.h>

#include "SystemTk.h"

/**
 * Get the current date in YYYMMDD format.
 */
std::string SystemTk::getCurrentDateYYYYMMDD()
{
    auto now = std::chrono::system_clock::now();

    std::time_t now_time_t = std::chrono::system_clock::to_time_t(now);

    std::tm* local_tm = std::localtime(&now_time_t);

    // 4. Format the time using std::put_time with %Y%m%d
    std::ostringstream ss;
    ss << std::put_time(local_tm, "%Y%m%d");

    return ss.str();
}

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



