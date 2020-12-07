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


#define XFER_PREP_PROTCOLVERSION			"ProtocolVersion"
#define XFER_PREP_BENCHPATHTYPE				"BenchPathType"
#define XFER_PREP_ERRORHISTORY				"ErrorHistory"

#define XFER_STATS_BENCHID 					"BenchID"
#define XFER_STATS_BENCHPHASENAME 			"PhaseName"
#define XFER_STATS_BENCHPHASECODE			"PhaseCode"
#define XFER_STATS_NUMWORKERSDONE			"NumWorkersDone"
#define XFER_STATS_NUMWORKERSDONEWITHERR	"NumWorkersDoneWithError"
#define XFER_STATS_NUMENTRIESDONE 			"NumEntriesDone"
#define XFER_STATS_NUMBYTESDONE 			"NumBytesDone"
#define XFER_STATS_NUMIOPSDONE 				"NumIOPSDone"
#define XFER_STATS_ELAPSEDUSECLIST			"ElapsedUSecList"
#define XFER_STATS_ELAPSEDUSECLIST_ITEM		"ElapsedUSecList.item"
#define XFER_STATS_ELAPSEDSECS 				"ElapsedSecs"
#define XFER_STATS_ERRORHISTORY				XFER_PREP_ERRORHISTORY
#define XFER_STATS_LAT_PREFIX_IOPS			"IOPS_"
#define XFER_STATS_LAT_PREFIX_ENTRIES		"Entries_"
#define XFER_STATS_LATMICROSECTOTAL			"LatMicroSecTotal"
#define XFER_STATS_LATNUMVALUES				"LatNumValues"
#define XFER_STATS_LATMINMICROSEC			"LatMinMicroSec"
#define XFER_STATS_LATMAXMICROSEC			"LatMaxMicroSec"
#define XFER_STATS_LATHISTOLIST				"LatHistoList"
#define XFER_STATS_LATHISTOLIST_ITEM		"LatHistoList.item"
#define XFER_STATS_CPUUTIL_STONEWALL		"CPUUtilStoneWall"
#define XFER_STATS_CPUUTIL					"CPUUtil"

#define XFER_START_BENCHID					XFER_STATS_BENCHID
#define XFER_START_BENCHPHASECODE			XFER_STATS_BENCHPHASECODE

#define XFER_INTERRUPT_QUIT					"quit"

#define HTTPCLIENTPATH_INFO					"/info"
#define HTTPCLIENTPATH_PROTOCOLVERSION		"/protocolversion"
#define HTTPCLIENTPATH_STATUS				"/status"
#define HTTPCLIENTPATH_BENCHRESULT			"/benchresult"
#define HTTPCLIENTPATH_PREPAREPHASE			"/preparephase"
#define HTTPCLIENTPATH_STARTPHASE			"/startphase"
#define HTTPCLIENTPATH_INTERRUPTPHASE		"/interruptphase"

#define MAKE_SERVER_PATH(path)				"^" path "$"
#define HTTPSERVERPATH_INFO					MAKE_SERVER_PATH(HTTPCLIENTPATH_INFO)
#define HTTPSERVERPATH_PROTOCOLVERSION		MAKE_SERVER_PATH(HTTPCLIENTPATH_PROTOCOLVERSION)
#define HTTPSERVERPATH_STATUS				MAKE_SERVER_PATH(HTTPCLIENTPATH_STATUS)
#define HTTPSERVERPATH_BENCHRESULT			MAKE_SERVER_PATH(HTTPCLIENTPATH_BENCHRESULT)
#define HTTPSERVERPATH_PREPAREPHASE			MAKE_SERVER_PATH(HTTPCLIENTPATH_PREPAREPHASE)
#define HTTPSERVERPATH_STARTPHASE			MAKE_SERVER_PATH(HTTPCLIENTPATH_STARTPHASE)
#define HTTPSERVERPATH_INTERRUPTPHASE		MAKE_SERVER_PATH(HTTPCLIENTPATH_INTERRUPTPHASE)


using HttpClient = SimpleWeb::Client<SimpleWeb::HTTP>;

namespace Web = SimpleWeb;


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

		BenchPathType benchPathType; // set as result of preparation phase

		struct CPUUtil
		{
			unsigned live = 0;
			unsigned stoneWall = 0;
			unsigned lastDone = 0;
		} cpuUtil; // all values are percent

		virtual void run() override;

		void finishPhase(bool allowExceptionThrow);
		void preparePhase();
		void startBenchPhase();
		void waitForBenchPhaseCompletion(bool checkInterruption);
		void interruptBenchPhase(bool allowExceptionThrow);
		std::string frameHostErrorMsg(std::string string);


	// inliners
	public:
		size_t getNumWorkersDone() const { return numWorkersDone; }
		size_t getNumWorkersDoneWithError() const { return numWorkersDoneWithError; }
		BenchPathType getBenchPathType() const { return benchPathType; }
		std::string getHost() const { return host; }
		unsigned getCPUUtilStoneWall() const { return cpuUtil.stoneWall; }
		unsigned getCPUUtilLastDone() const { return cpuUtil.lastDone; }
		unsigned getCPUUtilLive() const { return cpuUtil.live; }

		virtual void resetStats() override
		{
			Worker::resetStats();

			numWorkersDone = 0;
			numWorkersDoneWithError = 0;
		}

};




#endif /* WORKERS_REMOTEWORKER_H_ */
