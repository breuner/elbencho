// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <algorithm>
#include <boost/format.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

#include "Common.h"
#include "statistics/PhaseResultsPrinter.h"
#include "ProgException.h"
#include "statistics/ResultTerms.h"
#include "toolkits/FileTk.h"
#include "toolkits/TranslatorTk.h"
#include "toolkits/UnitTk.h"
#include "workers/RemoteWorker.h"
#include "workers/Worker.h"

#define PHASERESULTS_CONSOLE_SEPARATOR_LINE         "---"


/**
 * Print table header for phase results to stdout and also to results file (if specified by user).
 *
 * If a results file is set, current date and command line will also be printed to it.
 */
void PhaseResultsPrinter::printPhaseResultsTableHeader()
{
	// print to console
	printPhaseResultsTableHeaderToStream(std::cout);

	// print to human-readable results file (if specified by user)
	if(!progArgs.getResFilePathTXT().empty() )
	{
		std::ofstream fileStream;

		fileStream.open(progArgs.getResFilePathTXT(), std::ofstream::app);

		if(!fileStream)
		{
			std::cerr << "ERROR: Opening results file failed: " << progArgs.getResFilePathTXT() <<
				std::endl;

			return;
		}

		// print current date

		std::time_t currentTime = std::time(NULL);

        struct tm localTimeInfo;
        localtime_r(&currentTime, &localTimeInfo);

		fileStream << "ISO DATE: " << std::put_time(&localTimeInfo, "%FT%T%z") << std::endl;

		// print user-defined label

		if(!progArgs.getBenchLabel().empty() )
			fileStream << "LABEL: " << progArgs.getBenchLabel() << std::endl;

		// print command line

		fileStream << "COMMAND LINE: ";

		for(int i=0; i < progArgs.getProgArgCount(); i++)
        {
            if(!strcmp(progArgs.getProgArgVec()[i], "--" ARG_S3ACCESSSECRET_LONG) )
            { // skip parameter for s3 secret
                i += 1;
                continue;
            }

            fileStream << "\"" << progArgs.getProgArgVec()[i] << "\" ";
        }

		fileStream << std::endl;

		// blank line
		fileStream << std::endl;


		printPhaseResultsTableHeaderToStream(fileStream);
		fileStream.close();

		if(!fileStream)
		{
			std::cerr << "ERROR: Writing to results file failed: " << progArgs.getResFilePathTXT() <<
				std::endl;

			return;
		}
	}

	// print to CSV results file (if specified by user)
	if(!progArgs.getResFilePathCSV().empty() && progArgs.getPrintCSVLabels() &&
		FileTk::checkFileEmpty(progArgs.getResFilePathCSV() ) )
	{
		std::ofstream fileStream;

		fileStream.open(progArgs.getResFilePathCSV(), std::ofstream::app);

		if(!fileStream)
		{
			std::cerr << "ERROR: Opening CSV results file failed: " << progArgs.getResFilePathCSV() <<
				std::endl;

			return;
		}

		StringVec labelsVec;
		StringVec resultsVec;
		PhaseResults zeroResults = {};

		printPhaseStartISODateToStringVec(labelsVec, resultsVec);
		progArgs.getAsStringVec(labelsVec, resultsVec);
		printPhaseResultsToStringVec(zeroResults, labelsVec, resultsVec);

		std::string labelsCSVStr = TranslatorTk::stringVecToString(labelsVec, ",");

		fileStream << labelsCSVStr << std::endl;

		fileStream.close();

		if(!fileStream)
		{
			std::cerr << "ERROR: Writing to CSV results file failed: " <<
				progArgs.getResFilePathTXT() << std::endl;

			return;
		}
	}
}

/**
 * Get ISO date & time to string vector, e.g. to later use it for CSV output.
 */
void PhaseResultsPrinter::printPhaseStartISODateToStringVec(StringVec& outLabelsVec,
    StringVec& outResultsVec)
{
    time_t time = std::chrono::system_clock::to_time_t(workersSharedData.phaseStartLocalT);
    auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        workersSharedData.phaseStartLocalT.time_since_epoch()).count() % 1000;

    struct tm localTimeInfo;
    localtime_r(&time, &localTimeInfo);

    std::stringstream dateStream;
    dateStream << std::put_time(&localTimeInfo, "%FT%T") << "."
        << std::setfill('0') << std::setw(3) << milliseconds
        << std::put_time(&localTimeInfo, "%z");

    outLabelsVec.push_back("ISO date");
    outResultsVec.push_back(dateStream.str() );
}

/**
 * Print table header for phase results.
 *
 * @outstream where to print results to.
 */
void PhaseResultsPrinter::printPhaseResultsTableHeaderToStream(std::ostream& outStream)
{
	outStream << boost::format(phaseResultsFormatStr)
		% "OPERATION"
		% "RESULT TYPE"
		% ""
		% "FIRST DONE"
		% "LAST DONE"
		<< std::endl;
	outStream << boost::format(phaseResultsFormatStr)
		% "==========="
		% "================"
		% ""
		% "=========="
		% "========="
		<< std::endl;
}

/**
 * Print statistics to console and also to file (if set by user) after all workers completed a
 * phase.
 */
/**
 * Print the results of the completed phase to console, results file, csv file and json file.
 *
 * @param genRes false if the results could not be generated, so only a note gets printed.
 */
void PhaseResultsPrinter::printPhaseResults(bool genRes, const PhaseResults& phaseResults)
{
    if(!genRes)
        std::cout << "Phase: " << TranslatorTk::benchPhaseToPhaseName(
            workersSharedData.currentBenchPhase, &progArgs) << ": "
            "Skipping stats print due to unavailable worker results." << std::endl <<
            PHASERESULTS_CONSOLE_SEPARATOR_LINE << std::endl;
    else
        printPhaseResultsToStream(phaseResults, std::cout); // print to console


    // print to results file (if specified by user)
    if(!progArgs.getResFilePathTXT().empty() )
	{
		std::ofstream fileStream;

		fileStream.open(progArgs.getResFilePathTXT(), std::ofstream::app);

		if(!fileStream)
		{
			std::cerr << "ERROR: Opening results file failed: " << progArgs.getResFilePathTXT() <<
				std::endl;

			return;
		}

		if(!genRes)
			fileStream << "Skipping stats print due to unavailable worker results." << std::endl;
		else
			printPhaseResultsToStream(phaseResults, fileStream);

		fileStream << std::endl;
	}

	// print to results CSV file (if specified by user)
	if(genRes && !progArgs.getResFilePathCSV().empty() )
	{
		std::ofstream fileStream;

		fileStream.open(progArgs.getResFilePathCSV(), std::ofstream::app);

		if(!fileStream)
		{
			std::cerr << "ERROR: Opening results CSV file failed: " << progArgs.getResFilePathCSV() <<
				std::endl;

			return;
		}

        StringVec labelsVec;
        StringVec resultsVec;

        printPhaseStartISODateToStringVec(labelsVec, resultsVec);
        progArgs.getAsStringVec(labelsVec, resultsVec);
        printPhaseResultsToStringVec(phaseResults, labelsVec, resultsVec);

        std::string resultsCSVStr = TranslatorTk::stringVecToString(resultsVec, ",");

        fileStream << resultsCSVStr << std::endl;
	}

    // print to results JSON file (if specified by user)
    if(genRes && !progArgs.getResFilePathJSON().empty() )
        printPhaseResultsAsJSON(phaseResults);
}

