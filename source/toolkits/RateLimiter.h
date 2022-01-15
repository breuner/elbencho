#ifndef TOOLKITS_RATELIMITER_H_
#define TOOLKITS_RATELIMITER_H_

#include <chrono>
#include <thread>
#include "Common.h"

/**
 * Provides a way to let a thread sleep for the remainder of a second if a given limit is exceeded.
 */
class RateLimiter
{
	private:
		uint64_t limitPerSec; // limit in whatever unit should be rate limited (e.g. bytes)
		uint64_t numDoneThisSec; // how much we used up of our rate limit this sec (e.g. bytes)
		std::chrono::steady_clock::time_point startT; // when the current second started

	// inliners
	public:

	void initStart(uint64_t limitPerSec)
	{
		this->limitPerSec = limitPerSec;
		this->numDoneThisSec = 0;
		this->startT = std::chrono::steady_clock::now();
	}

	/**
	 * Wait (sleep) if rate limit exceeded, otherwise return immediately.
	 *
	 * @nextSize whatever the rate limited unit is (e.g. bytes)
	 */
	void wait(size_t nextSize)
	{
		std::chrono::steady_clock::time_point nowT = std::chrono::steady_clock::now();
		std::chrono::microseconds elapsedMicroSec =
			std::chrono::duration_cast<std::chrono::microseconds>
			(nowT - startT);

		IF_UNLIKELY(elapsedMicroSec.count() >= 1000000)
		{ // 1s elapsed without exceeding the rate limit => reset for next second
			numDoneThisSec = nextSize;
			startT = std::chrono::steady_clock::now();
			return;
		}
		else
		IF_UNLIKELY( (numDoneThisSec + nextSize) > limitPerSec)
		{ // next r/w op would exceed rate limit => wait until end of second
			std::this_thread::sleep_until(startT + std::chrono::microseconds(1000000) );

			numDoneThisSec = nextSize;
			startT = std::chrono::steady_clock::now();
			return;
		}
		else
		{ // rate limit not exceeded yet => proceed
			numDoneThisSec += nextSize;
		}

	}

};



#endif /* TOOLKITS_RATELIMITER_H_ */
