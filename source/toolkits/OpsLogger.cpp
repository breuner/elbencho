#include "OpsLogger.h"
#include "ProgArgs.h"

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
 * @workerRank rank of LocalWorker thread.
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

	std::stringstream dateStream;
	dateStream << std::put_time(std::localtime(&time), "%FT%T") << "."
		<< std::setfill('0') << std::setw(3) << milliseconds
		<< std::put_time(std::localtime(&time), "%z");

	dprintf(logFileFD,
		"{ "
		"\"date\": \"%s\", "
		"\"worker_rank\": %zu, "
		"\"op_name\": \"%s\", "
		"\"entry_name\": \"%s\", "
		"\"offset\": %" PRIu64 ", "
		"\"length\": %" PRIu64 ", "
		"\"is_finished\": %s, "
		"\"is_error\": %s "
		"}\n",
		dateStream.str().c_str(), workerRank, opName.c_str(), entryName.c_str(), offset,
		length, isOpFinished ? "true" : "false", isError ? "true" : "false");

	if(progArgs->getUseOpsLogLocking() )
		flock(logFileFD, LOCK_UN);
}

