#ifndef TOOLKITS_OPSLOGGER_H_
#define TOOLKITS_OPSLOGGER_H_

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <sys/file.h>

#include "Common.h"
//#include "ProgArgs.h"
#include "workers/WorkerException.h"
#include "workers/WorkersSharedData.h"


#define OPSLOGFILE_MODE		(S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH)

// macro to avoid function call overhead if ops logging is not enabled
#define OPLOG(opsLogger, opName, entryName, offset, length, isOpFinished, isError) \
	do { \
		IF_UNLIKELY(opsLogger.isEnabled() ) \
		{ \
			opsLogger.logOpJSON(opName, entryName, offset, length, isOpFinished, \
				isError); \
		} \
	} while(0)

// convenience wrappers to log before/after an op
#define OPLOG_PRE_OP(opName, entryName, offset, length) \
            OPLOG(opsLog, opName, entryName, offset, length, false, false)
#define OPLOG_POST_OP(opName, entryName, offset, length, isError) \
            OPLOG(opsLog, opName, entryName, offset, length, true, isError)


class ProgArgs; // forward declaration to avoid cyclic #include


/**
 * Log file writer for IO operations.
 */
class OpsLogger
{
	public:
		/**
		 * We don't want to throw an exception here if the log file can't be opened, so the
		 * openLogFile method needs to be called before this object can be used.
		 *
		 * @workerRank rank of the worker to which this logger belongs or "-1" if owner is not a
		 *      worker thread.
		 */
		OpsLogger(const ProgArgs* progArgs, ssize_t workerRank) :
		    progArgs(progArgs), workerRank(workerRank) {}

		~OpsLogger()
		{
			closeLogFile();
		}

		void openLogFile();
		void closeLogFile();
		void logOpJSON(std::string opName, std::string entryName,
			uint64_t offset, uint64_t length, bool isOpFinished, bool isError);


	private:
		const ProgArgs* progArgs;
		ssize_t workerRank{-1};
		int logFileFD{-1};

		// inliners
	public:
		/**
         * Check if ops logging is enabled. This is an efficient check for the OPLOG() macro.
         *
         * @return true if ops logging is enabled.
         */
		inline bool isEnabled() const
		{
			return (logFileFD != -1);
		}

};

#endif /* TOOLKITS_OPSLOGGER_H_ */
