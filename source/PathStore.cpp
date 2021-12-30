#include <boost/algorithm/string.hpp>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <sstream>
#include "Logger.h"
#include "PathStore.h"
#include "ProgException.h"

#ifndef ARG_TREEROUNDUP_LONG // because we can't include ProgArgs.h here
	#define ARG_TREEROUNDUP_LONG		"treeroundup"
#endif

/**
 * Load directories from file. Lines not starting with PATHSTORE_DIR_LINE_PREFIX will be ignored.
 *
 * Line format:
 * 	PATHSTORE_DIR_LINE_PREFIX <relative_path>
 *
 * @path path to file from which directories should be loaded.
 *
 * @throw ProgException on error, such as file not exists.
 */
void PathStore::loadDirsFromFile(std::string path)
{
	std::string lineStr;
	unsigned lineNum = 0;

	std::ifstream fileStream(path.c_str() );
	if(!fileStream)
		throw ProgException("Opening input file failed: " + path);

	// process each line in input file
	for( ; std::getline(fileStream, lineStr); lineNum++)
	{
		std::istringstream lineStream(lineStr);

		// check line prefix to match PATHSTORE_DIR_LINE_PREFIX

		std::string linePrefixStr;
		lineStream >> linePrefixStr;

		if(linePrefixStr != PATHSTORE_DIR_LINE_PREFIX)
			continue;

		// get rest of line as path
		PathStoreElem newElem;

		std::getline(lineStream, newElem.path);

		boost::trim(newElem.path); // (path would otherwise at least contain leading space)

		if(newElem.path.empty() )
			throw ProgException("Encountered invalid directory line without path in input file. "
				"File: " + path + "; "
				"Line number: " + std::to_string(lineNum) );

		// add new element to list
		paths.push_back(newElem);
		numPaths++;
	}
}

/**
 * Load files from file, skip the ones that are not within given size range. Lines not starting with
 * PATHSTORE_FILE_LINE_PREFIX will be ignored.
 *
 * Line format:
 *	PATHSTORE_FILE_LINE_PREFIX <size_in_bytes> <relative_path>
 *
 * @path path to file from which files should be loaded.
 * @minFileSize skip files smaller than this size.
 * @maxFileSize skip files larger than this size.
 * @roundUpSize round up file sizes to a multiple of given size; 0 disables rounding up.
 *
 * @throw ProgException on error, such as file not exists.
 */
void PathStore::loadFilesFromFile(std::string path, uint64_t minFileSize, uint64_t maxFileSize,
	uint64_t roundUpSize)
{
	std::string lineStr;
	unsigned lineNum = 0;

	std::ifstream fileStream(path.c_str() );
	if(!fileStream)
		throw ProgException("Opening input file failed: " + path);

	// process each line in input file
	for( ; std::getline(fileStream, lineStr); lineNum++)
	{
		std::istringstream lineStream(lineStr);

		// check line prefix to match PATHSTORE_DIR_LINE_PREFIX

		std::string linePrefixStr;

		lineStream >> linePrefixStr;

		if(linePrefixStr != PATHSTORE_FILE_LINE_PREFIX)
			continue; // prefix doesn't match => skip

		// get file size
		PathStoreElem newElem;
		newElem.rangeStart = 0;

		if(!(lineStream >> newElem.totalLen) )
			throw ProgException("Encountered invalid file line without size in input file. "
				"File: " + path + "; "
				"Line number: " + std::to_string(lineNum) );

		newElem.rangeLen = newElem.totalLen; // rangeLen equals file size here at load time

		// round up file size if requested by caller
		if(roundUpSize && (newElem.totalLen % roundUpSize) )
		{
			const uint64_t moduloRes = newElem.totalLen % roundUpSize;
			newElem.totalLen = newElem.totalLen - moduloRes + roundUpSize;
			newElem.rangeLen = newElem.totalLen;
		}

		const uint64_t fileSize = newElem.totalLen;

		// check matching file size for caller's given range
		if( (fileSize < minFileSize) || (fileSize > maxFileSize) )
			continue; // file size not within range => skip

		const uint64_t numFileBlocks = (fileSize / blockSize) +
			( (fileSize % blockSize) ? 1 : 0);

		// get rest of line as path
		std::getline(lineStream, newElem.path);

		boost::trim(newElem.path); // (path would otherwise at least contain leading space)

		if(newElem.path.empty() )
			throw ProgException("Encountered invalid file line without path in input file. "
				"File: " + path + "; "
				"Line number: " + std::to_string(lineNum) );

		// add new element to list
		paths.push_back(newElem);
		numPaths++;
		numBlocksTotal += numFileBlocks;
		numBytesTotal += fileSize;
	}
}

