// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <algorithm>
#include <chrono>
#include <iostream>
#include <mutex>
#include <string>

#include "Common.h"
#include "Logger.h"
#include "statistics/ResultTerms.h"
#include "statistics/Statistics.h"
#include "toolkits/TranslatorTk.h"
#include "workers/RemoteWorker.h"
#include "workers/Worker.h"


/**
 * Get current processed entries sum and bytes written/read of all workers.
 *
 * @param outLiveOps entries/ops/bytes done for all workers (does not need to be initialized to 0).
 * @param outLiveRWMixReadOps rwmix mode read entries/ops/bytes done for all workers (does not need
 * 		to be initialized to 0).
 * @param outLiveLatency this method will call divAllByNumValues(), so struct values are avg across
 * 		all workers.
 */
void Statistics::getLiveOps(LiveOps& outLiveOps, LiveOps& outLiveRWMixReadOps,
	LiveLatency& outLiveLatency)
{
	outLiveOps = {}; // set all members to zero
	outLiveRWMixReadOps = {}; // set all members to zero
	outLiveLatency = {}; // set all members to zero

	for(size_t i=0; i < workerVec.size(); i++)
	{
        if(!isAggregated(i) )
            continue;

		workerVec[i]->getAndAddLiveOps(outLiveOps, outLiveRWMixReadOps);
		workerVec[i]->getAndAddLiveLatency(outLiveLatency);
	}
}

/**
 * Whether a worker's counters go into the totals. A netbench server sees the same bytes as its
 * clients, so the coordinator counts the clients only; a service counts all its local workers.
 */
bool Statistics::isAggregated(size_t workerIdx) const
{
    return progArgs.getHostsVec().empty() ||
        !progArgs.getNetBenchArgs().isServerService(workerIdx);
}

size_t Statistics::countAggregatedWorkers() const
{
    size_t numWorkers = 0;

    for(size_t i=0; i < workerVec.size(); i++)
        numWorkers += isAggregated(i);

    return numWorkers;
}

/**
 * Get live statistics for progress report of RemoteWorkers in service mode.
 */
void Statistics::getLiveStatsAsPropertyTreeForService(bpt::ptree& outTree)
{
	LiveOps liveOps;
	LiveOps liveOpsReadMix;
	LiveLatency liveLatency;

	getLiveOps(liveOps, liveOpsReadMix, liveLatency);

	std::chrono::seconds elapsedDurationSecs =
				std::chrono::duration_cast<std::chrono::seconds>
				(std::chrono::steady_clock::now() - workersSharedData.phaseStartT);
	size_t elapsedSecs = elapsedDurationSecs.count();

    std::unique_lock<std::mutex> lock(workersSharedData.mutex); // L O C K (scoped)

	outTree.put(XFER_STATS_BENCHID, workersSharedData.currentBenchID);
	outTree.put(XFER_STATS_BENCHPHASENAME,
		TranslatorTk::benchPhaseToPhaseName(workersSharedData.currentBenchPhase, &progArgs) );
	outTree.put(XFER_STATS_BENCHPHASECODE, workersSharedData.currentBenchPhase);
	outTree.put(XFER_STATS_NUMWORKERSDONE, workersSharedData.numWorkersDone);
	outTree.put(XFER_STATS_NUMWORKERSDONEWITHERR, workersSharedData.numWorkersDoneWithError);
	outTree.put(XFER_STATS_TRIGGERSTONEWALL,
		!workerVec.empty() && workerVec[0]->getStoneWallTriggered());

    lock.unlock(); // U N L O C K

	outTree.put(XFER_STATS_NUMENTRIESDONE, liveOps.numEntriesDone);
	outTree.put(XFER_STATS_NUMBYTESDONE, liveOps.numBytesDone);
	outTree.put(XFER_STATS_NUMIOPSDONE, liveOps.numIOPSDone);
	outTree.put(XFER_STATS_CPUUTIL, (unsigned) liveCpuUtil.getCPUUtilPercent() );
	outTree.put(XFER_STATS_ELAPSEDSECS, elapsedSecs);
	outTree.put(XFER_STATS_LAT_NUM_IOPS, liveLatency.numAvgIOLatValues);
	outTree.put(XFER_STATS_LAT_SUM_IOPS, liveLatency.avgIOLatMicroSecsSum);
	outTree.put(XFER_STATS_LAT_NUM_ENTRIES, liveLatency.numAvgEntriesLatValues);
	outTree.put(XFER_STATS_LAT_SUM_ENTRIES, liveLatency.avgEntriesLatMicroSecsSum);

	if(progArgs.hasRWMixReadCounters(workersSharedData.currentBenchPhase) )
	{
		outTree.put(XFER_STATS_NUMENTRIESDONE_RWMIXREAD, liveOpsReadMix.numEntriesDone);
		outTree.put(XFER_STATS_NUMBYTESDONE_RWMIXREAD, liveOpsReadMix.numBytesDone);
		outTree.put(XFER_STATS_NUMIOPSDONE_RWMIXREAD, liveOpsReadMix.numIOPSDone);

		outTree.put(XFER_STATS_LAT_NUM_IOPS_RWMIXREAD,
			liveLatency.numAvgIOLatReadMixValues);
		outTree.put(XFER_STATS_LAT_SUM_IOPS_RWMIXREAD,
			liveLatency.avgIOLatReadMixMicroSecsSum);
		outTree.put(XFER_STATS_LAT_NUM_ENTRIES_RWMIXREAD,
			liveLatency.numAvgEntriesLatReadMixValues);
		outTree.put(XFER_STATS_LAT_SUM_ENTRIES_RWMIXREAD,
			liveLatency.avgEntriesLatReadMixMicrosSecsSum);
	}

	outTree.put(XFER_STATS_ERRORHISTORY, LoggerBase::getErrHistory() );
}

