#ifndef TERMINAL_H_
#define TERMINAL_H_

/**
 * Toolkit for working with terminal/console/TTY.
 */
class Terminal
{
	public:
		static bool isStdoutTTY();
		static int getTerminalLineLength(int defaultLen=0);
		static bool disableConsoleBuffering();
		static bool resetConsoleBuffering();

	private:
		Terminal() {}
};



#endif /* TERMINAL_H_ */
