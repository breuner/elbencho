// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <cstring>
#include <fstream>
#include <iostream>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "toolkits/TerminalTk.h"


/**
 * Check if stdout is a tty. Intended to find out if live stats can be enabled.
 *
 * @return true if stdout is a tty (and this is likely to understand special codes to erase line).
 */
bool TerminalTk::isStdoutTTY()
{
	if(isatty(fileno(stdout) ) == 1)
		return true;

	return false;
}

/**
 * Get length (columns) of terminal.
 *
 * @defaultLen The value to be returned if terminal line length cannot be retried.
 * @return If terminal line length cannot be retrieved (e.g. because output is not a terminal) then
 * 		defaultLen will be returned.
 */
int TerminalTk::getTerminalLineLength(int defaultLen)
{
	struct winsize consoleSize;

	int ioctlRes = ioctl(STDOUT_FILENO, TIOCGWINSZ, &consoleSize);

	if(ioctlRes == -1)
		return defaultLen;

	return consoleSize.ws_col;
}

/**
 * Disable stdout line buffering. Intended for single line live stats and live countdown.
 *
 * @statusVariable optional value to set to true if buffering has been disabled.
 * @return true on success (if console is not a tty, then this is also counted as success).
 */
bool TerminalTk::disableConsoleBuffering()
{
	if(!isStdoutTTY() )
		return true; // stdout might be a file

	int setBufRes = setvbuf(stdout, NULL, _IONBF, 0);

	return setBufRes ? false : true;
}

/**
 * Intended to re-enable (line) buffering after it was temporarily disabled for single line live
 * stats.
 *
 * @return true on success (if console is not a tty, then this is also counted as success).
 */
bool TerminalTk::resetConsoleBuffering()
{
	if(!isStdoutTTY() )
		return true; // stdout might be a file

	int setBufRes = setvbuf(stdout, NULL, _IOLBF, 0);

	return setBufRes ? false : true;
}

/**
 * Erase the contents of the current console line and write the new given line. This is useful for
 * single-line console live stats. The given line must not end with a newline and will get trimmed
 * to the console width.
 *
 * This is a no-op if stdout is not a TTY.
 *
 * Console buffering needs to be disabled for this to work.
 *
 * @return always true, just so that this can be used as "liveStatsDisabled || rewriteLine()"
 */
bool TerminalTk::rewriteConsoleLine(std::string lineStr)
{
    if(!isStdoutTTY() )
        return true; // stdout might be a file

    // check console size to handle console resize at runtime

    int terminalLineLen = getTerminalLineLength();
    if(!terminalLineLen)
        return true; // don't cancel program (by throwing) just because we can't show live stats

    // note: "-2" for "^C" printed when user presses ctrl+c
    unsigned usableLineLen = terminalLineLen - 2;

    if(lineStr.length() > usableLineLen)
        lineStr.resize(usableLineLen);


    std::cout << CONTROLCHARS_CLEARLINE_AND_CARRIAGERETURN << lineStr;

    return true;
}

/**
 * Erase the current line and move cursor back to beginning of erased line.
 *
 * @return see rewriteConsoleLine().
 */
bool TerminalTk::clearConsoleLine()
{
    return rewriteConsoleLine(std::string() );
}

/**
 * Check if we are currently in a GNU screen session (based on "STY" env var) without "altscreen"
 * feature enabled, which means e.g. switching to fullscreen live stats and back will not work
 * cleanly.
 *
 * There is no direct check for the altscreen feature available and it's off by default, so best we
 * can do is check the user's personal screen config file and the global screen config file.
 *
 * @return true if we are in a gnu screen session without altscreen support.
 */
bool TerminalTk::isScreenSessionWithoutAltscreen()
{
    // GNU screen sets the STY env var, so we can use that to detect screen
    const char* sty = std::getenv("STY");
    if(!sty)
        return false;

    /* helper lambda to scan a screenrc file for the altscreen setting; returns true if
        "altscreen on" found in given file, false otherwise */
    auto check_config_file = [](const std::string& path)
    {
        std::ifstream file(path);
        if(!file.is_open() )
            return false;

        std::string line;
        while (std::getline(file, line))
        {
            // trim leading whitespace
            line.erase(0, line.find_first_not_of(" \t") );

            // check if line starts with "altscreen on"
            if (line.rfind("altscreen on", 0) == 0)
                return true;
        }

        return false;
    };

    // check the user's local config first
    const char* home = std::getenv("HOME");
    if(home && check_config_file(std::string(home) + "/.screenrc") )
        return false; // altscreen is on in personal config

    // check the global config as a fallback
    if (check_config_file("/etc/screenrc"))
        return false; // altscreen is on in global config

    // neither in global conf nor in personal conf => altscreen is off by default
    return true;
}