/**
 * Gather statistics after all workers completed a phase.
 *
 * @phaseResults will contain gathered phase results if true is returned.
 * @return false if stats couldn't be generated due to unavailable worker results.
 */
bool Statistics::generatePhaseResults(PhaseResults& phaseResults)
{
	// check if results uninitialized. can happen if called in service mode before 1st run.
	IF_UNLIKELY(workerVec.empty() )
		return false;

	// (first worker might not have gotten work, so we can only init while we iterate over workers)
	bool firstAndLastFinishInitialized = false;

	// sum up total values
	for(size_t i=0; i < workerVec.size(); i++)
	{
        Worker* worker = workerVec[i];

        if(!isAggregated(i) )
            continue;

		if(worker->getElapsedUSecVec().empty() )
		{
			if(worker->getWorkerGotPhaseWork() )
			{
				ERRLOGGER(Log_NORMAL, "generate phase results: "
					"Worker triggers stonewall, but has no elapsed time. " <<
					"WorkerRank: " << worker->getRank() << std::endl);

				return false;
			}

			// worker doesn't trigger stonewall, so it's ok to not have elapsed time

			LOGGER(Log_DEBUG, "generate phase results: "
				"Worker has no elapsed time, also doesn't trigger stonewall. " <<
				"WorkerRank: " << worker->getRank() << std::endl);

			continue;
		}

		for(uint64_t elapsedUSec : worker->getElapsedUSecVec() )
		{
		    // init first/last elapsed milliseconds
		    if(!firstAndLastFinishInitialized)
            {
		        LOGGER(Log_DEBUG, "gen phase res: init first&last finished. "
		            "WorkerRank: " << worker->getRank() << std::endl);

                phaseResults.firstFinishUSec = elapsedUSec; // stonewall: time to compl of fastest
                phaseResults.lastFinishUSec = elapsedUSec; // time to completion of slowest worker

                firstAndLastFinishInitialized = true;
		        continue;
            }

			if(elapsedUSec < phaseResults.firstFinishUSec)
				phaseResults.firstFinishUSec = elapsedUSec;

			if(elapsedUSec > phaseResults.lastFinishUSec)
				phaseResults.lastFinishUSec = elapsedUSec;
		}

		worker->getAndAddLiveOps(phaseResults.opsTotal, phaseResults.opsTotalReadMix);
		worker->getAndAddStoneWallOps(phaseResults.opsStoneWallTotal,
			phaseResults.opsStoneWallTotalReadMix);
		phaseResults.iopsLatHisto += worker->getIOPSLatencyHistogram();
		phaseResults.iopsLatHistoReadMix += worker->getIOPSLatencyHistogramReadMix();
		phaseResults.entriesLatHisto += worker->getEntriesLatencyHistogram();
		phaseResults.entriesLatHistoReadMix += worker->getEntriesLatencyHistogramReadMix();

	} // end of for loop

	if(!firstAndLastFinishInitialized)
	{
        LOGGER(Log_DEBUG, "gen phase res: No worker provided elapsedUSec." << std::endl);
	    return false;
	}

	// total per sec for all workers by 1st finisher
	if(phaseResults.firstFinishUSec)
	{
		phaseResults.opsStoneWallTotal.getPerSecFromUSec(
			phaseResults.firstFinishUSec, phaseResults.opsStoneWallPerSec);
	}

	// rwmix read total per sec for all workers by 1st finisher
	if(phaseResults.firstFinishUSec && phaseResults.opsTotalReadMix.numBytesDone)
	{
		phaseResults.opsStoneWallTotalReadMix.getPerSecFromUSec(
			phaseResults.firstFinishUSec, phaseResults.opsStoneWallPerSecReadMix);
	}

	// total per sec for all workers by last finisher
	if(phaseResults.lastFinishUSec)
	{
		phaseResults.opsTotal.getPerSecFromUSec(
			phaseResults.lastFinishUSec, phaseResults.opsPerSec);
	}

	// rwmix read total per sec for all workers by last finisher
	if(phaseResults.lastFinishUSec && phaseResults.opsTotalReadMix.numBytesDone)
	{
		phaseResults.opsTotalReadMix.getPerSecFromUSec(
			phaseResults.lastFinishUSec, phaseResults.opsPerSecReadMix);
	}

	// cpu utilization
	if(progArgs.getHostsVec().empty() )
	{ // local mode => use vals from workersSharedData
		phaseResults.cpuUtilStoneWallPercent =
			workersSharedData.cpuUtilFirstDone.getCPUUtilPercent();
		phaseResults.cpuUtilPercent =
			workersSharedData.cpuUtilLastDone.getCPUUtilPercent();
	}
	else
	{ // master mode => calc average from remote values
		phaseResults.cpuUtilStoneWallPercent = 0;
		phaseResults.cpuUtilPercent = 0;

		for(Worker* worker : workerVec)
		{
			RemoteWorker* remoteWorker =  static_cast<RemoteWorker*>(worker);
			phaseResults.cpuUtilStoneWallPercent += remoteWorker->getCPUUtilStoneWall();
			phaseResults.cpuUtilPercent += remoteWorker->getCPUUtilLastDone();
		}

		phaseResults.cpuUtilStoneWallPercent /= workerVec.size();
		phaseResults.cpuUtilPercent /= workerVec.size();
	}

    if(progArgs.getShowServicesMinMax() && !progArgs.getHostsVec().empty() )
    {
        generateServicesMinMax(phaseResults);
    }

	return true;
}

