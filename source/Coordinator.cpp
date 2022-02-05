#include <csignal>
#include <iostream>
#include "Coordinator.h"
#include "HTTPService.h"
#include "ProgArgs.h"
#include "ProgException.h"
#include "toolkits/S3Tk.h"
#include "toolkits/SignalTk.h"
#include "workers/WorkerException.h"


/**
 * The entry point to start coordinating benchmarks.
 *
 * @return value is suitable to use as application main() return value, so 0 on success and non-zero
 * 		otherwise.
 */
int Coordinator::main()
{
	int retVal = EXIT_SUCCESS;

	try
	{
		SignalTk::registerFaultSignalHandlers();

		// HTTP service mode
		// note: no other threads may be running when HTTPService.startServer() daemonizes.
		if(progArgs.getRunAsService() )
		{
			LoggerBase::enableErrHistory();

			HTTPService(progArgs, workerManager, statistics).startServer();

			Logger() << "HTTP service ended." << std::endl; // should not happen

			workerManager.interruptAndNotifyWorkers();
			goto joinall_and_exit;
		}

		S3Tk::initS3Global(progArgs); // inits threads and thus after potential service daemonize
		workerManager.prepareThreads();

		/* register signal handlers for clean worker stop and stats print after ctrl+c. This is not
			done in service mode, because a service shall just quit on an interrupt signal. */
		registerInterruptSignalHandlers();

		// special preparations if we're running in master mode
		if(!progArgs.getHostsVec().empty() )
		{
			// check if we were just called to tell service hosts to interrupt
			if(progArgs.getInterruptServices() || progArgs.getQuitServices() )
			{
				// tell RemoteWorkers to terminate
				workerManager.startNextPhase(BenchPhase_TERMINATE);
				goto joinall_and_exit;
			}

			// check that services accepted given settings and things are consistent across services
			workerManager.checkServiceBenchPathInfos();
		}

		if(progArgs.hasUserRequestedDryRun() )
			statistics.printDryRunInfo();
		else
		{
			waitForUserDefinedStartTime();

			runBenchmarks();
		}

		// signal workers to self-terminate
		workerManager.startNextPhase(BenchPhase_TERMINATE);
	}
	catch(WorkerException& e)
	{
		/* WorkerExceptions in main thread are only for errors which have already been noticed and
		   logged by workers themselves, so we don't print anything here. */
	}
	catch(ProgInterruptedException& e)
	{ // interrupted by signal, e.g. user pressed ctrl+c
		Logger() << e.what() << std::endl;
		workerManager.interruptAndNotifyWorkers();
		retVal = EXIT_FAILURE;
	}
	catch(ProgTimeLimitException& e)
	{ // user-defined phase time limit expired
		Logger() << e.what() << std::endl;
		workerManager.interruptAndNotifyWorkers();
		// this is a user-defined time limit, not an error, so no retVal=EXIT_FAILURE here.
	}
	catch(ProgException& e)
	{
		ErrLogger(Log_NORMAL, false) << e.what() << std::endl;
		workerManager.interruptAndNotifyWorkers();
		retVal = EXIT_FAILURE;
	}

joinall_and_exit:

	workerManager.joinAllThreads();

	if(!LoggerBase::getErrHistory().empty() )
	{
		std::cerr << LoggerBase::getErrHistory();
		LoggerBase::clearErrHistory();
	}

	S3Tk::uninitS3Global(progArgs);

	if( (retVal != EXIT_SUCCESS) || workerManager.getNumWorkersDoneWithError() )
		return EXIT_FAILURE;

	return EXIT_SUCCESS;
}

/**
 * Wait for user-defined start time (if any).
 *
 * @throw ProgException if user-defined start time has already passed.
 */
void Coordinator::waitForUserDefinedStartTime()
{
	if(progArgs.getStartTime() && (time(NULL) > progArgs.getStartTime() ) )
		throw ProgException("Defined start time has already passed. Aborting.");

	statistics.printLiveCountdown();

	while(time(NULL) < progArgs.getStartTime() )
		usleep(1000);
}

/**
 * Check if WorkersSharedData flags for interrupt signal or phase time limit are set.
 * This is intended to be called immediately before and after a benchmark phase.
 *
 * @throw ProgInterruptedException if interrupt flag set or phase timeout flag set.
 */
