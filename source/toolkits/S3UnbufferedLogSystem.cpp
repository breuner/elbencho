// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifdef S3_SUPPORT

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <aws/core/utils/DateTime.h>

#include "Logger.h"
#include "toolkits/S3UnbufferedLogSystem.h"

namespace
{
    const mode_t S3_SDK_LOGFILE_MODE = (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);

    std::string makeLogFilePath(const std::string& filenamePrefix)
    {
        return filenamePrefix +
            std::string(Aws::Utils::DateTime::CalculateGmtTimestampAsString("%Y-%m-%d-%H").c_str() ) +
            ".log";
    }
}

/**
 * @param logLevel AWS SDK log level.
 * @param filenamePrefix directory and name prefix; UTC hour stamp and ".log" are appended.
 */
S3UnbufferedLogSystem::S3UnbufferedLogSystem(Aws::Utils::Logging::LogLevel logLevel,
    const std::string& filenamePrefix)
    : FormattedLogSystem(logLevel), filenamePrefix(filenamePrefix)
{
    std::lock_guard<std::mutex> lock(mutex);
    openLogFileUnlocked();
}

S3UnbufferedLogSystem::~S3UnbufferedLogSystem()
{
    std::lock_guard<std::mutex> lock(mutex);
    closeLogFile();
}

/**
 * Close the current log fd if open. Caller must hold mutex.
 */
void S3UnbufferedLogSystem::closeLogFile()
{
    if(fd < 0)
        return;

    close(fd);
    fd = -1;
}

/**
 * Open (or reopen) the hour-stamped log file. Caller must hold mutex.
 *
 * @return true if fd is valid afterwards.
 */
bool S3UnbufferedLogSystem::openLogFileUnlocked()
{
    const std::string path = makeLogFilePath(filenamePrefix);
    if( (fd >= 0) && (path == currentPath) )
        return true;

    const bool pathChanged = (path != currentPath);

    closeLogFile();
    currentPath = path;

    fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, S3_SDK_LOGFILE_MODE);
    if(fd < 0)
    {
        if(pathChanged)
            ERRLOGGER(Log_NORMAL, "Unable to open AWS S3 SDK log file: " << path << "; "
                "SysErr: " << strerror(errno) << std::endl);

        return false;
    }

    return true;
}

/**
 * Write one already-formatted SDK log line. Reopens the file when the UTC hour (and thus the
 * filename) changes.
 */
void S3UnbufferedLogSystem::ProcessFormattedStatement(Aws::String&& statement)
{
    const int errnoCopy = errno;

    std::lock_guard<std::mutex> lock(mutex);

    if(statement.empty() || !openLogFileUnlocked() )
    {
        errno = errnoCopy;
        return;
    }

    const char* ptr = statement.c_str();
    size_t remaining = statement.size();

    while(remaining > 0)
    {
        const ssize_t written = write(fd, ptr, remaining);
        if(written < 0)
        {
            if(errno == EINTR)
                continue;

            break;
        }

        ptr += written;
        remaining -= (size_t)written;
    }

    errno = errnoCopy;
}

#endif // S3_SUPPORT