/**
 * Per-service min/avg/max of IOPS and throughput, each service by its own slowest thread over
 * both counter sets (see ResultTerms::combine). Netbench servers and clients are not comparable,
 * so they form two groups.
 */
void Statistics::generateServicesMinMax(PhaseResults& phaseResults)
{
    const ResultTerms terms = ResultTerms::forPhase(progArgs, true);
    uint64_t sumIOPS[2] = {}, sumBytesPerSec[2] = {};
    size_t numServices[2] = {};

    auto update = [](ServiceMinMax& minMax, uint64_t perSec, const std::string& host)
    {
        if(minMax.minHost.empty() || (perSec < minMax.minPerSec) )
        {
            minMax.minPerSec = perSec;
            minMax.minHost = host;
        }

        if(minMax.maxHost.empty() || (perSec > minMax.maxPerSec) )
        {
            minMax.maxPerSec = perSec;
            minMax.maxHost = host;
        }
    };

    for(size_t i = 0; i < workerVec.size(); i++)
    {
        RemoteWorker* remoteWorker = static_cast<RemoteWorker*>(workerVec[i] );
        const UInt64Vec& elapsedVec = remoteWorker->getElapsedUSecVec();

        if(elapsedVec.empty() )
            continue; // (no phase work)

        const uint64_t slowestUSec = *std::max_element(elapsedVec.begin(), elapsedVec.end() );
        const bool isServer = progArgs.getNetBenchArgs().isServerService(i);
        LiveOps ops = {}, opsReadMix = {}, perSec = {};

        remoteWorker->getLiveOps(ops, opsReadMix);
        terms.combine(ops, opsReadMix);

        if(slowestUSec)
            ops.getPerSecFromUSec(slowestUSec, perSec);

        update(isServer ? phaseResults.svcServersIOPS : phaseResults.svcIOPS,
            perSec.numIOPSDone, remoteWorker->getHost() );
        update(isServer ? phaseResults.svcServersBytesPerSec : phaseResults.svcBytesPerSec,
            perSec.numBytesDone, remoteWorker->getHost() );

        sumIOPS[isServer] += perSec.numIOPSDone;
        sumBytesPerSec[isServer] += perSec.numBytesDone;
        numServices[isServer]++;
    }

    if(numServices[0])
    {
        phaseResults.svcIOPS.avgPerSec = sumIOPS[0] / numServices[0];
        phaseResults.svcBytesPerSec.avgPerSec = sumBytesPerSec[0] / numServices[0];
    }

    if(numServices[1])
    {
        phaseResults.svcServersIOPS.avgPerSec = sumIOPS[1] / numServices[1];
        phaseResults.svcServersBytesPerSec.avgPerSec = sumBytesPerSec[1] / numServices[1];
    }
}