void Coordinator::checkInterruptionBetweenPhases()
{
	if(WorkersSharedData::gotUserInterruptSignal)
		throw ProgInterruptedException("Terminating due to interrupt signal.");

	if(WorkersSharedData::isPhaseTimeExpired)
		throw ProgTimeLimitException("Terminating due to phase time limit.");
}

/**
 * Run coordinated benchmark through workers, print live stats and wait for workers to finish.
 *
 * @throw WorkerException on worker error; ProgException on main thread error.
 */
void Coordinator::runBenchmarkPhase(BenchPhase newBenchPhase)
{
	// don't start next phase if interrupted (e.g. by ctrl+c) or timeout for previous phase expired
	checkInterruptionBetweenPhases();

	workerManager.startNextPhase(newBenchPhase);

	statistics.printLiveStats();

	workerManager.waitForWorkersDone();

	if(!LoggerBase::getErrHistory().empty() )
	{
		std::cerr << LoggerBase::getErrHistory();
		LoggerBase::clearErrHistory();
	}

	// print stats
	statistics.printPhaseResults();

	// check again for interrupted or timeout (might be last phase and we want to return an error)
	checkInterruptionBetweenPhases();
}

/**
 * Run sync and drop caches phases if selected by user.
 */
void Coordinator::runSyncAndDropCaches()
{
	// temporarily disable phase time limit, as sync and cache drop cannot be interrupted this way
	size_t oldTimeLimitTecs = progArgs.getTimeLimitSecs();
	progArgs.setTimeLimitSecs(0);

	if(progArgs.getRunSyncPhase() )
		runBenchmarkPhase(BenchPhase_SYNC);

	if(progArgs.getRunDropCachesPhase() )
		runBenchmarkPhase(BenchPhase_DROPCACHES);

	// restore user-defined phase time limit
	progArgs.setTimeLimitSecs(oldTimeLimitTecs);
}

/**
 * Run coordinated benchmarks through workers according to user selection.
 *
 * @throw WorkerException on worker error; ProgException on main thread error.
 */
void Coordinator::runBenchmarks()
{
	for(size_t iterationIndex = 0; iterationIndex < progArgs.getIterations(); iterationIndex++)
	{	
		statistics.printPhaseResultsTableHeader();

		runSyncAndDropCaches();

		if(progArgs.getRunCreateDirsPhase() )
		{
			runBenchmarkPhase(BenchPhase_CREATEDIRS);
			runSyncAndDropCaches();
		}

		if(progArgs.getRunCreateFilesPhase() )
		{
			runBenchmarkPhase(BenchPhase_CREATEFILES);
			runSyncAndDropCaches();
		}

		if(progArgs.getRunStatFilesPhase() )
		{
			runBenchmarkPhase(BenchPhase_STATFILES);
			runSyncAndDropCaches();
		}

		if(progArgs.getRunListObjPhase() )
		{
			runBenchmarkPhase(BenchPhase_LISTOBJECTS);
			runSyncAndDropCaches();
		}

		if(progArgs.getRunListObjParallelPhase() )
		{
			runBenchmarkPhase(BenchPhase_LISTOBJPARALLEL);
			runSyncAndDropCaches();
		}

		if(progArgs.getRunReadPhase() )
		{
			runBenchmarkPhase(BenchPhase_READFILES);
			runSyncAndDropCaches();
		}
	
		if(progArgs.getRunDeleteFilesPhase() )
		{
			runBenchmarkPhase(BenchPhase_DELETEFILES);
			runSyncAndDropCaches();
		}

		if(progArgs.getRunDeleteDirsPhase() )
		{
			runBenchmarkPhase(BenchPhase_DELETEDIRS);
			runSyncAndDropCaches();
		}
	}
}

/**
 * Set interrupted flag as a friendly ask for coordinator/workers to self-terminate. Also resets
 * to default signal handler, so that next attempt to interrupt will be successful if friendly way
 * did not work out.
 */
void Coordinator::handleInterruptSignal(int signal)
{
	// reset signal handler to default, so that next interrupt request will definitely interrupt
	std::signal(signal, SIG_DFL);

	WorkersSharedData::gotUserInterruptSignal = true;
}

/**
 * Register handler of SIGINT/SIGTERM.
 */
void Coordinator::registerInterruptSignalHandlers()
{
	std::signal(SIGINT, handleInterruptSignal);
	std::signal(SIGTERM, handleInterruptSignal);
}



