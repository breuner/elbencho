#ifndef SIGNALTK_H_
#define SIGNALTK_H_


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
};


#endif /* SIGNALTK_H_ */