/**
 * Get results of a completed benchmark phase.
 */
void Statistics::getBenchResultAsPropertyTreeForService(bpt::ptree& outTree)
{
	LiveOps liveOps;
	LiveOps liveOpsReadMix;
	LiveLatency liveLatency;
	LatencyHistogram iopsLatHisto; // sum of all histograms
	LatencyHistogram iopsLatHistoReadMix; // sum of all histograms
	LatencyHistogram entriesLatHisto; // sum of all histograms
	LatencyHistogram entriesLatHistoReadMix; // sum of all histograms

	getLiveOps(liveOps, liveOpsReadMix, liveLatency);

    std::unique_lock<std::mutex> lock(workersSharedData.mutex); // L O C K

	outTree.put(XFER_STATS_BENCHID, workersSharedData.currentBenchID);
	outTree.put(XFER_STATS_BENCHPHASENAME,
		TranslatorTk::benchPhaseToPhaseName(workersSharedData.currentBenchPhase, &progArgs) );
	outTree.put(XFER_STATS_BENCHPHASECODE, workersSharedData.currentBenchPhase);
	outTree.put(XFER_STATS_NUMWORKERSDONE, workersSharedData.numWorkersDone);
	outTree.put(XFER_STATS_NUMWORKERSDONEWITHERR, workersSharedData.numWorkersDoneWithError);

    lock.unlock(); // U N L O C K

	outTree.put(XFER_STATS_NUMENTRIESDONE, liveOps.numEntriesDone);
	outTree.put(XFER_STATS_NUMBYTESDONE, liveOps.numBytesDone);
	outTree.put(XFER_STATS_NUMIOPSDONE, liveOps.numIOPSDone);
	outTree.put(XFER_STATS_CPUUTIL_STONEWALL,
		(unsigned) workersSharedData.cpuUtilFirstDone.getCPUUtilPercent() );
	outTree.put(XFER_STATS_CPUUTIL,
		(unsigned) workersSharedData.cpuUtilLastDone.getCPUUtilPercent() );

	bool triggerStonewall = false; // true if at least one worker has this set to true

	for(Worker* worker : workerVec)
	{
		bool workerTriggersStonewall = worker->getWorkerGotPhaseWork();

		if(workerTriggersStonewall)
		{
			triggerStonewall = true;

			// add finishElapsedUSec of each worker
			for(uint64_t elapsedUSec : worker->getElapsedUSecVec() )
				outTree.add(XFER_STATS_ELAPSEDUSECLIST_ITEM, elapsedUSec);
		}

		iopsLatHisto += worker->getIOPSLatencyHistogram();
		entriesLatHisto += worker->getEntriesLatencyHistogram();

		if(progArgs.hasRWMixReadCounters(workersSharedData.currentBenchPhase) )
		{
			iopsLatHistoReadMix += worker->getIOPSLatencyHistogramReadMix();
			entriesLatHistoReadMix += worker->getEntriesLatencyHistogramReadMix();
		}
	}

	outTree.put(XFER_STATS_TRIGGERSTONEWALL, triggerStonewall);

	iopsLatHisto.getAsPropertyTreeForService(outTree, XFER_STATS_LAT_PREFIX_IOPS);
	entriesLatHisto.getAsPropertyTreeForService(outTree, XFER_STATS_LAT_PREFIX_ENTRIES);

	if(progArgs.hasRWMixReadCounters(workersSharedData.currentBenchPhase) )
	{
		outTree.put(XFER_STATS_NUMENTRIESDONE_RWMIXREAD, liveOpsReadMix.numEntriesDone);
		outTree.put(XFER_STATS_NUMBYTESDONE_RWMIXREAD, liveOpsReadMix.numBytesDone);
		outTree.put(XFER_STATS_NUMIOPSDONE_RWMIXREAD, liveOpsReadMix.numIOPSDone);

		iopsLatHistoReadMix.getAsPropertyTreeForService(outTree, XFER_STATS_LAT_PREFIX_IOPS_RWMIXREAD);
		entriesLatHistoReadMix.getAsPropertyTreeForService(outTree, XFER_STATS_LAT_PREFIX_ENTRIES_RWMIXREAD);
	}

	outTree.put(XFER_STATS_ERRORHISTORY, LoggerBase::getErrHistory() );
}

