// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef TOOLKITS_S3INTERRUPTIBLERETRYSTRATEGY_H_
#define TOOLKITS_S3INTERRUPTIBLERETRYSTRATEGY_H_

#ifdef S3_SUPPORT

#include <atomic>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/client/RetryStrategy.h>

/**
 * A decorator around any Aws::Client::RetryStrategy that checks the
 * interrupt flag before delegating ShouldRetry. All other methods are
 * forwarded unchanged. The base strategy is typically created by
 * Aws::Client::InitRetryStrategy(), which honors the AWS_RETRY_MODE and
 * AWS_MAX_ATTEMPTS environment variables.
 */
class S3InterruptibleRetryStrategy : public Aws::Client::RetryStrategy
{
    public:
        S3InterruptibleRetryStrategy(std::shared_ptr<Aws::Client::RetryStrategy> baseStrategy,
                                     std::atomic_bool *isInterruptionRequestedPtr)
            : baseStrategy(std::move(baseStrategy)), isInterruptionRequestedPtr(isInterruptionRequestedPtr)
        {
        }

        bool ShouldRetry(const Aws::Client::AWSError<Aws::Client::CoreErrors> &error,
                         long attemptedRetries) const override
        {
            if (isInterruptionRequestedPtr && isInterruptionRequestedPtr->load())
                return false;

            return baseStrategy->ShouldRetry(error, attemptedRetries);
        }

        long CalculateDelayBeforeNextRetry(const Aws::Client::AWSError<Aws::Client::CoreErrors> &error,
                                           long attemptedRetries) const override
        {
            return baseStrategy->CalculateDelayBeforeNextRetry(error, attemptedRetries);
        }

        long GetMaxAttempts() const override
        {
            return baseStrategy->GetMaxAttempts();
        }

        void GetSendToken() override
        {
            baseStrategy->GetSendToken();
        }

        bool HasSendToken() override
        {
            return baseStrategy->HasSendToken();
        }

        void RequestBookkeeping(const Aws::Client::HttpResponseOutcome &outcome) override
        {
            baseStrategy->RequestBookkeeping(outcome);
        }

        void RequestBookkeeping(const Aws::Client::HttpResponseOutcome &outcome,
                                const Aws::Client::AWSError<Aws::Client::CoreErrors> &lastError) override
        {
            baseStrategy->RequestBookkeeping(outcome, lastError);
        }

        const char *GetStrategyName() const override
        {
            return baseStrategy->GetStrategyName();
        }

    private:
        std::shared_ptr<Aws::Client::RetryStrategy> baseStrategy;
        std::atomic_bool *isInterruptionRequestedPtr; // can be NULL
};

#endif // S3_SUPPORT

#endif // TOOLKITS_S3INTERRUPTIBLERETRYSTRATEGY_H_
