#include <cstring>
#include <errno.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "ProgException.h"
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
