#ifndef LOGGER_H_
#define LOGGER_H_

#include <iostream>
#include <mutex>
#include <sstream>

#define LOGGER(logLevel, stream) 	do { Logger(logLevel) << stream; } while(0)
#define ERRLOGGER(logLevel, stream) do { ErrLogger(logLevel, true) << stream; } while(0)

#ifdef BUILD_DEBUG
#define LOGGER_DEBUG_BUILD(stream)	LOGGER(Log_DEBUG, stream)
#else // no debug build
#define LOGGER_DEBUG_BUILD(stream)	/* nothing to do if not a debug build */
#endif

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

			switch(logLevel)
			{
				case Log_VERBOSE: std::cout << "VERBOSE: " << msg; break;
				case Log_DEBUG: std::cout << "DEBUG: " << msg; break;
				default: std::cout << msg;
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
				errHistoryStream << prefixStr << msg;

			if(logToConsole)
				std::cerr << prefixStr << msg;
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
