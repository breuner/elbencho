// SPDX-FileCopyrightText: 2020-2025 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <unistd.h>
#include <vector>

#include "OpsLogger.h"
#include "ProgArgs.h"


// size of the stack buffer for a single log line; a longer line falls back to the heap
#define OPSLOGFILE_LINE_BUF_LEN     1024


namespace
{
    /**
     * Format a single ops log line into the given buffer.
     *
     * @return number of chars needed for the line excluding the terminating zero, as returned
     *      by snprintf(), i.e. a value greater than or equal to bufLen means that the given
     *      buffer was too small and the line got truncated.
     */
    int formatOpLogLine(char* buf, size_t bufLen, const char* dateStr, ssize_t workerRank,
        const char* opName, const char* entryName, uint64_t offset, uint64_t length,
        bool isOpFinished, bool isError)
    {
        return snprintf(buf, bufLen,
            "{ "
            "\"date\": \"%s\", "
            "\"worker_rank\": %zd, "
            "\"op_name\": \"%s\", "
            "\"entry_name\": \"%s\", "
            "\"offset\": %" PRIu64 ", "
            "\"length\": %" PRIu64 ", "
            "\"is_finished\": %s, "
            "\"is_error\": %s "
            "}\n",
            dateStr, workerRank, opName, entryName, offset, length,
            isOpFinished ? "true" : "false", isError ? "true" : "false");
    }
}

/**
 * Open the log file. This has to be called before any op can be logged. This is a no-op
 * if ops logging is not enabled or if log file is already open. Log file close is done in
 * constructor but can also be done earlier through closeLogFile().
 *
 * @throw WorkerException on error.
 */
void OpsLogger::openLogFile()
{
	if(progArgs->getOpsLogPath().empty() || (logFileFD != -1) )
		return;

	logFileFD = open(progArgs->getOpsLogPath().c_str(), O_WRONLY | O_CREAT | O_APPEND,
		OPSLOGFILE_MODE);

	if(logFileFD == -1)
		throw WorkerException("Unable to open ops log file: " +
			progArgs->getOpsLogPath() + "; "
			"SysErr: " + strerror(errno) );
}

/**
 * Close the log file. Further ops cannot be logged unless the file is explicitly opened
 * again.
 */
void OpsLogger::closeLogFile()
{
	if(logFileFD == -1)
		return;

	close(logFileFD);

	logFileFD = -1;
}

/**
 * Log this operation as a single line JSON document. This requires a previous call to
 * openLogFile().
 *
 * Most likely you don't want to call this directly. Call the OPLOG() macro instead.
 *
 * @opName the name of the I/O operation to log (e.g. open, read, s3put).
 * @entryName the name of the dir/file/object to which the operation is applied.
 * @offset the offset of the operation within the file/object if applicable, e.g. for a read
 * 		operation.
 * @length the length of the operation within the file/object if applicable, e.g. for a read
 * 		operation.
 * @isOpFinished false if this is the log entry before the operation start, true if the
 * 		operation is finished.
 * @isSuccess true if this operation completed successfully; only meaningful if isOpFinished
 * 		is true.
 *
 * @throw WorkerException on error.
 */
void OpsLogger::logOpJSON(std::string opName, std::string entryName,
	uint64_t offset, uint64_t length, bool isOpFinished, bool isError)
{
	if(!isEnabled() )
		return;

	if(progArgs->getUseOpsLogLocking() )
		flock(logFileFD, LOCK_EX);

	auto now = std::chrono::system_clock::now();
	time_t time = std::chrono::system_clock::to_time_t(now);
	auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
		now.time_since_epoch()).count() % 1000;

    struct tm localTimeInfo;
    localtime_r(&time, &localTimeInfo);

	std::stringstream dateStream;
	dateStream << std::put_time(&localTimeInfo, "%FT%T") << "."
		<< std::setfill('0') << std::setw(3) << milliseconds
		<< std::put_time(&localTimeInfo, "%z");

    /* the complete line is formatted first and then submitted through a single write(),
        because a single write() to a file which was opened with O_APPEND cannot be
        interleaved with the writes of other threads or processes, whereas multiple writes
        per line can and would result in a log file that is not parsable anymore. (this is
        why printf-style functions must not be used here: they are free to split their
        output into multiple writes, e.g. one per conversion.) */

    char stackBuf[OPSLOGFILE_LINE_BUF_LEN];
    std::vector<char> heapBuf; // only used for a line that does not fit into stackBuf
    char* lineBuf = stackBuf;

    const std::string dateStr(dateStream.str() );

    int lineLen = formatOpLogLine(lineBuf, sizeof(stackBuf), dateStr.c_str(), workerRank,
        opName.c_str(), entryName.c_str(), offset, length, isOpFinished, isError);

    /* entry names can be long, e.g. a deeply nested path or S3 key, so a line which does not
        fit gets formatted again on a larger buffer instead of logging a truncated and thus
        invalid json line */
    if( (lineLen >= 0) && ( (size_t)lineLen >= sizeof(stackBuf) ) )
    {
        heapBuf.resize( (size_t)lineLen + 1);
        lineBuf = heapBuf.data();

        lineLen = formatOpLogLine(lineBuf, heapBuf.size(), dateStr.c_str(), workerRank,
            opName.c_str(), entryName.c_str(), offset, length, isOpFinished, isError);
    }

    /* a partial write can only be the result of an error or a signal interruption, in which
        case the rest of the line is submitted as a best effort */
    size_t numWritten = 0;

    while( (lineLen > 0) && (numWritten < (size_t)lineLen) )
    {
        ssize_t writeRes = write(logFileFD, &lineBuf[numWritten], (size_t)lineLen - numWritten);

        if(writeRes > 0)
            numWritten += (size_t)writeRes;
        else
        if( (writeRes == -1) && (errno == EINTR) )
            continue; // interrupted, so try again
        else
            break; // write error or no progress at all, so there is nothing left to do here
    }

	if(progArgs->getUseOpsLogLocking() )
		flock(logFileFD, LOCK_UN);
}

