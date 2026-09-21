// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef TOOLKITS_S3INTERRUPTIBLERETRYSTRATEGY_H_
#define TOOLKITS_S3INTERRUPTIBLERETRYSTRATEGY_H_

#ifdef S3_SUPPORT

#include <atomic>
#include <aws/core/client/AWSError.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/client/RetryStrategy.h>
#include <aws/core/http/HttpResponse.h>

/**
 * A decorator around any Aws::Client::RetryStrategy that checks the interrupt flag before
 * delegating ShouldRetry. It also remaps HTTP statuses and throttle exception names that the AWS
 * SDK leaves as non-retryable (notably 429 with Code "TooManyRequests") so the wrapped strategy
 * can apply its normal attempt limit, quota, and backoff. The base strategy is typically created
 * by Aws::Client::InitRetryStrategy(), which honors AWS_RETRY_MODE and AWS_MAX_ATTEMPTS.
 */
class S3InterruptibleRetryStrategy : public Aws::Client::RetryStrategy
{
    public:
        S3InterruptibleRetryStrategy(std::shared_ptr<Aws::Client::RetryStrategy> baseStrategy,
            std::atomic_bool *isInterruptionRequestedPtr)
            : baseStrategy(std::move(baseStrategy) ),
              isInterruptionRequestedPtr(isInterruptionRequestedPtr)
        {
        }

        bool ShouldRetry(const Aws::Client::AWSError<Aws::Client::CoreErrors> &error,
            long attemptedRetries) const override
        {
            if(isInterruptionRequestedPtr && isInterruptionRequestedPtr->load() )
                return false;

            return baseStrategy->ShouldRetry(asRetryableIfNeeded(error), attemptedRetries);
        }

        long CalculateDelayBeforeNextRetry(
            const Aws::Client::AWSError<Aws::Client::CoreErrors> &error,
            long attemptedRetries) const override
        {
            return baseStrategy->CalculateDelayBeforeNextRetry(
                asRetryableIfNeeded(error), attemptedRetries);
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
            if(outcome.IsSuccess() )
                baseStrategy->RequestBookkeeping(outcome);
            else
                baseStrategy->RequestBookkeeping(
                    Aws::Client::HttpResponseOutcome(asRetryableIfNeeded(outcome.GetError() ) ) );
        }

        void RequestBookkeeping(const Aws::Client::HttpResponseOutcome &outcome,
            const Aws::Client::AWSError<Aws::Client::CoreErrors> &lastError) override
        {
            if(outcome.IsSuccess() )
                baseStrategy->RequestBookkeeping(outcome, asRetryableIfNeeded(lastError) );
            else
                baseStrategy->RequestBookkeeping(
                    Aws::Client::HttpResponseOutcome(asRetryableIfNeeded(outcome.GetError() ) ),
                    asRetryableIfNeeded(lastError) );
        }

        const char *GetStrategyName() const override
        {
            return baseStrategy->GetStrategyName();
        }

    private:
        /**
         * AWS SDK and S3-compatible throttle codes. Includes names the core mapper only registers
         * with AWS_NEW_RETRIES_2026, plus "TooManyRequests".
         */
        static bool isThrottlingExceptionName(const Aws::String &exceptionName)
        {
            static const char *const names[] = {
                "BandwidthLimitExceeded",
                "EC2ThrottledException",
                "LimitExceededException",
                "PriorRequestNotComplete",
                "ProvisionedThroughputExceededException",
                "RequestLimitExceeded",
                "RequestThrottled",
                "RequestThrottledException",
                "SlowDown",
                "ThrottledException",
                "Throttling",
                "ThrottlingException",
                "TooManyRequests",
                "TooManyRequestsException",
                "TransactionInProgressException",
            };

            for(const char *name : names)
            {
                if(exceptionName == name)
                    return true;
            }

            return false;
        }

        /**
         * Classify an error the SDK marked non-retryable. Named throttle codes and HTTP 429/509
         * become RETRYABLE_THROTTLING; other retryable HTTP statuses become RETRYABLE.
         *
         * @return NOT_RETRYABLE if this decorator should not override the SDK classification.
         */
        static Aws::Client::RetryableType classifyRetryableType(
            const Aws::Client::AWSError<Aws::Client::CoreErrors> &error)
        {
            if(isThrottlingExceptionName(error.GetExceptionName() ) )
                return Aws::Client::RetryableType::RETRYABLE_THROTTLING;

            const Aws::Http::HttpResponseCode responseCode = error.GetResponseCode();
            if(responseCode == Aws::Http::HttpResponseCode::TOO_MANY_REQUESTS ||
                responseCode == Aws::Http::HttpResponseCode::BANDWIDTH_LIMIT_EXCEEDED)
                return Aws::Client::RetryableType::RETRYABLE_THROTTLING;

            if(Aws::Http::IsRetryableHttpResponseCode(responseCode) )
                return Aws::Client::RetryableType::RETRYABLE;

            return Aws::Client::RetryableType::NOT_RETRYABLE;
        }

        /**
         * Return error unchanged if the SDK already marked it retryable. Otherwise copy it and set
         * a retryable type when HTTP status or exception name says it should be retried, so the
         * wrapped strategy sees the same flags it uses for quota, adaptive limiting, and backoff.
         */
        static Aws::Client::AWSError<Aws::Client::CoreErrors> asRetryableIfNeeded(
            const Aws::Client::AWSError<Aws::Client::CoreErrors> &error)
        {
            if(error.ShouldRetry() )
                return error;

            const Aws::Client::RetryableType retryableType = classifyRetryableType(error);
            if(retryableType == Aws::Client::RetryableType::NOT_RETRYABLE)
                return error;

            Aws::Client::AWSError<Aws::Client::CoreErrors> remapped(error);
            remapped.SetRetryableType(retryableType);
            return remapped;
        }

        std::shared_ptr<Aws::Client::RetryStrategy> baseStrategy;
        std::atomic_bool *isInterruptionRequestedPtr; // can be NULL
};

#endif // S3_SUPPORT

#endif // TOOLKITS_S3INTERRUPTIBLERETRYSTRATEGY_H_
