#ifndef PATHSTORE_H_
#define PATHSTORE_H_

#include <list>
#include <string>
#include "Common.h"
#include "ProgException.h"

/**
 * Elements of PathStore.
 */
struct PathStoreElem
{
	std::string path; // relative path to file or dir
	uint64_t rangeStart{0}; // offset for read or write in case this path refers to a file
	uint64_t rangeLen{0}; // length to read or write in case this path refers to a file
};

typedef std::list<PathStoreElem> PathList;

/**
 * Stores a list of paths. Typically used to store either a list of files or dirs to process by
 * the workers.
 *
 * Block size must be set before adding any paths to this store.
 *
 * (This class doesn't know whether the contained paths represent files or dirs.)
 */
class PathStore
{
	public:
		void loadDirsFromFile(std::string path);
		void loadFilesFromFile(std::string path, uint64_t minFileSize, uint64_t maxFileSize,
			uint64_t roundUpSize);

		void sortByPathLen();
		void sortByFileSize();
		void randomShuffle();

		void getWorkerSublistNonShared(unsigned workerRank, unsigned numDataSetThreads,
			PathStore& outPathStore) const;
		void getWorkerSublistShared(unsigned workerRank, unsigned numDataSetThreads,
			PathStore& outPathStore) const;

	private:
		uint64_t blockSize{0}; // progArgs blockSize
		uint64_t numBlocksTotal{0}; // sum of blocks in all files (if file size >0)
		uint64_t numBytesTotal{0}; // sum of bytes in all files
		size_t numPaths{0}; /* only exists because C++14's std::list.size() with devtoolset-9 on
				CentOS 7 counts elements each time instead of having a separate counter for O(1) */
		PathList paths;

		// inliners
	public:
		uint64_t getNumBlocksTotal() const { return numBlocksTotal; }
		uint64_t getNumBytesTotal() const { return numBytesTotal; }
		const PathList& getPaths() const { return paths; }
		size_t getNumPaths() const { return numPaths; }

		/**
		 * @blockSize blockSize from ProgArgs
		 */
		void setBlockSize(uint64_t blockSize)
		{
			if(!paths.empty() )
				throw ProgException("PathStore block size setter called on non-empty store.");

			this->blockSize = blockSize;
		}

		void clear()
		{
			blockSize = 0;
			numBlocksTotal = 0;
			numBytesTotal = 0;
			paths.clear();
			numPaths = 0;
		}

};

#endif /* PATHSTORE_H_ */
