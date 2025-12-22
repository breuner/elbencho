// SPDX-FileCopyrightText: 2020-2025 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef TOOLKITS_RATELIMITERRWMIXTHREADS_H_
#define TOOLKITS_RATELIMITERRWMIXTHREADS_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>

#include "Common.h"
#include "workers/WorkerException.h"

/**
 * In contrast to the general class RateLimiter where each thread is only staying within its own
 * limit (and thus can just sleep for the remainder of a second if the limit is exceeded)
 * this class balances a number of reader threads versus a number of writer threads. Thus, we need
 * allow some headroom for all threads to be active in parallel and to quickly wake up the other
 * thread group to not stall the global progress of all threads.
 */
class RateLimiterRWMixThreads
{
    private:
        unsigned short readRatioPercent; // 0..100: % of read out of the combined read+write bytes
        unsigned numReaderThreads; // number of readers out of all threads
        unsigned numWriterThreads; // number of writers out of all threads
        size_t maxBlockSize; // max block size to calculate head room in bytes

        static std::atomic_uint_fast64_t numBytesRead; // atomic singleton for all threads
        static std::condition_variable readWaitCondition; // singleton condition for all readers
        static std::mutex readWaitConditionMutex; // singleton condition for all writers

        static std::atomic_uint_fast64_t numBytesWrite; // atomic singleton for all threads
        static std::condition_variable writeWaitCondition; // singleton condition for all writers
        static std::mutex writeWaitConditionMutex; // singleton condition for all readers

    // inliners
    public:

    /**
     * Delayed init of this class because it might typically cannot be initialized when it's
     * instantiated.
     *
     * @param readRatioPercent 0..100: % of read out of the combined read+write bytes
     */
    void initStart(unsigned short readRatioPercent, unsigned numReaderThreads,
        unsigned numWriterThreads, size_t maxBlockSize)
    {
        this->readRatioPercent = readRatioPercent;
        this->numReaderThreads = numReaderThreads;
        this->numWriterThreads = numWriterThreads;
        this->maxBlockSize = maxBlockSize;

        this->numBytesRead = 0;
        this->numBytesWrite = 0;
    }

    /**
     * For reader threads. Wait (sleep) if rate limit exceeded, otherwise return immediately.
     *
     * @nextBlockSize size of next read block.
     */
    void waitRead(size_t nextBlockSize, std::atomic_bool& isInterruptionRequested)
    {
        const unsigned maxWaitTimeoutSecs = 600; // max time to wait; exceeding this means error
        const unsigned sleepMS = 20; // short sleep time to check for interrupt

        std::chrono::steady_clock::time_point waitStartT = std::chrono::steady_clock::now();

        // loop until we're allowed to write again
        for( ; ; )
        {
            // assume the writers are one blockSize ahead to have some headroom
            uint64_t headRoomBytes = maxBlockSize * numWriterThreads;
            uint64_t writesWithHeadroom = numBytesWrite + headRoomBytes;
            uint64_t numBytesAllowedRead =
                ( (writesWithHeadroom + numBytesRead) * readRatioPercent) / 100;

            // check if we can continue the read without waiting for the writers
            if(numBytesRead <= numBytesAllowedRead)
            {
                // wake up any sleeping readers
                readWaitCondition.notify_all();

                numBytesRead += nextBlockSize;
                return;
            }

            // we can't read right now => check timeout and then sleep until someone wakes us up

            IF_UNLIKELY(isInterruptionRequested)
                throw WorkerInterruptedException(
                    "Received friendly request to interrupt execution.");

            std::chrono::steady_clock::time_point nowT = std::chrono::steady_clock::now();

            std::chrono::seconds elapsedDurationSec =
                std::chrono::duration_cast<std::chrono::seconds>(nowT - waitStartT);

            IF_UNLIKELY(elapsedDurationSec.count() >= maxWaitTimeoutSecs)
                throw WorkerException("Max wait time exceeded for rate balanced reader. "
                    "Your read ratio might be too high so that the readers starve over a long "
                    "time or you might have forgotten to add --infloop. "
                    "Max wait time in secs: " + std::to_string(maxWaitTimeoutSecs) );

            // let our writer buddies know that we're waiting for them
            writeWaitCondition.notify_all();

            /* note: readWaitConditionMutex does not protect anything. it only exists because it's
                a requirement for readWaitCondition. */
            std::unique_lock<std::mutex> lock(readWaitConditionMutex); // L O C K (scoped)

            /* note: there is a chance that the writers all exceed their rate limit in the
                brief moment between our check and our going to sleep. it's unlikely, but
                it's the reason why we have to keep the sleepMS time very short. */
            readWaitCondition.wait_for(lock, std::chrono::milliseconds(sleepMS) );

        } // end of infinite for-loop

    }

    /**
     * For writer threads. Wait (sleep) if rate limit exceeded, otherwise return immediately.
     *
     * @nextBlockSize size of next write block.
     */
    void waitWrite(size_t nextBlockSize, std::atomic_bool& isInterruptionRequested)
    {
        const unsigned maxWaitTimeoutSecs = 600; // max time to wait; exceeding this means error
        const unsigned sleepMS = 20; // short sleep time to check for interrupt

        std::chrono::steady_clock::time_point waitStartT = std::chrono::steady_clock::now();

        // loop until we're allowed to write again
        for( ; ; )
        {
            // assume the readers are one blockSize ahead to have some headroom
            uint64_t headRoomBytes = maxBlockSize * numReaderThreads;
            uint64_t readsWithHeadroom = numBytesRead + headRoomBytes;
            uint64_t numBytesAllowedWrite =
                ( (readsWithHeadroom + numBytesWrite) * (100 - readRatioPercent) ) / 100;

            // check if we can continue the write without waiting for the readers
            if(numBytesWrite <= numBytesAllowedWrite)
            {
                // wake up any sleeping writers
                writeWaitCondition.notify_all();

                numBytesWrite += nextBlockSize;
                return;
            }

            // we can't write right now => check timeout and then sleep until someone wakes us up

            IF_UNLIKELY(isInterruptionRequested)
                throw WorkerInterruptedException(
                    "Received friendly request to interrupt execution.");

            std::chrono::steady_clock::time_point nowT = std::chrono::steady_clock::now();

            std::chrono::seconds elapsedDurationSec =
                std::chrono::duration_cast<std::chrono::seconds>(nowT - waitStartT);

            IF_UNLIKELY(elapsedDurationSec.count() >= maxWaitTimeoutSecs)
                throw WorkerException("Max wait time exceeded for rate balanced writer. "
                    "Your read ratio might be too low so that the writers starve over a long "
                    "time or you might have forgotten to add --infloop. "
                    "Max wait time in secs: " + std::to_string(maxWaitTimeoutSecs) );

            // let our reader buddies know that we're waiting for them
            readWaitCondition.notify_all();

            /* note: writeWaitConditionMutex does not protect anything. it only exists because it's
                a requirement for writeWaitCondition. */
            std::unique_lock<std::mutex> lock(writeWaitConditionMutex); // L O C K (scoped)

            /* note: there is a chance that the readers all exceed their rate limit in the
                brief moment between our check and our going to sleep. it's unlikely, but
                it's the reason why we have to keep the sleepMS time very short. */
            writeWaitCondition.wait_for(lock, std::chrono::milliseconds(sleepMS) );

        } // end of infinite for-loop

    }

};


#endif /* TOOLKITS_RATELIMITERRWMIXTHREADS_H_ */
