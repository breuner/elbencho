// SPDX-FileCopyrightText: 2020-2025 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef TERMINAL_H_
#define TERMINAL_H_

#define CONTROLCHARS_CLEARLINE_AND_CARRIAGERETURN   "\x1B[2K\r" /* "\x1B[2K" is the VT100 code to
                                        clear the line. "\r" moves cursor to beginning of line. */

/**
 * Toolkit for working with terminal/console/TTY.
 */
class TerminalTk
{
	public:
		static bool isStdoutTTY();
		static int getTerminalLineLength(int defaultLen=0);
		static bool disableConsoleBuffering();
		static bool resetConsoleBuffering();
		static bool rewriteConsoleLine(std::string lineStr);
		static bool clearConsoleLine();

	private:
		TerminalTk() {}
};



#endif /* TERMINAL_H_ */