/**
 * Sort list by length of path and alphabetical as secondary criteria. The latter is to have
 * guaranteed same order across multiple hosts. This order makes sense for directories to ensure
 * that dirs higher in the tree get created before their subdirs.
 */
void PathStore::sortByPathLen()
{
	paths.sort([](const PathStoreElem& a, const PathStoreElem& b)
		{ return (a.path.size() < b.path.size() ) ||
			( (a.path.size() == b.path.size() ) && (a.path < b.path) ); } );
}

/**
 * Sort list by file size as primary criteria and alphabetical as secondary criteria. The latter is
 * to have guaranteed same order across multiple hosts. This order makes sense because each worker
 * thread will pick each n-th element, so this helps to achieve some reasonable balance among
 * workers.
 */
void PathStore::sortByFileSize()
{
	paths.sort([](const PathStoreElem& a, const PathStoreElem& b)
		{ return (a.totalLen < b.totalLen) ||
			( (a.totalLen == b.totalLen) && (a.path < b.path) ); } );
}

/**
 * Random shuffle internal list.
 */
void PathStore::randomShuffle()
{
	std::vector<PathStoreElem> tmpPathVec; // for random shuffle

	tmpPathVec.reserve(numPaths);

	// move all elements from list over to tmp vector
	while(numPaths)
	{
		tmpPathVec.push_back(*paths.begin() );
		paths.pop_front();
		numPaths--;
	}

	// sort tmp vector
	std::random_device rd;
	std::mt19937 generator(rd() );
	std::shuffle(tmpPathVec.begin(), tmpPathVec.end(), generator);

	// move all elements back to paths list
	while(tmpPathVec.size() )
	{
		paths.push_front(tmpPathVec[tmpPathVec.size() - 1] );
		numPaths++;
		tmpPathVec.pop_back();
	}
}

/**
 * Get worker-specific list from global PathStore. Full files will be assigned to outPathStore, so
 * this is more appropriate for small files (and for directories).
 *
 * This PathStore should be ordered by file size for balance among workers, because each worker will
 * get each n-th element, where n is the number of data set workers.
 *
 * @workerRank the rank of this worker for which to get the sublist.
 * @numDataSetThreads as defined in ProgArgs.
 * @throwOnFileSmallerBlock true to throw an exception if a file size is found that is smaller than
 * 		the given block size; this is useful for random IO checks.
 * @outPathStore the store to which the result list should be added.
 *
 * @throw ProgException if throwOnFileSmallerBlock condition found.
 */
void PathStore::getWorkerSublistNonShared(unsigned workerRank, unsigned numDataSetThreads,
	bool throwOnFileSmallerBlock, PathStore& outPathStore) const
{
	if(workerRank >= numPaths)
		return; // not even a single element in this store for the given worker rank

	PathList::const_iterator pathsIter = paths.begin();

	std::advance(pathsIter, workerRank);
	size_t currentIdx=workerRank;

	while(true)
	{
		const uint64_t fileSize = pathsIter->totalLen;
		const uint64_t numFileBlocks = (fileSize / blockSize) +
				( (fileSize % blockSize) ? 1 : 0);

		if(throwOnFileSmallerBlock && (fileSize < blockSize) )
			throw ProgException("Found file that is smaller than block size. Consider using "
				"\"--" ARG_TREEROUNDUP_LONG "\". "
				"File: " + pathsIter->path + "; "
				"FileSize: " + std::to_string(fileSize) + "; "
				"BlockSize: " + std::to_string(blockSize) );

		// add to outPathStore
		outPathStore.paths.push_back(*pathsIter);
		outPathStore.numPaths++;
		outPathStore.numBlocksTotal += numFileBlocks;
		outPathStore.numBytesTotal += fileSize;

		// check if next iter advance would be behind the end of the path list, so we're done
		if( (currentIdx + numDataSetThreads) >= numPaths)
			break;

		std::advance(pathsIter, numDataSetThreads);
		currentIdx += numDataSetThreads;
	}
}

/**
 * Get worker-specific list from global PathStore. Files can be shared between multiple workers,
 * so different workers can read/write different ranges of a file. This is more appropriate for
 * large files.
 *
 * Note: This method assumes that it's called on the global list, where all path ranges start at
 * zero and range length equals file size.
 *
 * Note: This method assumes that each file has at least one block, so not zero length.
 *
 * @workerRank the rank of this worker for which to get the sublist.
 * @numDataSetThreads as defined in ProgArgs.
 * @throwOnSliceSmallerBlock true to throw an exception if a file slice is found that is smaller
 * 		than the given block size; this is useful for random IO checks.
 * @outPathStore the store to which the result list should be added.
 *
 * @throw ProgException if throwOnSliceSmallerBlock condition found.
 */
