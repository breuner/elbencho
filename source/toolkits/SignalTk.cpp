// SPDX-FileCopyrightText: 2020-2025 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <csignal>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include "Common.h"
#include "ProgArgs.h"
#include "ProgException.h"
#include "SignalTk.h"

#ifdef SYSCALLH_SUPPORT
	#include <sys/syscall.h>
#endif

#ifdef BACKTRACE_SUPPORT
    #include <boost/stacktrace.hpp>
#endif

#define SIGNALTK_BACKTRACE_PATH			"/tmp/" EXE_NAME "_fault_trace.txt"
#define SIGNALTK_BACKTRACE_FILEMODE		(S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH)


/**
 * Register handler for SIGSEGV and similar.
 */
void SignalTk::registerFaultSignalHandlers(const ProgArgs& progArgs)
{
	/* don't register fault signal handlers with local libhdfs usage, because JVM uses them:
		https://docs.oracle.com/javase/10/troubleshoot/handle-signals-and-exceptions.htm#JSTGD354 */
	if(progArgs.getUseHDFS() && progArgs.getHostsStr().empty() )
		return;

	std::signal(SIGSEGV, SignalTk::faultSignalHandler);
	std::signal(SIGFPE, SignalTk::faultSignalHandler);
	std::signal(SIGBUS, SignalTk::faultSignalHandler);
	std::signal(SIGILL, SignalTk::faultSignalHandler);
	std::signal(SIGABRT, SignalTk::faultSignalHandler);
}

/**
 * Handler for SIGSEGV and similar to try to print useful info like a backtrace in such cases.
 * This will call exit(EXIT_FAILURE) at the end to terminate the application immediately.
 *
 * @sig received signal number (SIGSEGV etc.).
 */
void SignalTk::faultSignalHandler(int sig)
{
	std::ostringstream stream;

	switch (sig)
	{
		case SIGSEGV:
		{
			stream << "FAULT HANDLER (PID " << getpid() << " / TID " << getThreadID() << "): "
				"Segmentation fault" << std::endl;
		} break;

		case SIGFPE:
		{
			stream << "FAULT HANDLER (PID " << getpid() << " / TID " << getThreadID() << "): "
				"Floating point exception" << std::endl;
		} break;

		case SIGBUS:
		{
			stream << "FAULT HANDLER (PID " << getpid() << " / TID " << getThreadID() << "): "
				"Bus error (bad memory access)" << std::endl;
			stream << "NOTE: If this happened with mmap, then you probably forgot to grow the "
				"files to their full size, e.g. via \"--" << ARG_TRUNCTOSIZE_LONG << "\" or "
				"\"--" << ARG_PREALLOCFILE_LONG "\"." << std::endl;
		} break;

		case SIGILL:
		{
			stream << "FAULT HANDLER (PID " << getpid() << " / TID " << getThreadID() << "): "
				"Illegal instruction" << std::endl;
		} break;

		case SIGABRT:
		{ // note: SIGABRT is special: after signal handler returns the process dies immediately
			std::cerr << "FAULT HANDLER (PID " << getpid() << " / TID " << getThreadID() << "): "
				"Abnormal termination" << std::endl;
		} break;

		default:
		{
			std::cerr << "FAULT HANDLER (PID " << getpid() << " / TID " << getThreadID() << "): "
				"Received an unknown signal: " << std::to_string(sig) << std::endl;
		} break;
	}

	std::string backtraceStr = logBacktrace();

	std::signal(sig, SIG_DFL); /* Reset the handler to its default only after we're done with
		backtrace logging, otherwise the next caller might terminate before we logged. */

	throw ProgException(stream.str() + backtraceStr);
}

/**
 * Block SIGINT/SIGTERM signals for the calling pthread.
 *
 * Linux can direct a process-directed signal to any thread that doesn't block it. So this is to
 * ensure that only the main thread and not other threads receive a SIGINT, e.g. if a user presses
 * ctrl+c.
 *
 * A new thread inherits a copy of its creator's signal mask.
 */
bool SignalTk::blockInterruptSignals()
{
	sigset_t signalMask; // mask of signals to block

	sigemptyset(&signalMask);

	sigaddset(&signalMask, SIGINT);
	sigaddset(&signalMask, SIGTERM);

	int sigmaskRes = pthread_sigmask(SIG_BLOCK, &signalMask, NULL);

	return (sigmaskRes == 0);
}

/**
 * Unblock the signals (e.g. SIGINT) that were blocked by blockInterruptSignals().
 */
bool SignalTk::unblockInterruptSignals()
{
	sigset_t signalMask; // mask of signals to unblock

	sigemptyset(&signalMask);

	sigaddset(&signalMask, SIGINT);
	sigaddset(&signalMask, SIGTERM);

	int sigmaskRes = pthread_sigmask(SIG_UNBLOCK, &signalMask, NULL);

	return (sigmaskRes == 0);
}

/**
 * Log backtrace of the calling thread to file SIGNALTK_BACKTRACE_PATH and to stderr.
 *
 * Note: Might return an empty string if compiled without backtrace support for musl-libc
 * compatibility.
 */
std::string SignalTk::logBacktrace()
{
#ifndef BACKTRACE_SUPPORT

    return "This executable was compiled without backtrace support.";

#else // BACKTRACE_SUPPORT

    std::cerr << "Trying to generate a backtrace..." << std::endl;

    std::ostringstream stream;

    stream << "******** BACKTRACE START ********" << std::endl;
    stream << boost::stacktrace::stacktrace();
    stream << "********* BACKTRACE END *********" << std::endl;

    std::cerr << "Backtrace generation done." << std::endl;

    // additionally log backtrace to file if possible

    std::ofstream fileStream;

    fileStream.open(SIGNALTK_BACKTRACE_PATH, std::ofstream::app);

    if(fileStream)
    {
        fileStream << stream.str();
        fileStream.close();

        // make file writable by all in case different users generate backtraces on shared system
        chmod(SIGNALTK_BACKTRACE_PATH, SIGNALTK_BACKTRACE_FILEMODE);

        std::cerr << "Saved backtrace: " << SIGNALTK_BACKTRACE_PATH << std::endl;
    }

    return stream.str();

#endif // BACKTRACE_SUPPORT
}

/**
 * Return linux-specific thread ID to show which thread encountered an error.
 *
 * This would usually just be gettid(), but gettid() is not available on all platforms, so we use
 * syscall() just like the man page suggests.
 *
 * @return linux-specific thread ID (which is not the same as POSIX thread ID)
 */
pid_t SignalTk::getThreadID()
{
#ifdef SYS_gettid
	return syscall(SYS_gettid);
#else // SYS_gettid
	return (unsigned long)pthread_self();
#endif // SYS_gettid
}