/**
 * Print phase result statistics to given stream.
 *
 * @outStream where to print results to.
 */
void PhaseResultsPrinter::printPhaseResultsToStream(const PhaseResults& phaseResults,
	std::ostream& outStream)
{
	std::string phaseName =
		TranslatorTk::benchPhaseToPhaseName(workersSharedData.currentBenchPhase, &progArgs);
	std::string entryTypeUpperCase = TranslatorTk::benchPhaseToPhaseEntryType(
        workersSharedData.currentBenchPhase, &progArgs, true);
	std::string entryTypeLowerCase = TranslatorTk::benchPhaseToPhaseEntryType(
        workersSharedData.currentBenchPhase, &progArgs, false);
    std::string throughputUnitStr = progArgs.getShowThroughputBase10() ? "MB/s" : "MiB/s";
    uint64_t throughputDivisor = progArgs.getShowThroughputBase10() ? (1000*1000) : (1024*1024);

    const ResultTerms terms = ResultTerms::forPhase(progArgs,
        phaseResults.opsTotalReadMix.numBytesDone || phaseResults.opsTotalReadMix.numEntriesDone);

	const bool showDirStats = progArgs.getShowDirStats() &&
		(progArgs.getBenchPathType() == BenchPathType_DIR) &&
		progArgs.getTreeFilePath().empty() &&
		( (workersSharedData.currentBenchPhase == BenchPhase_CREATEFILES) ||
			(workersSharedData.currentBenchPhase == BenchPhase_READFILES) );

	// elapsed time (the phase name may exceed the column, e.g. "NET/libfabric")
	outStream << boost::format(PhaseResultsPrinter::phaseResultsFormatStr)
		% phaseName.substr(0, operationColWidth)
		% "Elapsed time"
		% ":"
		% UnitTk::elapsedMSToHumanStr(phaseResults.firstFinishUSec / 1000)
		% UnitTk::elapsedMSToHumanStr(phaseResults.lastFinishUSec / 1000)
		<< std::endl;

	// entries (dirs/files) per second
	if(phaseResults.opsTotal.numEntriesDone)
	{
		outStream << boost::format(PhaseResultsPrinter::phaseResultsFormatStr)
			% ""
			% (entryTypeUpperCase + "/s" + terms.suffix(terms.entriesPrimary) )
			% ":"
			% phaseResults.opsStoneWallPerSec.numEntriesDone
			% phaseResults.opsPerSec.numEntriesDone
			<< std::endl;
	}

	// dirs per second in file read/write phase of dir mode
	if(showDirStats && phaseResults.opsTotal.numEntriesDone)
	{
		outStream << boost::format(PhaseResultsPrinter::phaseResultsFormatStr)
			% ""
			% ("Dirs/s" + terms.suffix(terms.entriesPrimary) )
			% ":"
			% (phaseResults.opsStoneWallPerSec.numEntriesDone / progArgs.getNumFiles() )
			% (phaseResults.opsPerSec.numEntriesDone / progArgs.getNumFiles() )
			<< std::endl;
	}

    // rwmix read entries (dirs/files) per second
    if(phaseResults.opsTotalReadMix.numEntriesDone)
    {
        outStream << boost::format(PhaseResultsPrinter::phaseResultsFormatStr)
            % ""
            % terms.secondaryLabel(entryTypeUpperCase + "/s")
            % ":"
            % phaseResults.opsStoneWallPerSecReadMix.numEntriesDone
            % phaseResults.opsPerSecReadMix.numEntriesDone
            << std::endl;

        if(terms.showTotal)
            outStream << boost::format(PhaseResultsPrinter::phaseResultsFormatStr)
                % ""
                % terms.totalLabel(entryTypeUpperCase + "/s")
                % ":"
                % (phaseResults.opsStoneWallPerSec.numEntriesDone +
                    phaseResults.opsStoneWallPerSecReadMix.numEntriesDone)
                % (phaseResults.opsPerSec.numEntriesDone +
                    phaseResults.opsPerSecReadMix.numEntriesDone)
                << std::endl;
    }

	// dirs per second in rwmix file read phase of dir mode
	if(showDirStats && phaseResults.opsTotalReadMix.numEntriesDone)
	{
		outStream << boost::format(PhaseResultsPrinter::phaseResultsFormatStr)
			% ""
			% terms.secondaryLabel("Dirs/s")
			% ":"
			% (phaseResults.opsStoneWallPerSecReadMix.numEntriesDone / progArgs.getNumFiles() )
			% (phaseResults.opsPerSecReadMix.numEntriesDone / progArgs.getNumFiles() )
			<< std::endl;
	}

    // IOPS
    if(phaseResults.opsTotal.numIOPSDone)
    {
        /* print iops only if path is bdev/file; or in dir mode when each file consists of more than
            one block read/write (because otherwise iops is equal to files/s) */
        if( (progArgs.getBenchPathType() != BenchPathType_DIR) ||
            (progArgs.getBlockSize() != progArgs.getFileSize() ) ||
            (!phaseResults.opsTotal.numEntriesDone) )
            outStream << boost::format(PhaseResultsPrinter::phaseResultsFormatStr)
                % ""
                % ("IOPS" + terms.suffix(terms.primary) )
                % ":"
                % phaseResults.opsStoneWallPerSec.numIOPSDone
                % phaseResults.opsPerSec.numIOPSDone
                << std::endl;
    }

    // IOPS rwmix read
    if(phaseResults.opsTotalReadMix.numIOPSDone)
    {
        /* print iops only if path is bdev/file; or in dir mode when each file consists of more than
            one block read/write (because otherwise iops is equal to files/s) */
        if( (progArgs.getBenchPathType() != BenchPathType_DIR) ||
            (progArgs.getBlockSize() != progArgs.getFileSize() ) ||
            (!phaseResults.opsTotalReadMix.numEntriesDone) )
        {
            outStream << boost::format(PhaseResultsPrinter::phaseResultsFormatStr)
                % ""
                % terms.secondaryLabel("IOPS")
                % ":"
                % phaseResults.opsStoneWallPerSecReadMix.numIOPSDone
                % phaseResults.opsPerSecReadMix.numIOPSDone
                << std::endl;

            if(terms.showTotal)
                outStream << boost::format(PhaseResultsPrinter::phaseResultsFormatStr)
                    % ""
                    % terms.totalLabel("IOPS")
                    % ":"
                    % (phaseResults.opsStoneWallPerSec.numIOPSDone +
                        phaseResults.opsStoneWallPerSecReadMix.numIOPSDone)
                    % (phaseResults.opsPerSec.numIOPSDone +
                        phaseResults.opsPerSecReadMix.numIOPSDone)
                    << std::endl;
        }
    }

    // bytes per second total for all workers
    if(phaseResults.opsTotal.numBytesDone)
    {
        outStream << boost::format(PhaseResultsPrinter::phaseResultsFormatStr)
            % ""
            % terms.label(throughputUnitStr, "Throughput " + throughputUnitStr)
            % ":"
            % (phaseResults.opsStoneWallPerSec.numBytesDone / throughputDivisor)
            % (phaseResults.opsPerSec.numBytesDone / throughputDivisor)
            << std::endl;
    }

    // rwmix read bytes per second total for all workers
    if(phaseResults.opsTotalReadMix.numBytesDone)
    {
        outStream << boost::format(PhaseResultsPrinter::phaseResultsFormatStr)
            % ""
            % terms.secondaryLabel(throughputUnitStr)
            % ":"
            % (phaseResults.opsStoneWallPerSecReadMix.numBytesDone / throughputDivisor)
            % (phaseResults.opsPerSecReadMix.numBytesDone / throughputDivisor)
            << std::endl;

        if(terms.showTotal)
            outStream << boost::format(PhaseResultsPrinter::phaseResultsFormatStr)
                % ""
                % terms.totalLabel(throughputUnitStr)
                % ":"
                % ( (phaseResults.opsStoneWallPerSec.numBytesDone +
                    phaseResults.opsStoneWallPerSecReadMix.numBytesDone) / throughputDivisor)
                % ( (phaseResults.opsPerSec.numBytesDone +
                    phaseResults.opsPerSecReadMix.numBytesDone) / throughputDivisor)
                << std::endl;
    }

	// sum of bytes read/written
	if(phaseResults.opsTotal.numBytesDone)
	{
		outStream << boost::format(PhaseResultsPrinter::phaseResultsFormatStr)
			% ""
			% terms.label("MiB", "Total MiB")
			% ":"
			% (phaseResults.opsStoneWallTotal.numBytesDone / (1024*1024) )
			% (phaseResults.opsTotal.numBytesDone / (1024*1024) )
			<< std::endl;
	}

	// rwmix read sum of bytes
	if(phaseResults.opsTotalReadMix.numBytesDone)
	{
		outStream << boost::format(PhaseResultsPrinter::phaseResultsFormatStr)
			% ""
			% terms.secondaryLabel("MiB")
			% ":"
			% (phaseResults.opsStoneWallTotalReadMix.numBytesDone / (1024*1024) )
			% (phaseResults.opsTotalReadMix.numBytesDone / (1024*1024) )
			<< std::endl;
	}

	// entries (dirs/files) processed
	if(phaseResults.opsTotal.numEntriesDone)
	{
		outStream << boost::format(PhaseResultsPrinter::phaseResultsFormatStr)
			% ""
			% terms.entriesLabel(entryTypeUpperCase, entryTypeUpperCase + " total")
			% ":"
			% phaseResults.opsStoneWallTotal.numEntriesDone
			% phaseResults.opsTotal.numEntriesDone
			<< std::endl;
	}

	// dirs processed in file read/write phase of dir mode
	if(showDirStats && phaseResults.opsTotal.numEntriesDone)
	{
		outStream << boost::format(PhaseResultsPrinter::phaseResultsFormatStr)
			% ""
			% terms.entriesLabel("Dirs", "Dirs total")
			% ":"
			% (phaseResults.opsStoneWallTotal.numEntriesDone / progArgs.getNumFiles() )
			% (phaseResults.opsTotal.numEntriesDone / progArgs.getNumFiles() )
			<< std::endl;
	}

	// rwmix read entries (dirs/files) processed
	if(phaseResults.opsTotalReadMix.numEntriesDone)
	{
		outStream << boost::format(PhaseResultsPrinter::phaseResultsFormatStr)
			% ""
			% terms.secondaryLabel(entryTypeUpperCase)
			% ":"
			% phaseResults.opsStoneWallTotalReadMix.numEntriesDone
			% phaseResults.opsTotalReadMix.numEntriesDone
			<< std::endl;
	}

	// dis processed in rwmix file read phase of dir mode
	if(showDirStats && phaseResults.opsTotalReadMix.numEntriesDone)
	{
		outStream << boost::format(PhaseResultsPrinter::phaseResultsFormatStr)
			% ""
			% terms.secondaryLabel("Dirs")
			% ":"
			% (phaseResults.opsStoneWallTotalReadMix.numEntriesDone / progArgs.getNumFiles() )
			% (phaseResults.opsTotalReadMix.numEntriesDone / progArgs.getNumFiles() )
			<< std::endl;
	}

	// IOs (number of blocks read/written)
	if(phaseResults.opsTotal.numIOPSDone)
	{
		if(progArgs.getLogLevel() > Log_NORMAL)
			outStream << boost::format(PhaseResultsPrinter::phaseResultsFormatStr)
				% ""
				% terms.label("IOs", "IOs total")
				% ":"
				% phaseResults.opsStoneWallTotal.numIOPSDone
				% phaseResults.opsTotal.numIOPSDone
				<< std::endl;
	}

	// rwmix read IOs (number of blocks read)
	if(phaseResults.opsTotalReadMix.numIOPSDone)
	{
		if(progArgs.getLogLevel() > Log_NORMAL)
			outStream << boost::format(PhaseResultsPrinter::phaseResultsFormatStr)
				% ""
				% terms.secondaryLabel("IOs")
				% ":"
				% phaseResults.opsStoneWallTotalReadMix.numIOPSDone
				% phaseResults.opsTotalReadMix.numIOPSDone
				<< std::endl;
	}

	// cpu utilization
	if(progArgs.getShowCPUUtilization() )
	{
		outStream << boost::format(PhaseResultsPrinter::phaseResultsFormatStr)
			% ""
			% "CPU util %"
			% ":"
			% std::to_string( (unsigned) phaseResults.cpuUtilStoneWallPercent)
			% std::to_string( (unsigned) phaseResults.cpuUtilPercent)
			<< std::endl;
	}

	// print individual elapsed time results for each worker
	if(progArgs.getShowAllElapsed() )
	{
		outStream << boost::format(PhaseResultsPrinter::phaseResultsLeftFormatStr)
			% ""
			% "Time ms each"
			% ":";

		outStream << "[ ";

		// print elapsed milliseconds of each I/O thread
		for(Worker* worker : workerVec)
		{
			for(uint64_t elapsedUSec : worker->getElapsedUSecVec() )
				outStream << (elapsedUSec / 1000) << " ";
		}

		outStream << "]" << std::endl;
	}

	// print hosts sorted from fastest to slowest (if this was a distributed run)
	if(progArgs.getShowServicesElapsed() && !progArgs.getHostsVec().empty() )
	{
		outStream << boost::format(PhaseResultsPrinter::phaseResultsLeftFormatStr)
			% ""
			% "Svc compl. time"
			% ":";

		outStream << "[ ";

		typedef std::multimap<uint64_t, std::string> CompletionTimeMultiMap;
		typedef CompletionTimeMultiMap::value_type CompletionTimeMultiMapVal;
		CompletionTimeMultiMap completionTimeMap; // key is time ms, val is svc name

		// build map by slowest finisher of each service instance
		for(Worker* worker : workerVec)
		{
			RemoteWorker* remoteWorker = static_cast<RemoteWorker*>(worker);
			uint64_t slowestThreadUSec = 0;

			// find slowest thread of this service instance
			for(uint64_t elapsedUSec : worker->getElapsedUSecVec() )
			{
				if(elapsedUSec > slowestThreadUSec)
					slowestThreadUSec = elapsedUSec;
			}

			completionTimeMap.insert(
				CompletionTimeMultiMapVal(slowestThreadUSec/1000, remoteWorker->getHost() ) );
		}

		// services are now ordered by completion time of their slowest thread through the map

		// print ordered list of services ascending by slowest thread completion time
		for(const CompletionTimeMultiMapVal& mapVal : completionTimeMap)
			outStream << mapVal.second << "=" << UnitTk::elapsedMSToHumanStr(mapVal.first) << " ";

		outStream << "]" << std::endl;
	}

    // per-service min/avg/max rows (if this was a distributed run)
    if(progArgs.getShowServicesMinMax() && !progArgs.getHostsVec().empty() )
    {
        auto printRow = [&](const std::string& label, const ServiceMinMax& minMax,
            uint64_t divisor)
        {
            if(minMax.minHost.empty() )
                return;

            outStream << boost::format(PhaseResultsPrinter::phaseResultsLeftFormatStr)
                % ""
                % label
                % ":";

            outStream << "[ "
                "min=" << (minMax.minPerSec / divisor) << " (" << minMax.minHost << ") "
                "avg=" << (minMax.avgPerSec / divisor) << " "
                "max=" << (minMax.maxPerSec / divisor) << " (" << minMax.maxHost << ") ]" <<
                std::endl;
        };

        if(phaseResults.opsTotal.numIOPSDone || phaseResults.opsTotalReadMix.numIOPSDone)
        {
            printRow(terms.servicesLabel + " IOPS", phaseResults.svcIOPS, 1);
            printRow("Servers IOPS", phaseResults.svcServersIOPS, 1);
        }

        if(phaseResults.opsTotal.numBytesDone || phaseResults.opsTotalReadMix.numBytesDone)
        {
            printRow(terms.servicesLabel + " " + throughputUnitStr, phaseResults.svcBytesPerSec,
                throughputDivisor);
            printRow("Servers " + throughputUnitStr, phaseResults.svcServersBytesPerSec,
                throughputDivisor);
        }
    }

	// entries & iops latency results
	printPhaseResultsLatencyToStream(phaseResults.entriesLatHisto,
		entryTypeUpperCase + (terms.entriesPrimary.empty() ? "" : " " + terms.primaryShort),
        outStream);
	printPhaseResultsLatencyToStream(phaseResults.entriesLatHistoReadMix,
		entryTypeUpperCase + " " + terms.secondaryShort, outStream);

	printPhaseResultsLatencyToStream(phaseResults.iopsLatHisto,
		"IO" + terms.suffix(terms.primaryShort), outStream);
	printPhaseResultsLatencyToStream(phaseResults.iopsLatHistoReadMix,
		"IO " + terms.secondaryShort, outStream);

	// warn in case of invalid results
	if( (phaseResults.firstFinishUSec == 0) && !progArgs.getIgnore0USecErrors() )
	{
		/* very fast completion, so notify user about possibly useless results (due to accuracy,
			but also because we show 0 op/s to avoid division by 0 */

		outStream << "WARNING: Fastest worker thread completed in less than 1 microsecond, "
			"so results might not be useful (some op/s are shown as 0). You might want to try a "
			"larger data set. Otherwise, option '--" ARG_IGNORE0USECERR_LONG "' disables this "
			"message.)" << std::endl;
	}

	// print horizontal separator between phases
	outStream << PHASERESULTS_CONSOLE_SEPARATOR_LINE << std::endl;
}