/**
 * Print dry run info (number of entries and dataset size) for each of the user-given phases.
 */
void Statistics::printDryRunInfo()
{
    const S3ProgArgs& s3Args = progArgs.getS3Args();

	if(progArgs.getNetBenchArgs().getUseNetBench() )
	{
		progArgs.getNetBenchArgs().printDryRunInfo(progArgs);
		return;
	}

	if(progArgs.getRunCreateDirsPhase() )
		printDryRunPhaseInfo(BenchPhase_CREATEDIRS);

	if(progArgs.getRunDeleteDirsPhase() )
		printDryRunPhaseInfo(BenchPhase_DELETEDIRS);

	if(progArgs.getRunCreateFilesPhase() )
		printDryRunPhaseInfo(BenchPhase_CREATEFILES);

	if(progArgs.getRunReadPhase() )
		printDryRunPhaseInfo(BenchPhase_READFILES);

	if(progArgs.getRunDeleteFilesPhase() )
		printDryRunPhaseInfo(BenchPhase_DELETEFILES);

	if(progArgs.getRunStatFilesPhase() )
		printDryRunPhaseInfo(BenchPhase_STATFILES);

	if(s3Args.getRunS3AclPut() )
		printDryRunPhaseInfo(BenchPhase_PUTOBJACL);

	if(s3Args.getRunS3AclGet() )
		printDryRunPhaseInfo(BenchPhase_GETOBJACL);

	if(s3Args.getRunS3BucketAclPut() )
		printDryRunPhaseInfo(BenchPhase_PUTBUCKETACL);

	if(s3Args.getRunS3BucketAclGet() )
		printDryRunPhaseInfo(BenchPhase_GETBUCKETACL);
}

/**
 * Print dry run info (num entries and dataset size) for a particular phase.
 */
void Statistics::printDryRunPhaseInfo(BenchPhase benchPhase)
{
	size_t numEntriesPerThread;
	uint64_t numBytesPerThread;

	WorkerManager::getPhaseNumEntriesAndBytes(progArgs, benchPhase, progArgs.getBenchPathType(),
		numEntriesPerThread, numBytesPerThread);

	std::string perUnitStr = progArgs.getHostsVec().empty() ?
		"thread" : "service";

	uint64_t totalMultiplier = progArgs.getHostsVec().empty() ?
		progArgs.getNumThreads() : progArgs.getHostsVec().size();

	uint64_t numEntriesTotal = numEntriesPerThread * totalMultiplier;
	uint64_t numBytesTotal = numBytesPerThread * totalMultiplier;

	std::string benchPhaseStr = TranslatorTk::benchPhaseToPhaseName(benchPhase, &progArgs);

	std::cout << "Phase: " << benchPhaseStr << std::endl;
	std::cout << "* Entries per " << perUnitStr << ": " << numEntriesPerThread << " | " <<
		(numEntriesPerThread / 1000) << " K" " | " <<
		(numEntriesPerThread / (1000*1000) ) << " M" << std::endl;
	std::cout << "* Entries total:      " << numEntriesTotal << " | " <<
		(numEntriesTotal / 1000) << " K" " | " <<
		(numEntriesTotal / (1000*1000) ) << " M" << std::endl;

	// show bytes info only for create/write & read phases
	if( (benchPhase != BenchPhase_CREATEFILES) && (benchPhase != BenchPhase_READFILES) )
		return;

	std::cout << "* Bytes per " << perUnitStr << ":   " << numBytesPerThread << " | " <<
		(numBytesPerThread / (1024*1024) ) << " MiB" " | " <<
		(numBytesPerThread / (1024*1024*1024) ) << " GiB" << std::endl;
	std::cout << "* Bytes total:        " << numBytesTotal << " | " <<
		(numBytesTotal / (1024*1024) ) << " MiB" " | " <<
		(numBytesTotal / (1024*1024*1024) ) << " GiB" << std::endl;
}

/**
 * Print the results of the completed phase (console, results file, csv and json file).
 */
void Statistics::printPhaseResults()
{
	PhaseResults phaseResults = {}; // zero init

    const bool genRes = generatePhaseResults(phaseResults);

    printer.printPhaseResults(genRes, phaseResults);
}
