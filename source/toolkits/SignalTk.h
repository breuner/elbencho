#ifndef TOOLKITS_SIGNALTK_H_
#define TOOLKITS_SIGNALTK_H_

#include <string>


class SignalTk
{
	public:
		static void registerFaultSignalHandlers();
		static bool blockInterruptSignals();
		static bool unblockInterruptSignals();
		static std::string logBacktrace();

	private:
		SignalTk() {}

		static void faultSignalHandler(int sig);
		static pid_t getThreadID();
};


#endif /* TOOLKITS_SIGNALTK_H_ */