/**
 * Print phase result statistics to StringVec, e.g. for the StringVec to be turned into CSV.
 *
 * This can be called with all phaseResults set to 0 if caller is only interested in outLabelsVec.
 */
void PhaseResultsPrinter::printPhaseResultsToStringVec(const PhaseResults& phaseResults,
	StringVec& outLabelsVec, StringVec& outResultsVec)
{
	std::string phaseName =
		TranslatorTk::benchPhaseToPhaseName(workersSharedData.currentBenchPhase, &progArgs);

	outLabelsVec.push_back("operation");
	outResultsVec.push_back(phaseName);

	// elapsed time

	outLabelsVec.push_back("time ms [first]");
	outResultsVec.push_back(std::to_string(phaseResults.firstFinishUSec / 1000) );

	outLabelsVec.push_back("time ms [last]");
	outResultsVec.push_back(std::to_string(phaseResults.lastFinishUSec / 1000) );

	// entries per second

	outLabelsVec.push_back("entries/s [first]");
	outResultsVec.push_back(!phaseResults.opsTotal.numEntriesDone ?
		"" : std::to_string(phaseResults.opsStoneWallPerSec.numEntriesDone) );

	outLabelsVec.push_back("entries/s [last]");
	outResultsVec.push_back(!phaseResults.opsTotal.numEntriesDone ?
		"" : std::to_string(phaseResults.opsPerSec.numEntriesDone) );

	// IOPS

	outLabelsVec.push_back("IOPS [first]");
	outResultsVec.push_back(!phaseResults.opsTotal.numIOPSDone ?
		"" : std::to_string(phaseResults.opsStoneWallPerSec.numIOPSDone) );

	outLabelsVec.push_back("IOPS [last]");
	outResultsVec.push_back(!phaseResults.opsTotal.numIOPSDone ?
		"" : std::to_string(phaseResults.opsPerSec.numIOPSDone) );

	// MiB/s

	outLabelsVec.push_back("MiB/s [first]");
	outResultsVec.push_back(!phaseResults.opsTotal.numBytesDone ?
		"" : std::to_string(phaseResults.opsStoneWallPerSec.numBytesDone / (1024*1024) ) );

	outLabelsVec.push_back("MiB/s [last]");
	outResultsVec.push_back(!phaseResults.opsTotal.numBytesDone ?
		"" : std::to_string(phaseResults.opsPerSec.numBytesDone / (1024*1024) ) );

	// cpu utilization

	outLabelsVec.push_back("CPU% [first]");
	outResultsVec.push_back(std::to_string( (unsigned)phaseResults.cpuUtilStoneWallPercent) );

	outLabelsVec.push_back("CPU% [last]");
	outResultsVec.push_back(std::to_string( (unsigned)phaseResults.cpuUtilPercent) );

	// entries

	outLabelsVec.push_back("entries [first]");
	outResultsVec.push_back(!phaseResults.opsTotal.numEntriesDone ?
		"" : std::to_string(phaseResults.opsStoneWallTotal.numEntriesDone) );

	outLabelsVec.push_back("entries [last]");
	outResultsVec.push_back(!phaseResults.opsTotal.numEntriesDone ?
		"" : std::to_string(phaseResults.opsTotal.numEntriesDone) );

	// MiB

	outLabelsVec.push_back("MiB [first]");
	outResultsVec.push_back(!phaseResults.opsTotal.numBytesDone ?
		"" : std::to_string(phaseResults.opsStoneWallTotal.numBytesDone / (1024*1024) ) );

	outLabelsVec.push_back("MiB [last]");
	outResultsVec.push_back(!phaseResults.opsTotal.numBytesDone ?
		"" : std::to_string(phaseResults.opsTotal.numBytesDone / (1024*1024) ) );

	// entries & iops latency results

	printPhaseResultsLatencyToStringVec(phaseResults.entriesLatHisto, "Ent",
		outLabelsVec, outResultsVec);
	printPhaseResultsLatencyToStringVec(phaseResults.iopsLatHisto, "IO",
		outLabelsVec, outResultsVec);

	// rwmix read entries per second

	outLabelsVec.push_back("rwmix read entries/s [first]");
	outResultsVec.push_back(!phaseResults.opsTotalReadMix.numEntriesDone ?
		"" : std::to_string(phaseResults.opsStoneWallPerSecReadMix.numEntriesDone) );

	outLabelsVec.push_back("rwmix read entries/s [last]");
	outResultsVec.push_back(!phaseResults.opsTotalReadMix.numEntriesDone ?
		"" : std::to_string(phaseResults.opsPerSecReadMix.numEntriesDone) );

	// rwmix read IOPS

	outLabelsVec.push_back("rwmix read IOPS [first]");
	outResultsVec.push_back(!phaseResults.opsTotalReadMix.numIOPSDone ?
		"" : std::to_string(phaseResults.opsStoneWallPerSecReadMix.numIOPSDone) );

	outLabelsVec.push_back("rwmix read IOPS [last]");
	outResultsVec.push_back(!phaseResults.opsTotalReadMix.numIOPSDone ?
		"" : std::to_string(phaseResults.opsPerSecReadMix.numIOPSDone) );

	// rwmix read MiB/s

	outLabelsVec.push_back("rwmix read MiB/s [first]");
	outResultsVec.push_back(!phaseResults.opsTotalReadMix.numBytesDone ?
		"" : std::to_string(phaseResults.opsStoneWallPerSecReadMix.numBytesDone / (1024*1024) ) );

	outLabelsVec.push_back("rwmix read MiB/s [last]");
	outResultsVec.push_back(!phaseResults.opsTotalReadMix.numBytesDone ?
		"" : std::to_string(phaseResults.opsPerSecReadMix.numBytesDone / (1024*1024) ) );

	// rwmix read entries

	outLabelsVec.push_back("rwmix read entries [first]");
	outResultsVec.push_back(!phaseResults.opsTotalReadMix.numEntriesDone ?
		"" : std::to_string(phaseResults.opsStoneWallTotalReadMix.numEntriesDone) );

	outLabelsVec.push_back("rwmix read entries [last]");
	outResultsVec.push_back(!phaseResults.opsTotalReadMix.numEntriesDone ?
		"" : std::to_string(phaseResults.opsTotalReadMix.numEntriesDone) );

	// rwmix read MiB

	outLabelsVec.push_back("rwmix read MiB [first]");
	outResultsVec.push_back(!phaseResults.opsTotalReadMix.numBytesDone ?
		"" : std::to_string(phaseResults.opsStoneWallTotalReadMix.numBytesDone / (1024*1024) ) );

	outLabelsVec.push_back("rwmix read MiB [last]");
	outResultsVec.push_back(!phaseResults.opsTotalReadMix.numBytesDone ?
		"" : std::to_string(phaseResults.opsTotalReadMix.numBytesDone / (1024*1024) ) );

	// rwmix read entries & iops latency results

	printPhaseResultsLatencyToStringVec(phaseResults.entriesLatHistoReadMix, "rwmix read Ent",
		outLabelsVec, outResultsVec);
	printPhaseResultsLatencyToStringVec(phaseResults.iopsLatHistoReadMix, "rwmix read IO",
		outLabelsVec, outResultsVec);

	// elbencho version

	outLabelsVec.push_back("version");
	outResultsVec.push_back(EXE_VERSION);

	// command line

	std::ostringstream cmdStream;
	std::string cmdString;

    for(int i=0; i < progArgs.getProgArgCount(); i++)
    {
        if(!strcmp(progArgs.getProgArgVec()[i], "--" ARG_S3ACCESSSECRET_LONG) )
        { // skip over s3 secret
            i += 1;
            continue;
        }

        cmdStream << "\"" << progArgs.getProgArgVec()[i] << "\" ";
    }

	cmdString = cmdStream.str();
	std::replace(cmdString.begin(), cmdString.end(), ',', ' '); // replace all commas with spaces

	outLabelsVec.push_back("command");
	outResultsVec.push_back(cmdString);
}