void PathStore::getWorkerSublistShared(unsigned workerRank, unsigned numDataSetThreads,
	bool throwOnSliceSmallerBlock, PathStore& outPathStore) const
{
	if(paths.empty() )
		return;

	const unsigned numThreads = numDataSetThreads; // just a shorthand
	const uint64_t standardWorkerNumBlocks = numBlocksTotal / numThreads;

	// note: last worker might need to write up to "numThreads-1" more blocks than the others
	uint64_t thisWorkerNumBlocks = standardWorkerNumBlocks;
	if( (workerRank == (numThreads-1) ) && (numBlocksTotal % numThreads) )
		thisWorkerNumBlocks = numBlocksTotal - (standardWorkerNumBlocks * (numThreads-1) );

	// indices of start and end block for this worker. (end block is not inclusive.)
	uint64_t startBlock = workerRank * standardWorkerNumBlocks;
	uint64_t endBlock = startBlock + thisWorkerNumBlocks;

	LOGGER(Log_DEBUG, "get sublist shared - workerRank: " << workerRank << "; "
		"dataSetThreads: " << numThreads << "; "
		"blocksTotal: " << numBlocksTotal << "; "
		"standardWorkerNumBlocks: " << standardWorkerNumBlocks << "; "
		"thisWorkerNumBlocks: " << thisWorkerNumBlocks << "; "
		"startBlock: " << startBlock << "; "
		"endBlock: " << endBlock << "; " << std::endl);

	if(!thisWorkerNumBlocks)
		return; // nothing to do for this worker

	PathList::const_iterator pathsIter = paths.begin();
	uint64_t currentBlockIdx = 0;
	uint64_t numBlocksLeft = thisWorkerNumBlocks; // blocks not yet assigned to this worker

	// iterate over paths to find relevant ones and set worker's ranges in outPathStore
	while( (pathsIter != paths.end() ) && (currentBlockIdx < endBlock) )
	{
		const uint64_t fileSize = pathsIter->totalLen;
		const uint64_t numFileBlocks = (fileSize / blockSize) +
				( (fileSize % blockSize) ? 1 : 0);
		uint64_t firstFileBlock = currentBlockIdx;
		uint64_t lastFileBlock = firstFileBlock + numFileBlocks - 1;

		// check if this file is still before our relevant range start
		if(lastFileBlock < startBlock)
		{
			pathsIter++;
			currentBlockIdx += numFileBlocks;
			continue;
		}

		// if we got here then some part of the current file must contain relevant range

		// find relevant file blocks
		uint64_t remainingFileBlocks;
		uint64_t rangeStart;
		uint64_t rangeLen;

		// find rangeStart for this file
		if(startBlock <= firstFileBlock)
		{ // first block of file is part of relevant range
			rangeStart = 0;
			remainingFileBlocks = numFileBlocks;
		}
		else
		{ // first block of file is not part of relevant range => calc relevant file range start
			uint64_t innerFileBlockOffset = startBlock - firstFileBlock;
			rangeStart = innerFileBlockOffset * blockSize;
			remainingFileBlocks = numFileBlocks - innerFileBlockOffset;
		}

		// find rangeLen for this file
		if(numBlocksLeft < remainingFileBlocks)
		{ // file contains the rest of our needed blocks (without last block)
			rangeLen = numBlocksLeft * blockSize;
			numBlocksLeft = 0;
		}
		else
		{ // full rest of file is part of relevant range. (last file block might be partial block.)
			rangeLen = fileSize - rangeStart;
			numBlocksLeft -= remainingFileBlocks;
		}

		// prepare path element with relevant range
		PathStoreElem pathElem = *pathsIter;
		pathElem.rangeStart = rangeStart;
		pathElem.rangeLen = rangeLen;

		if(throwOnSliceSmallerBlock && (rangeLen < blockSize) )
			throw ProgException("Found file slice that is smaller than block size. Consider using "
				"\"--" ARG_TREEROUNDUP_LONG "\". "
				"File: " + pathsIter->path + "; "
				"RangeStart: " + std::to_string(rangeStart) + "; "
				"RangeLength: " + std::to_string(rangeLen) + "; "
				"BlockSize: " + std::to_string(blockSize) );

		// add to outPathStore
		outPathStore.paths.push_back(pathElem);
		outPathStore.numPaths++;
		outPathStore.numBytesTotal += rangeLen;

		// prepare for next round
		pathsIter++;
		currentBlockIdx += numFileBlocks;
	}

	outPathStore.numBlocksTotal += thisWorkerNumBlocks;
}

/**
 * Generate a treefile line for a file.
 *
 * @param path path to file
 * @fileSize file size
 * @return generated file line including std::endl
 */
std::string PathStore::generateFileLine(std::string filePath, uint64_t fileSize)
{
	std::stringstream stream;

	stream << PATHSTORE_FILE_LINE_PREFIX << " " << fileSize << " " << filePath << std::endl;

	return stream.str();
}
