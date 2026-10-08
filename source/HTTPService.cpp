// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <libgen.h>
#include <netinet/in.h>
#include <pwd.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "HTTPService.h"
#include "toolkits/SystemTk.h"

#define SERVICE_LOG_DIR			"/tmp"
#define SERVICE_LOG_FILEPREFIX	EXE_NAME
#define SERVICE_LOG_FILEMODE	(S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)


/**
 * Daemonize this process into background and switch stdout/stderr to logfile.
 *
 * Will exit(1) the process on error.
 *
 * Note: No other threads may be running when daemon() is called, otherwise things will get into
 * undefined state.
 */
void HTTPService::daemonize()
{
	std::string logfile;

	if(getenv("TMP") != NULL)
		logfile = getenv("TMP"); // default for linux
	else
	if(getenv("TEMP") != NULL)
		logfile = getenv("TEMP"); // default for windows
	else
		logfile = SERVICE_LOG_DIR;

	logfile += std::string("/") + SERVICE_LOG_FILEPREFIX + "_" +
		SystemTk::getUsername() + "_" +
		"p" + std::to_string(progArgs.getServicePort() ) + "." // port
		"log";

    // keep old log file (if any)
    std::string oldLogFile = logfile + ".old";
    rename(logfile.c_str(), oldLogFile.c_str() );

	int logFileFD = open(logfile.c_str(), O_CREAT | O_WRONLY | O_APPEND, SERVICE_LOG_FILEMODE);
	if(logFileFD == -1)
	{
		std::cerr << "ERROR: Failed to open logfile. Path: " << logfile << "; "
			"SysErr: " << strerror(errno) << std::endl;
		exit(1);
	}

	// try to get exclusive lock on logfile to make sure no other instance is using it
	int lockRes = flock(logFileFD, LOCK_EX | LOCK_NB);
	if(lockRes == -1)
	{
		if(errno == EWOULDBLOCK)
			std::cerr << "ERROR: Unable to get exclusive lock on logfile. Probably another "
				"instance is running and using the same service port. "
				"Path: " << logfile << "; "
				"Port: " << progArgs.getServicePort() << std::endl;
		else
			std::cerr << "ERROR: Failed to get exclusive lock on logfile. "
				"Path: " << logfile << "; "
				"Port: " << progArgs.getServicePort() << "; "
				"SysErr: " << strerror(errno) << std::endl;

		exit(1);
	}

	std::cout << "Daemonizing into background... Logfile: " << logfile << std::endl;

	// file locked, so trunc to 0 to delete messages from old instances
	int truncRes = ftruncate(logFileFD, 0);
	if(truncRes == -1)
	{
		std::cerr << "ERROR: Failed to truncate logfile. Path: " << logfile << "; "
			"SysErr: " << strerror(errno) << std::endl;
		exit(1);
	}

	int devNullFD = open("/dev/null", O_RDONLY);
	if(devNullFD == -1)
	{
		std::cerr << "ERROR: Failed to open /dev/null to daemonize. "
			"SysErr: " << strerror(errno) << std::endl;
		exit(1);
	}

	// close stdin and replace by /dev/null
	int stdinDup = dup2(devNullFD, STDIN_FILENO);
	if(stdinDup == -1)
	{
		std::cerr << "ERROR: Failed to replace stdin to daemonize. "
			"SysErr: " << strerror(errno) << std::endl;
		exit(1);
	}

	// close stdout and replace by logfile
	int stdoutDup = dup2(logFileFD, STDOUT_FILENO);
	if(stdoutDup == -1)
	{
		std::cerr << "ERROR: Failed to replace stdout to daemonize. "
			"SysErr: " << strerror(errno) << std::endl;
		exit(1);
	}

	// close stderr and replace by logfile
	int stderrDup = dup2(logFileFD, STDERR_FILENO);
	if(stderrDup == -1)
	{
		std::cerr << "ERROR: Failed to replace stderr to daemonize. "
			"SysErr: " << strerror(errno) << std::endl;
		exit(1);
	}

	// close temporary FDs
	close(devNullFD);
	close(logFileFD);

	int daemonRes = daemon(0 /* nochdir */, 1 /* noclose */);
	if(daemonRes == -1)
	{
		std::cerr << "ERROR: Failed to daemonize into background. "
			"SysErr: " << strerror(errno) << std::endl;
		exit(1);
	}

	// if we got here, we successfully daemonized into background

	LOGGER(Log_NORMAL, "Running in background. PID: " << getpid() << std::endl);

	const StringVec& benchPathsServiceOverrideVec = progArgs.getBenchPathsServiceOverride();
	if(!benchPathsServiceOverrideVec.empty() )
	{
		std::string logMsg = "NOTE: Benchmark paths given. These paths will be used instead of any "
			"path list provided by master. Paths: ";

		for(std::string path : benchPathsServiceOverrideVec)
			logMsg += "\"" + path + "\" ";

		LOGGER(Log_NORMAL, logMsg << std::endl);
	}

	std::string gpuIDsServiceOverride = progArgs.getGPUIDsServiceOverride();
	if(!gpuIDsServiceOverride.empty() )
		LOGGER(Log_NORMAL, "NOTE: GPU IDs given. These GPU IDs will be used instead of any "
			"GPU ID list provided by master. GPU IDs: " << gpuIDsServiceOverride << std::endl);

	std::string s3EndpointsServiceOverride = progArgs.getS3Args().getS3EndpointsServiceOverride();
	if(!s3EndpointsServiceOverride.empty() )
		LOGGER(Log_NORMAL, "NOTE: S3 endpoints given. These will be used instead of any S3 "
			"endpoints provided by master: " << s3EndpointsServiceOverride << std::endl);
}

