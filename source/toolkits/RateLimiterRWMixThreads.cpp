#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>

#include "Common.h"
#include "toolkits/RateLimiterRWMixThreads.h"
#include "workers/WorkerException.h"


std::atomic_uint_fast64_t RateLimiterRWMixThreads::numBytesRead; // atomic singleton for all threads
std::condition_variable RateLimiterRWMixThreads::readWaitCondition; // singleton cond for readers
std::mutex RateLimiterRWMixThreads::readWaitConditionMutex; // singleton condition for all writers

std::atomic_uint_fast64_t RateLimiterRWMixThreads::numBytesWrite; // atomic singleton for all thr
std::condition_variable RateLimiterRWMixThreads::writeWaitCondition; // singleton cond for writers
std::mutex RateLimiterRWMixThreads::writeWaitConditionMutex; // singleton condition for all readers
