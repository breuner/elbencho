// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef LOG_H_
#define LOG_H_

#include <iostream>
#include <mutex>
#include <string>

/**
 * Minimal thread-safe logging: info and debug messages go to stdout, errors to stderr.
 */
class Log
{
    public:
        static inline bool verbose = false; // enables debug()

        static void info(const std::string& msg) { print(std::cout, msg); }
        static void error(const std::string& msg) { print(std::cerr, "ERROR: " + msg); }

        static void debug(const std::string& msg)
        {
            if(verbose)
                print(std::cout, msg);
        }

    private:
        Log() {}

        static void print(std::ostream& out, const std::string& msg)
        {
            static std::mutex mutex;
            std::lock_guard<std::mutex> lock(mutex);

            out << msg << std::endl;
        }
};

#endif // LOG_H_