/**
 * Bind and listen on the wildcard address of the given family, then close the socket.
 * This is a preflight so a port conflict can be reported before daemonizing.
 *
 * @return false if this address family is not supported on this host.
 * @throw ProgException if the port is not available or another socket error occurred.
 */
static bool checkWildcardPortAvailable(int addressFamily, unsigned short port)
{
    const int listenBacklogSize = 1;

    int sockFD = socket(addressFamily, SOCK_STREAM, 0);
    if(sockFD == -1)
    {
        if(errno == EAFNOSUPPORT)
            return false;

        throw ProgException(std::string("Unable to create socket to check port availability. ") +
            "SysErr: " + strerror(errno) );
    }

    /* note: reuse option is important because http server sock will be in TIME_WAIT state for
        quite a while after stopping the service via --quit */

    int enableAddrReuse = 1;

    int setOptRes = setsockopt(
        sockFD, SOL_SOCKET, SO_REUSEADDR, &enableAddrReuse, sizeof(enableAddrReuse) );
    if(setOptRes == -1)
    {
        int errnoCopy = errno; // close() below could change errno

        close(sockFD);

        throw ProgException(std::string("Unable to enable socket address reuse. ") +
            "SysErr: " + strerror(errnoCopy) );
    }

#ifdef IPV6_V6ONLY

    // this platform provides the IPV6_V6ONLY flag, so we can turn it off to listen for IPv4 & IPv6

    if(addressFamily == AF_INET6)
    {
        // Match the HTTP servers, which accept IPv4 connections on the IPv6 wildcard socket.
        int v6Only = 0;

        setOptRes = setsockopt(sockFD, IPPROTO_IPV6, IPV6_V6ONLY, &v6Only, sizeof(v6Only) );
        if(setOptRes == -1)
        {
            int errnoCopy = errno; // close() below could change errno

            close(sockFD);

            throw ProgException(std::string("Unable to disable IPV6_V6ONLY for preflight check. ") +
                "SysErr: " + strerror(errnoCopy) );
        }
    }
#endif // IPV6_V6ONLY

    int bindRes;

    if(addressFamily == AF_INET6)
    {
        struct sockaddr_in6 sockAddr = {};

        sockAddr.sin6_family = AF_INET6;
        sockAddr.sin6_addr = in6addr_any;
        sockAddr.sin6_port = htons(port);

        bindRes = bind(sockFD, (struct sockaddr*) &sockAddr, sizeof(sockAddr) );
    }
    else
    {
        struct sockaddr_in sockAddr = {};

        sockAddr.sin_family = AF_INET;
        sockAddr.sin_addr.s_addr = INADDR_ANY;
        sockAddr.sin_port = htons(port);

        bindRes = bind(sockFD, (struct sockaddr*) &sockAddr, sizeof(sockAddr) );
    }

    if(bindRes == -1)
    {
        int errnoCopy = errno; // close() below could change errno

        close(sockFD);

        throw ProgException(std::string("Unable to bind to desired port. "
            "Service already running? ") +
            "Port: " + std::to_string(port) + "; "
            "SysErr: " + strerror(errnoCopy) );
    }

    int listenRes = listen(sockFD, listenBacklogSize);
    if(listenRes == -1)
    {
        int errnoCopy = errno; // close() below could change errno

        close(sockFD);

        throw ProgException(std::string("Unable to listen on desired port. ") +
            "Port: " + std::to_string(port) + "; "
            "SysErr: " + strerror(errnoCopy) );
    }

    close(sockFD);

    return true;
}

/**
 * Check if desired TCP port is available, so that an error message can be printed before
 * daemonizing.
 *
 * Prefers the same dual-stack IPv6 wildcard the HTTP servers bind. Falls back to IPv4 only when
 * this host has no IPv6. A busy port is reported immediately and is not retried on the other
 * family, because the real listen socket would fail the same way.
 *
 * @throw ProgException if port not available or other error occured.
 */
void HTTPService::checkPortAvailable()
{
    unsigned short port = progArgs.getServicePort();

#ifdef CYGWIN_SUPPORT
    // we force HTTPServiceSWS to IPv4-only on cygwin, so the preflight has to match that.
    if(!checkWildcardPortAvailable(AF_INET, port) )
        throw ProgException(std::string("Unable to create socket to check port availability. ") +
            "SysErr: " + strerror(EAFNOSUPPORT) );
#else
    if(checkWildcardPortAvailable(AF_INET6, port) )
        return;

    if(!checkWildcardPortAvailable(AF_INET, port) )
        throw ProgException(std::string("Unable to create socket to check port availability. ") +
            "SysErr: " + strerror(EAFNOSUPPORT) );
#endif // CYGWIN_SUPPORT
}

