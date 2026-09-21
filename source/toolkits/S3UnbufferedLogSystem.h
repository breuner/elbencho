// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef TOOLKITS_S3UNBUFFEREDLOGSYSTEM_H_
#define TOOLKITS_S3UNBUFFEREDLOGSYSTEM_H_

#ifdef S3_SUPPORT

#include <mutex>
#include <string>

#include <aws/core/utils/logging/FormattedLogSystem.h>

/**
 * AWS SDK logger that write()s each formatted line immediately (O_APPEND). There is no
 * application-level write buffer; durability is left to the kernel page cache. The filename is
 * prefix plus UTC "YYYY-MM-DD-HH.log", matching DefaultLogSystem, and the fd is reopened when the
 * hour changes so long-lived service processes keep rolling.
 *
 * Note: This exists because the DefaultLogSystem used application-level buffering of multiple log
 *     lines, so especially in service mode there were log lines missing at the end of a benchmark
 *     phase when the service instance kept running.
 */
class S3UnbufferedLogSystem : public Aws::Utils::Logging::FormattedLogSystem
{
    public:
        S3UnbufferedLogSystem(Aws::Utils::Logging::LogLevel logLevel,
            const std::string& filenamePrefix);
        ~S3UnbufferedLogSystem() override;

        S3UnbufferedLogSystem(const S3UnbufferedLogSystem&) = delete;
        S3UnbufferedLogSystem& operator=(const S3UnbufferedLogSystem&) = delete;

        void Flush() override {}

    protected:
        void ProcessFormattedStatement(Aws::String&& statement) override;

    private:
        void closeLogFile();
        bool openLogFileUnlocked();

        std::mutex mutex;
        std::string filenamePrefix;
        std::string currentPath;
        int fd{-1};
};

#endif // S3_SUPPORT

#endif /* TOOLKITS_S3UNBUFFEREDLOGSYSTEM_H_ */
