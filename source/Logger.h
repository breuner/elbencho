#ifndef LOGGER_H_
#define LOGGER_H_

#include <iostream>
#include <mutex>
#include <sstream>
#include <sys/time.h>

#define LOGGER(logLevel, stream) 	do { Logger(logLevel) << stream; } while(0)
#define ERRLOGGER(logLevel, stream) do { ErrLogger(logLevel, true) << stream; } while(0)

#ifdef BUILD_DEBUG
#define LOGGER_DEBUG_BUILD(stream)	LOGGER(Log_DEBUG, stream)
#else // no debug build
#define LOGGER_DEBUG_BUILD(stream)	/* nothing to do if not a debug build */
#endif

/**
 * Note: Log_NORMAL must be lowest level, so that we can test via "if(level > Log_NORMAL)"
 */
enum LogLevel
{
	Log_NORMAL=0,
	Log_VERBOSE=1,
	Log_DEBUG=2,
};

/**
 * Despite being a normal logger, this also is a thread-safe central history for errors that might
 * occur e.g. in Worker threads and later need to be queried in a different thread, such as the http
 * server which wants to report worker error messages to the http client. The history is also
 * relevant locally, as errors in workers during ncurses full screen live stats would get erased
 * from console when ncurses mode ends.
 */
class LoggerBase
{
	protected:
		static std::mutex mutex;
		static std::stringstream errHistoryStream;
		static bool keepErrHistory; // true to keep errors in errHistoryStream
		static LogLevel filterLevel; // messages with level higher than this will not be printed

		LoggerBase() {};

	// inliners
	public:
		/**
		 * @logLevel messages with level higher than this will not be logged.
		 */
		static void setFilterLevel(LogLevel filterLevel)
		{
			std::unique_lock<std::mutex> lock(mutex); // L O C K (scoped)

			LoggerBase::filterLevel = filterLevel;
		}

		static void enableErrHistory()
		{
			std::unique_lock<std::mutex> lock(mutex); // L O C K (scoped)

			keepErrHistory = true;
		}

		static void clearErrHistory()
		{
			std::unique_lock<std::mutex> lock(mutex); // L O C K (scoped)

			errHistoryStream.str(std::string() );
		}

		static std::string getErrHistory()
		{
			std::unique_lock<std::mutex> lock(mutex); // L O C K (scoped)

			std::string tmpStr(errHistoryStream.str() );

			return tmpStr;
		}

		static void logMsg(LogLevel logLevel, std::string msg)
		{
			std::unique_lock<std::mutex> lock(mutex); // L O C K (scoped)

			// (note: mutex is also to sync console output if multiple threads log concurrently)

			if(logLevel > filterLevel)
				return; // nothing to log

			// add timestamp as prefix to normal log msgs only if log level is above normal
			std::string timestampPrefixStr;

			if(filterLevel > Log_NORMAL)
				timestampPrefixStr = getTimestamp() + " ";

			switch(logLevel)
			{
				case Log_VERBOSE: std::cout << timestampPrefixStr << "VERBOSE: " << msg; break;
				case Log_DEBUG: std::cout << timestampPrefixStr << "DEBUG: " << msg; break;
				default: std::cout << timestampPrefixStr << msg;
			}
		}

		static void logErrMsg(LogLevel logLevel, bool logToConsole, bool logPrefix, std::string msg)
		{
			std::unique_lock<std::mutex> lock(mutex); // L O C K (scoped)

			// (note: mutex is also to sync console output if multiple threads log concurrently)

			if(logLevel > filterLevel)
				return; // nothing to log

			std::string prefixStr;

			if(logPrefix)
			{
				switch(logLevel)
				{
					case Log_VERBOSE: prefixStr = "ERROR VERBOSE: "; break;
					case Log_DEBUG: prefixStr = "ERROR DEBUG: "; break;
					default: prefixStr = "ERROR: "; break;
				}
			}

			if(keepErrHistory)
				errHistoryStream << getTimestamp() << " " << prefixStr << msg;

			if(logToConsole)
				std::cerr << getTimestamp() << " " << prefixStr << msg;
		}

	private:
		/**
		 * Returns a short string representing current time of day in the format hh:mm:ss.ms ("ms"
		 * as width 3).
		 */
		static std::string getTimestamp()
		{
			timeval currentTime;
			gettimeofday(&currentTime, NULL);
			int milliSecs = currentTime.tv_usec / 1000;

			char strBuf[16]; // (16 is just long enough for the string)
			strftime(strBuf, sizeof(strBuf), "%H:%M:%S", localtime(&currentTime.tv_sec) );

			std::string resultStr(strBuf);

			snprintf(strBuf, sizeof(strBuf), ".%03d", milliSecs);

			resultStr += strBuf;

			return resultStr;
		}
};

/**
 * Logger for standard (non-error) messages. Note that messages are only printed in destructor,
 * hence the LOGGER macro.
 */
class Logger : public LoggerBase, public std::ostringstream
{
	public:
		Logger(LogLevel logLevel = Log_NORMAL) : LoggerBase(), logLevel(logLevel) {};

		~Logger()
		{
			if(!this->str().empty() )
				logMsg(logLevel, this->str() );
		}

	private:
		LogLevel logLevel; // level for msg that is being logged now

	// inliners
	public:
		void flush()
		{
			logMsg(logLevel, this->str() );
			this->str(std::string() );
		}
};

/**
 * Logger for error messages. Note that messages are only printed in destructor, hence the ERRLOGGER
 * macro.
 */
class ErrLogger : public LoggerBase, public std::ostringstream
{
	public:
		ErrLogger(LogLevel logLevel = Log_NORMAL, bool logToConsole = true, bool logPrefix = true) :
			LoggerBase(), logLevel(logLevel), logToConsole(logToConsole), logPrefix(logPrefix) {};

		~ErrLogger()
		{
			if(!this->str().empty() )
				logErrMsg(logLevel, logToConsole, logPrefix, this->str() );
		}

	private:
		LogLevel logLevel; // level for msg that is being logged now
		bool logToConsole; // whether to log msg only to history (=false) or also to console (=true)
		bool logPrefix; // whether to add the "ERROR: " prefix (=true) or not (=false)

	// inliners
	public:
		void flush()
		{
			logErrMsg(logLevel, logToConsole, logPrefix, this->str() );
			this->str(std::string() );
		}
};

#endif /* LOGGER_H_ */
