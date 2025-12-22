// SPDX-FileCopyrightText: 2020-2025 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef WORKERS_REMOTEWORKER_H_
#define WORKERS_REMOTEWORKER_H_

#include <client_http.hpp>
#include <ctype.h>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "Worker.h"


using HttpClient = SimpleWeb::Client<SimpleWeb::HTTP>;


/**
 * Each worker represents a single HTTP client thread coordinating a remote service of possibly
 * multiple threads.
 */
class RemoteWorker : public Worker
{
	public:
		/**
		 * @host hostname:port to connect to.
		 */
		explicit RemoteWorker(WorkersSharedData* workersSharedData, size_t workerRank,
			std::string host) :
			Worker(workersSharedData, workerRank), host(host), httpClient(host) {}

		~RemoteWorker() {}


	private:
		std::string host; // hostname:port to connect to
		HttpClient httpClient;

		size_t numWorkersDone{0};// number of threads that are through with current phase
		size_t numWorkersDoneWithError{0}; // number of threads that failed the current phase

		BenchPathInfo benchPathInfo; // set as result of preparation phase

		struct CPUUtil
		{
			unsigned live = 0;
			unsigned stoneWall = 0;
			unsigned lastDone = 0;
		} cpuUtil; // all values are percent

		LiveLatency liveLatency = {};

		virtual void run() override;

		void finishPhase(bool allowExceptionThrow);
		void prepareRemoteFile();
		void preparePhase();
		void startBenchPhase();
		void waitForBenchPhaseCompletion(bool checkInterruption);
		void interruptBenchPhase(bool allowExceptionThrow, bool logSuccessMsg=false);
		std::string frameHostErrorMsg(std::string string);


	// inliners
	public:
		size_t getNumWorkersDone() const { return numWorkersDone; }
		size_t getNumWorkersDoneWithError() const { return numWorkersDoneWithError; }
		const BenchPathInfo& getBenchPathInfo() const { return benchPathInfo; }
		std::string getHost() const { return host; }
		unsigned getCPUUtilStoneWall() const { return cpuUtil.stoneWall; }
		unsigned getCPUUtilLastDone() const { return cpuUtil.lastDone; }
		unsigned getCPUUtilLive() const { return cpuUtil.live; }

		/**
		 * Add current live latency values of this worker to given outSumLiveOps and reset them.
		 */
		virtual void getAndAddLiveLatency(LiveLatency& outLiveLatency) override
		{
			liveLatency.getAndAddOps(outLiveLatency);
		}

		virtual void resetStats() override
		{
			Worker::resetStats();

			numWorkersDone = 0;
			numWorkersDoneWithError = 0;

			liveLatency = {};
		}

};




#endif /* WORKERS_REMOTEWORKER_H_ */