/**
 * Print latency results as sub-task of printPhaseResults().
 *
 * @latHisto the latency histogram for which to print the results.
 * @latTypeStr short latency type description, e.g. for entries or iops.
 * @outstream where to print results to.
 */
void PhaseResultsPrinter::printPhaseResultsLatencyToStream(const LatencyHistogram& latHisto,
	std::string latTypeStr, std::ostream& outStream)
{
	std::string phaseName =
		TranslatorTk::benchPhaseToPhaseName(workersSharedData.currentBenchPhase, &progArgs);

	// print IO latency min/avg/max
	if(progArgs.getShowLatency() && latHisto.getNumStoredValues() )
	{
		// individual results header (note: keep format in sync with general table format string)
		outStream << boost::format(PhaseResultsPrinter::phaseResultsLeftFormatStr)
			% ""
			% (latTypeStr + " latency")
			% ":";

		outStream <<
			"[ " <<
			"min=" << UnitTk::latencyUsToHumanStr(latHisto.getMinMicroSecLat() ) << " "
			"avg=" << UnitTk::latencyUsToHumanStr(latHisto.getAverageMicroSec() ) << " "
			"max=" << UnitTk::latencyUsToHumanStr(latHisto.getMaxMicroSecLat() ) <<
			" ]" << std::endl;
	}

	// print IO latency percentiles
	if(progArgs.getShowLatencyPercentiles() && latHisto.getNumStoredValues() )
	{
		// individual results header (note: keep format in sync with general table format string)
		outStream << boost::format(PhaseResultsPrinter::phaseResultsLeftFormatStr)
			% ""
			% (latTypeStr + " lat % us")
			% ":";

		outStream << "[ ";

		outStream <<
			"1%<=" << latHisto.getPercentileStr(1) << " "
			"50%<=" << latHisto.getPercentileStr(50) << " "
			"75%<=" << latHisto.getPercentileStr(75) << " "
			"99%<=" << latHisto.getPercentileStr(99);

		// print configurable amount of percentile nines (99.9%, 99.99%, ...)
		std::string ninesStr = "99.";
		for(unsigned short numDecimals=1;
			numDecimals <= progArgs.getNumLatencyPercentile9s();
			numDecimals++)
		{
			ninesStr += "9"; // append next decimal 9
			double percentage = std::stod(ninesStr);

			outStream << " " << std::setprecision(numDecimals+3) << // +3 for "99."
				percentage << "%<=" << latHisto.getPercentileStr(percentage);
		}

		outStream << " ]" << std::endl;
	}

	// print IO latency histogram
	if(progArgs.getShowLatencyHistogram() && latHisto.getNumStoredValues() )
	{
		// individual results header (note: keep format in sync with general table format string)
		outStream << boost::format(PhaseResultsPrinter::phaseResultsLeftFormatStr)
			% ""
			% (latTypeStr + " lat hist")
			% ":";

		outStream << "[ ";

		outStream << latHisto.getHistogramStr();

		outStream << " ]" << std::endl;
	}

    // print IO latency histogram summary
    if(progArgs.getShowLatencyHistogramGrouped() && latHisto.getNumStoredValues() )
    {
        // individual results header (note: keep format in sync with general table format string)
        outStream << boost::format(PhaseResultsPrinter::phaseResultsLeftFormatStr)
            % ""
            % (latTypeStr + " lat grpd")
            % ":";

        outStream << "[ ";

        // continuation lines align under this same "[ " column (computed, not hardcoded, so it
        // can't drift out of sync if phaseResultsLeftFormatStr ever changes)
        size_t continuationIndent = (boost::format(PhaseResultsPrinter::phaseResultsLeftFormatStr)
            % "" % "" % "").str().length() + 2; // +2 for the "[ " printed above

        outStream << latHisto.getHistogramGroupedStr(continuationIndent);

        outStream << " ]" << std::endl;
    }
}

