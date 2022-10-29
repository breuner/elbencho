#include <cstring>
#include <errno.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "ProgException.h"
#include "Terminal.h"

/**
 * Check if stdout is a tty. Intended to find out if live stats can be enabled.
 *
 * @return true if stdout is a tty (and this is likely to understand special codes to erase line).
 */
bool Terminal::isStdoutTTY()
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
int Terminal::getTerminalLineLength(int defaultLen)
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
bool Terminal::disableConsoleBuffering()
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
bool Terminal::resetConsoleBuffering()
{
	if(!isStdoutTTY() )
		return true; // stdout might be a file

	int setBufRes = setvbuf(stdout, NULL, _IOLBF, 0);

	return setBufRes ? false : true;
}
