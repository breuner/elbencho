// SPDX-FileCopyrightText: 2020-2025 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef TOOLKITS_SIGNALTK_H_
#define TOOLKITS_SIGNALTK_H_

#include <string>
#include <unistd.h>

class ProgArgs; // forward declaration

class SignalTk
{
	public:
		static void registerFaultSignalHandlers(const ProgArgs& progArgs);
		static bool blockInterruptSignals();
		static bool unblockInterruptSignals();
		static std::string logBacktrace();

	private:
		SignalTk() {}

		static void faultSignalHandler(int sig);
		static pid_t getThreadID();
};


#endif /* TOOLKITS_SIGNALTK_H_ */