/**
 * Print latency results to StringVec, e.g. for the StringVec to be turned into CSV.
 *
 * This can be called with all latHisto set to 0 if caller is only interested in outLabelsVec.
 *
 * @latHisto the latency histogram for which to print the results.
 * @latTypeStr short latency type description, e.g. for entries or iops.
 */
void PhaseResultsPrinter::printPhaseResultsLatencyToStringVec(const LatencyHistogram& latHisto,
	std::string latTypeStr, StringVec& outLabelsVec, StringVec& outResultsVec)
{
	// latency min/avg/max

	outLabelsVec.push_back(latTypeStr + " lat us [min]");
	outResultsVec.push_back(!latHisto.getNumStoredValues() ?
		"" : std::to_string(latHisto.getMinMicroSecLat() ) );

	outLabelsVec.push_back(latTypeStr + " lat us [avg]");
	outResultsVec.push_back(!latHisto.getNumStoredValues() ?
		"" : std::to_string(latHisto.getAverageMicroSec() ) );

	outLabelsVec.push_back(latTypeStr + " lat us [max]");
	outResultsVec.push_back(!latHisto.getNumStoredValues() ?
		"" : std::to_string(latHisto.getMaxMicroSecLat() ) );
}

void PhaseResultsPrinter::printPhaseResultsAsJSON(const PhaseResults& phaseResults)
{
    bpt::ptree ptree;

    bpt::ptree configSubtree;

    bpt::ptree firstDoneSubtree;
    bpt::ptree firstDoneSubtreeReadMix;
    bpt::ptree lastDoneSubtree;
    bpt::ptree lastDoneSubtreeReadMix;

    bpt::ptree lastDoneLatencySubtree;
    bpt::ptree entriesLatencySubtree;
    bpt::ptree entriesLatencySubtreeReadMix;
    bpt::ptree iopsLatencySubtree;
    bpt::ptree iopsLatencySubtreeReadMix;

    std::string phaseName =
        TranslatorTk::benchPhaseToPhaseName(workersSharedData.currentBenchPhase, &progArgs);

    // phase name

    ptree.put("phase_type", phaseName);

    // bench ID

    ptree.put("phase_id", workersSharedData.currentBenchID);

    // label

    if(!progArgs.getBenchLabel().empty() )
        ptree.put("label", progArgs.getBenchLabel() );

    // start time

    time_t time = std::chrono::system_clock::to_time_t(workersSharedData.phaseStartLocalT);
    auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        workersSharedData.phaseStartLocalT.time_since_epoch()).count() % 1000;

    struct tm localTimeInfo;
    localtime_r(&time, &localTimeInfo);

    std::stringstream dateStream;
    dateStream << std::put_time(&localTimeInfo, "%FT%T") << "."
        << std::setfill('0') << std::setw(3) << milliseconds
        << std::put_time(&localTimeInfo, "%z");

    ptree.put("iso_start_date", dateStream.str() );

    // config subtree...

    configSubtree.put("path_type", TranslatorTk::benchPathTypeToStr(
        progArgs.getBenchPathType(), &progArgs) );

    configSubtree.put("paths", progArgs.getBenchPaths().size() );

    configSubtree.put("hosts", progArgs.getHostsVec().empty() ?
        1 :  progArgs.getHostsVec().size() );

    configSubtree.put("threads", progArgs.getNumThreads() );

    if(progArgs.getBenchPathType() == BenchPathType_DIR)
    {
        if(progArgs.getTreeFilePath().empty() )
        {
            configSubtree.put("dirs", progArgs.getNumDirs() );
            configSubtree.put("files", progArgs.getNumFiles() );
        }
        else
        { // custom tree
            configSubtree.put("custom_tree_dirs",
                progArgs.getCustomTreeDirs().getNumPaths() );
            configSubtree.put("custom_tree_files_shared",
                progArgs.getCustomTreeFilesShared().getNumPaths() );
            configSubtree.put("files_not_shared",
                progArgs.getCustomTreeFilesNonShared().getNumPaths() );
        }
    }

    configSubtree.put("file_size", progArgs.getFileSize() );
    configSubtree.put("block_size", progArgs.getBlockSize() );
    configSubtree.put("direct_io", progArgs.getUseDirectIO() );
    configSubtree.put("random_offsets", progArgs.getUseRandomOffsets() );

    if(progArgs.getUseRandomOffsets() )
        configSubtree.put("random_aligned", !progArgs.getUseRandomUnaligned() );

    configSubtree.put("io_depth", progArgs.getIODepth() );

    if(!progArgs.getHostsVec().empty() )
        configSubtree.put("shared_service_paths", progArgs.getIsServicePathShared() );

    if( (progArgs.getBenchMode() != BenchMode_S3) &&
        (progArgs.getBenchPathType() != BenchPathType_BLOCKDEV) )
        configSubtree.put("truncate_files", progArgs.getDoTruncate() );

    // elbencho version

    configSubtree.put("version", EXE_VERSION);

    // command line

    std::ostringstream cmdStream;
    std::string cmdString;

    for(int i=0; i < progArgs.getProgArgCount(); i++)
    {
        if(!strcmp(progArgs.getProgArgVec()[i], "--" ARG_S3ACCESSSECRET_LONG) )
        { // skip over s3 secret
            i += 1;
            continue;
        }

        cmdStream << "\"" << progArgs.getProgArgVec()[i] << "\" ";
    }

    cmdString = cmdStream.str();
    std::replace(cmdString.begin(), cmdString.end(), ',', ' '); // replace all commas with spaces

    configSubtree.put("command", cmdString);


    // elasped time (first done & last done)

    firstDoneSubtree.put("elapsed_time_ms", phaseResults.firstFinishUSec / 1000);
    lastDoneSubtree.put("elapsed_time_ms", phaseResults.lastFinishUSec / 1000);

    // lambda to fill first done & last done subtrees with per-second values
    std::function addPerSecResultsToSubtree = [](const LiveOps& opsTotal,
        const LiveOps& inOpsPerSec, bpt::ptree& outTree)
    {
        // entries per second

        if(opsTotal.numEntriesDone)
            outTree.put("entries/s", inOpsPerSec.numEntriesDone);

        // IOPS

        if(opsTotal.numIOPSDone)
            outTree.put("iops", inOpsPerSec.numIOPSDone);

        // Bytes/s

        if(opsTotal.numBytesDone)
            outTree.put("bytes/s", inOpsPerSec.numBytesDone);

    }; // end of addPerSecResultsToSubtree lambda

    // lambda to fill first done & last done subtrees with absolute/total values
    std::function addTotalResultsToSubtree = [](const LiveOps& opsTotal,
        const LiveOps& inOpsTotal, bpt::ptree& outTree)
    {
        // entries

        if(opsTotal.numEntriesDone)
            outTree.put("entries", inOpsTotal.numEntriesDone);

        // Bytes transferred

        if(opsTotal.numBytesDone)
            outTree.put("bytes", inOpsTotal.numBytesDone);

    }; // end of addTotalResultsToSubtree lambda

    // first done subtree per-sec counters

    addPerSecResultsToSubtree(phaseResults.opsTotal,
        phaseResults.opsStoneWallPerSec, firstDoneSubtree);
    addPerSecResultsToSubtree(phaseResults.opsTotalReadMix,
        phaseResults.opsStoneWallPerSecReadMix, firstDoneSubtreeReadMix);

    // first done subtree total counters

    addTotalResultsToSubtree(phaseResults.opsTotal,
        phaseResults.opsStoneWallTotal, firstDoneSubtree);
    addTotalResultsToSubtree(phaseResults.opsTotalReadMix,
        phaseResults.opsStoneWallTotalReadMix, firstDoneSubtreeReadMix);

    // last done subtree per-sec counters

    addPerSecResultsToSubtree(phaseResults.opsTotal,
        phaseResults.opsPerSec, lastDoneSubtree);
    addPerSecResultsToSubtree(phaseResults.opsTotalReadMix,
        phaseResults.opsPerSecReadMix, lastDoneSubtreeReadMix);

    // last done subtree total counters

    addTotalResultsToSubtree(phaseResults.opsTotal,
        phaseResults.opsTotal, lastDoneSubtree);
    addTotalResultsToSubtree(phaseResults.opsTotalReadMix,
        phaseResults.opsTotalReadMix, lastDoneSubtreeReadMix);

    // copy rwmix read subtrees into first & last done subtrees

    if(firstDoneSubtreeReadMix.size() )
        firstDoneSubtree.put_child("rwmix_read", firstDoneSubtreeReadMix);

    if(lastDoneSubtreeReadMix.size() )
        lastDoneSubtree.put_child("rwmix_read", lastDoneSubtreeReadMix);

    // cpu utilization

    firstDoneSubtree.put("cpu%", (unsigned)phaseResults.cpuUtilStoneWallPercent);
    lastDoneSubtree.put("cpu%", (unsigned)phaseResults.cpuUtilPercent);

    // per-service min/avg/max (by own completion time, so only under last done)

    if(progArgs.getShowServicesMinMax() && !progArgs.getHostsVec().empty() )
    {
        auto addServicesMinMax = [](const ServiceMinMax& iops, const ServiceMinMax& bytesPerSec,
            bool haveIOPS, bool haveBytes, const char* childName, bpt::ptree& outTree)
        {
            if(iops.minHost.empty() )
                return;

            bpt::ptree subtree;

            auto addRate = [&subtree](const char* name, const ServiceMinMax& minMax)
            {
                bpt::ptree rateTree;
                rateTree.put("min", minMax.minPerSec);
                rateTree.put("min_host", minMax.minHost);
                rateTree.put("avg", minMax.avgPerSec);
                rateTree.put("max", minMax.maxPerSec);
                rateTree.put("max_host", minMax.maxHost);

                subtree.put_child(name, rateTree);
            };

            if(haveIOPS)
                addRate("iops", iops);

            if(haveBytes)
                addRate("bytes/s", bytesPerSec);

            if(subtree.size() )
                outTree.put_child(childName, subtree);
        };

        const bool haveIOPS = phaseResults.opsTotal.numIOPSDone ||
            phaseResults.opsTotalReadMix.numIOPSDone;
        const bool haveBytes = phaseResults.opsTotal.numBytesDone ||
            phaseResults.opsTotalReadMix.numBytesDone;
        const ResultTerms terms = ResultTerms::forPhase(progArgs, haveBytes);

        addServicesMinMax(phaseResults.svcIOPS, phaseResults.svcBytesPerSec, haveIOPS, haveBytes,
            terms.servicesKey.c_str(), lastDoneSubtree);
        addServicesMinMax(phaseResults.svcServersIOPS, phaseResults.svcServersBytesPerSec,
            haveIOPS, haveBytes, "servers", lastDoneSubtree);
    }

    // entries & iops latency results

    // lambda to fill latency
    std::function addLatencyResultsToSubtree = [&progArgs = progArgs](
        const LatencyHistogram& latHisto, bpt::ptree& outTree)
    {
        if(!progArgs.getShowLatency() || !latHisto.getNumStoredValues() )
            return;

        outTree.put("min_us", latHisto.getMinMicroSecLat() );
        outTree.put("avg_us", latHisto.getAverageMicroSec() );
        outTree.put("max_us", latHisto.getMaxMicroSecLat() );
    }; // end of addLatencyResultsToSubtree lambda

    addLatencyResultsToSubtree(phaseResults.entriesLatHisto, entriesLatencySubtree);
    addLatencyResultsToSubtree(phaseResults.entriesLatHistoReadMix, entriesLatencySubtreeReadMix);
    addLatencyResultsToSubtree(phaseResults.iopsLatHisto, iopsLatencySubtree);
    addLatencyResultsToSubtree(phaseResults.iopsLatHistoReadMix, iopsLatencySubtreeReadMix);

    // copy latency subtrees into main tree

    if(entriesLatencySubtreeReadMix.size() )
        entriesLatencySubtree.put_child("rwmix_read", entriesLatencySubtreeReadMix);

    if(iopsLatencySubtreeReadMix.size() )
        iopsLatencySubtree.put_child("rwmix_read", iopsLatencySubtreeReadMix);

    if(entriesLatencySubtree.size() )
        lastDoneLatencySubtree.put_child("entries", entriesLatencySubtree);

    if(iopsLatencySubtree.size() )
        lastDoneLatencySubtree.put_child("IO", iopsLatencySubtree);

    // latency histograms

    if(progArgs.getShowLatencyHistogram() || progArgs.getShowLatencyHistogramGrouped() )
    {
        if(phaseResults.entriesLatHisto.getNumStoredValues() )
            phaseResults.entriesLatHisto.getAsPropertyTreeForJSONFile(lastDoneLatencySubtree,
                "entries.histogram");

        if(phaseResults.entriesLatHistoReadMix.getNumStoredValues() )
            phaseResults.entriesLatHistoReadMix.getAsPropertyTreeForJSONFile(lastDoneLatencySubtree,
                "entries.histogram.rwmix_read");

        if(phaseResults.iopsLatHisto.getNumStoredValues() )
            phaseResults.iopsLatHisto.getAsPropertyTreeForJSONFile(lastDoneLatencySubtree,
                "IO.histogram");

        if(phaseResults.iopsLatHistoReadMix.getNumStoredValues() )
            phaseResults.iopsLatHistoReadMix.getAsPropertyTreeForJSONFile(lastDoneLatencySubtree,
                "IO.histogram.rwmix_read");
    }

    if(lastDoneLatencySubtree.size() )
        lastDoneSubtree.put_child("latency", lastDoneLatencySubtree);


    // copy first level subtrees into main tree

    ptree.put_child("config", configSubtree);
    ptree.put_child("first_done", firstDoneSubtree);
    ptree.put_child("last_done", lastDoneSubtree);


    // print json

    try
    {
        std::ofstream fileStream;

        fileStream.open(progArgs.getResFilePathJSON(), std::ofstream::app);

        if(!fileStream)
        {
            std::cerr << "ERROR: Opening results JSON file failed: " << progArgs.getResFilePathJSON() <<
                std::endl;

            return;
        }

        bpt::json_parser::write_json(fileStream, ptree, false); // throws on error

		fileStream << std::endl;
    }
    catch(const std::exception& e)
    {
        throw ProgException("Error writing JSON result file. "
            "File: " + progArgs.getResFilePathJSON() + "; " +
            "Error: " + e.what() );
    }
}
