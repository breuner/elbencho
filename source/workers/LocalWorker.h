#ifndef WORKERS_LOCALWORKER_H_
#define WORKERS_LOCALWORKER_H_

#include <ctype.h>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "CuFileHandleData.h"
#include "toolkits/net/BasicSocket.h"
#include "toolkits/offsetgen/OffsetGenerator.h"
#include "toolkits/offsetgen/OffsetGenRandomAlignedFullCoverageV2.h"
#include "toolkits/OpsLogger.h"
#include "toolkits/random/RandAlgoInterface.h"
#include "toolkits/RateLimiter.h"
#include "toolkits/RateLimiterRWMixThreads.h"
#include "toolkits/S3Tk.h"
#include "S3UploadStore.h"
#include "Worker.h"

#ifdef CUDA_SUPPORT
	#include <cuda_runtime.h>
	#include <curand.h>
#endif

#ifdef LIBAIO_SUPPORT
	#include <libaio.h>
#endif

#ifdef HDFS_SUPPORT
	#include <hdfs.h>
#endif

typedef std::vector<BasicSocket*> SocketVec;

// delaration for function typedefs below
class LocalWorker;

// io_prep_pwrite or io_prep_read from libaio
typedef void (LocalWorker::*AIO_RW_PREPPER)(struct iocb* iocb, int fd, void* buf, size_t count,
	long long offset);

/**
 * {pread,pwrite}Wrapper (as in "man 2 pread") or cuFile{Read,Write}Wrapper.
 *
 * @fileHandleIdx is index of LocalWorker::fileHandles.*VecPtr.
 */
typedef ssize_t (LocalWorker::*POSITIONAL_RW)(size_t fileHandleIdx, void* buf, size_t nbytes,
	off_t offset);

// function pointer for GPU memcpy before write or after read
typedef void (LocalWorker::*GPU_MEMCPY_RW)(void* hostIOBuf, void* gpuIOBuf, size_t count);

// function pointer for sync or async IO
typedef int64_t (LocalWorker::*RW_BLOCKSIZED)();

// function pointer for cuFile handle register
typedef void (LocalWorker::*CUFILE_HANDLE_REGISTER)(int fd, CuFileHandleData& handleData);

// function pointer for cuFile handle deregister
typedef void (LocalWorker::*CUFILE_HANDLE_DEREGISTER)(CuFileHandleData& handleData);

// preWriteIntegrityCheckFillBuf/postReadIntegrityCheckVerifyBuf
typedef void (LocalWorker::*BLOCK_MODIFIER)(char* hostIOBuf, char* gpuIOBuf, size_t bufLen,
	off_t fileOffset);

// preRWRateLimiter
typedef void (LocalWorker::*RW_RATE_LIMITER)(size_t rwSize,
    std::atomic_bool& isInterruptionRequested);


enum class CorsResult 
{
	NOT_SET,
	OK,
	MISSING,
	MISMATCH,	
};

/**
 * Each worker represents a single thread performing local I/O.
 */
class LocalWorker : public Worker
{
	public:
		explicit LocalWorker(WorkersSharedData* workersSharedData, size_t workerRank);
		~LocalWorker();

		virtual void cleanupAfterPhaseDone() override;

	protected:
		virtual void run() override;
		virtual void cleanup() override;

	private:
		BufferVec ioBufVec; // host buffers used for block-sized read/write (count matches iodepth)

		BufferVec gpuIOBufVec; // gpu memory buffers for read/write via cuda (count matches iodepth)

		struct
		{
			IntVec threadFDVec; // separate open files in file/bdev mode (progArgs::useNoFDSharing)
			CuFileHandleDataVec threadCuFileHandleDataVec; /* separate cuFile handles
															(progArgs::useNoFDSharing) */

			IntVec fdVec; // fd of current file in dir mode
			const IntVec* fdVecPtr{NULL}; /* for funcPositionalRW; fdVec in dir mode,
				progArgs fdVec in file/bdev mode */
			ssize_t errorFDVecIdx{-1}; // set by low-level funcs to "!=-1" where no path available

			CuFileHandleDataVec cuFileHandleDataVec; // cuFile handle for current file in dir mode
			CuFileHandleDataPtrVec cuFileHandleDataPtrVec; /* for funcPositionalRW; number of
				elements corresponds to num elems in fdVecPtr target */

			BufferVec mmapVec; /* pointers to mmap regions if user selected mmap IO; number of
				entries and their order matches fdVec */
		} fileHandles;

		RateLimiter rateLimiter; // for r/w rate limit per sec if set by user
		RateLimiterRWMixThreads rateLimiterRWMixThreads; // for r/w threads rate balance if set

		uint64_t numIOPSSubmitted{0}; // internal sequential counter, not reset between phases

		// phase-dependent variables
		BenchPhase benchPhase{BenchPhase_IDLE};

		// phase-dependent function & offsetGen pointers
		RW_BLOCKSIZED funcRWBlockSized; // pointer to sync or async read/write
		POSITIONAL_RW funcPositionalWrite; // pwrite, cuFileWrite for sync write
		POSITIONAL_RW funcPositionalRead; // pread, cuFileRead for sync read
		AIO_RW_PREPPER funcAioRwPrepper; // io_prep_pwrite/io_prep_read for async read/write
		BLOCK_MODIFIER funcPreWriteBlockModifier; // mod block buf before write (e.g. integrity chk)
		BLOCK_MODIFIER funcPostReadBlockChecker; // check block buf post read (e.g. integrity check)
		GPU_MEMCPY_RW funcPreWriteCudaMemcpy; // copy from GPU memory
		GPU_MEMCPY_RW funcPostReadCudaMemcpy; // copy to GPU memory
		CUFILE_HANDLE_REGISTER funcCuFileHandleReg; // cuFile handle register
		CUFILE_HANDLE_DEREGISTER funcCuFileHandleDereg; // cuFile handle deregister
		RW_RATE_LIMITER funcRWRateLimiter; // limit per-thread read or write throughput
		std::unique_ptr<OffsetGenerator> rwOffsetGen; // r/w offset gen for phase-dependent funcs
		std::unique_ptr<RandAlgoInterface> randOffsetAlgo; // for random offsets
		std::unique_ptr<RandAlgoInterface> randBlockVarAlgo; // for random block contents variance
		std::unique_ptr<RandAlgoInterface> randBlockVarReseed; // reseed for golden prime block var

		PathStore customTreeDirs; // non-shared dirs for custom tree mode
		PathStore customTreeFiles; // non-shared and shared files for custom tree mode

#ifdef CUDA_SUPPORT
		int gpuID{-1}; // GPU ID for this worker, initialized in allocGPUIOBuffer
		curandGenerator_t gpuRandGen{NULL};
#endif

#ifdef S3_SUPPORT
        std::shared_ptr<S3Client> s3Client; // (shared_ptr expected by some SDK functions)
        std::string s3EndpointStr; // set after s3Client initialized
        static S3UploadStore s3SharedUploadStore; // singleton for shared uploads

        bool useS3SSE{false}; // for plain server-side encryption
        std::string s3SSECKey; // SSE-C encryption key
        std::string s3SSECKeyMD5; // SSE-C encryption key MD5 hash
        std::string s3SSEKMSKey; // SSE-KMS encryption key
		S3ChecksumAlgorithm s3ChecksumAlgorithm; // for x-amz-sdk-checksum-algorithm header

        // Stores the CorsResult for the latest response, assuming it ran with s3ModeAddCorsHeader
        CorsResult lastCorsResult{CorsResult::NOT_SET};
        // Stores the last value of the 'access-control-allow-origin' header (only if there's a mismatch)
        Aws::String lastMismatchedOriginHeader;
#endif

#ifdef HDFS_SUPPORT
		hdfsFS hdfsFSHandle{NULL}; // currently referenced hdfs instance
		hdfsFile hdfsFileHandle{NULL}; // currently open file on hdfs
#endif

#ifdef LIBAIO_SUPPORT
        struct
        { // init in initLibAio()
            io_context_t ioContext = {};
            std::vector<struct iocb> iocbVec;
            std::vector<struct iocb*> iocbPointerVec;
            std::vector<std::chrono::steady_clock::time_point> ioStartTimeVec;
        } libaioContext;
#endif // LIBAIO_SUPPORT

		static SocketVec serverSocketVec; // singleton netbench server sockets for all local threads
		BasicSocket* clientSocket{NULL}; // netbench socket for client

		OpsLogger opsLog; // logger for IO operations


		static void bufFill(char* buf, uint64_t fillValue, size_t bufLen);

		void preparePhase();
		void finishPhase();

        void initLibAio();
        void uninitLibAio();
		void initS3Client();
		void uninitS3Client();
		void initHDFS();
		void uninitHDFS();
		void initNetBench();
		void initNetBenchServer();
		void initNetBenchClient();
		void uninitNetBench();
		void uninitNetBenchAfterPhaseDone();
		void initThreadFDVec();
		void uninitThreadFDVec();
		void initThreadCuFileHandleDataVec();
		void uninitThreadCuFileHandleDataVec();
		void initThreadMmapVec();
		void uninitThreadMmapVec();
		void initThreadPhaseVars();
		void initPhaseFileHandleVecs();
		void initPhaseRWOffsetGen();
		void nullifyPhaseFunctionPointers();
		void initPhaseFunctionPointers();

		void allocIOBuffer();
		void allocGPUIOBuffer();
		void prepareCustomTreePathStores();

		int64_t rwBlockSized();
		int64_t aioBlockSized();
		void calcFileIdxAndOffsetStriped(const uint64_t rwOffsetGenNext,
		    const uint64_t fileSize, const bool isSingleFile,
		    size_t& outFileIdx, uint64_t& outFileOffset);

		void dirModeIterateDirs();
		void dirModeIterateCustomDirs();
		void dirModeIterateFiles();
		void dirModeIterateCustomFiles();

		void fileModeIterateFilesRand();
		void fileModeIterateFilesSeq();
		void fileModeDeleteFiles();
		std::string fileModeLogPathFromFileHandlesErr();

		void s3ModeIterateBuckets();
		void s3ModeIterateObjects();
		void s3ModeIterateObjectsRand();
		void s3ModeIterateCustomObjects();

#ifdef S3_SUPPORT
        template <typename R>
        void s3ModeThrowOnError(const Aws::Utils::Outcome<R, S3ErrorType>& outcome, const std::string& failMessage,
                                const std::string& bucketName, const std::string& objectName="");
        template <typename R>
        void s3ModeThrowOnCorsError(const Aws::Utils::Outcome<R, S3ErrorType>& outcome,
                                   const std::string& bucketName, const std::string& objectName="");
#endif // S3_SUPPORT

        template <typename REQUESTTYPE>
            void s3ModeAddServerSideEncryption(REQUESTTYPE& request);
        template <typename REQUESTTYPE>
        void s3ModeAddCorsHeader(REQUESTTYPE& request);
        template <typename REQUESTTYPE>
			inline void s3ModeAddChecksumAlgorithm(REQUESTTYPE& request);
		void s3ModeCreateBucket(std::string bucketName);
		void s3ModeHeadBucket(std::string bucketName);
		void s3ModeCreateBucketTagging(const std::string& bucketName);
		void s3ModeDeleteBucketTagging(const std::string& bucketName);
		void s3ModeGetBucketTagging(const std::string& bucketName);
		void s3ModeDeleteBucket(const std::string& bucketName);
		void s3ModePutBucketAcl(std::string bucketName);
		void s3ModeGetBucketAcl(std::string bucketName);
        void s3ModeGetBucketVersioning(const std::string& bucketName);
        void s3ModePutBucketVersioning(const std::string& bucketName, bool enable = true);
		void s3ModeUploadObjectSinglePart(std::string bucketName, std::string objectName);
		void s3ModeUploadObjectMultiPart(std::string bucketName, std::string objectName);
		void s3ModeUploadObjectMultiPartShared(std::string bucketName, std::string objectName,
			uint64_t objectTotalSize);
		bool s3AbortMultipartUpload(std::string bucketName, std::string objectName,
			std::string uploadID);
		void s3ModeAbortUnfinishedSharedUploads();
		void s3ModeDownloadObject(std::string bucketName, std::string objectName,
			const bool isRWMixedReader);
		void s3ModeStatObject(std::string bucketName, std::string objectName);
		void s3ModeDeleteObject(std::string bucketName, std::string objectName);
		void s3ModeListObjects();
		void s3ModeListObjParallel();
		void s3ModeVerifyListing(StringSet& expectedSet, StringList& receivedList,
			std::string bucketName, std::string listPrefix);
		void s3ModeListAndMultiDeleteObjects();
		void s3ModePutObjectAcl(std::string bucketName, std::string objectName);
		void s3ModeGetObjectAcl(std::string bucketName, std::string objectName);
        void s3ModeGetObjectTags(const std::string& bucketName, const std::string& objectName);
        void s3ModePutObjectTags(const std::string& bucketName, const std::string& objectName);
        void s3ModeDeleteObjectTags(const std::string& bucketName, const std::string& objectName);
        void s3ModeGetObjectLockConfiguration(const std::string& bucketName);
        void s3ModePutObjectLockConfiguration(const std::string& bucketName, bool unset = false);
		bool getS3ModeDoReverseSeqFallback();
		std::string getS3RandObjectPrefix(size_t workerRank, size_t dirIdx, size_t fileIdx,
			const std::string& objectPrefix);

		void hdfsDirModeIterateDirs();
		void hdfsDirModeIterateFiles();

		void netbenchDoTransfer();
		void netbenchDoTransferServer();
		void netbenchDoTransferClient();

		void anyModeSync();
		void anyModeDropCaches();

		int getDirModeOpenFlags(BenchPhase benchPhase);
		int dirModeOpenAndPrepFile(BenchPhase benchPhase, const IntVec& pathFDs,
			unsigned pathFDsIndex, const char* relativePath, int openFlags, uint64_t fileSize);

		// for phase function pointers...

		void noOpCudaMemcpy(void* hostIOBuf, void* gpuIOBuf, size_t count);
		void cudaMemcpyGPUToHost(void* hostIOBuf, void* gpuIOBuf, size_t count);
		void cudaMemcpyHostToGPU(void* hostIOBuf, void* gpuIOBuf, size_t count);

		void noOpCuFileHandleReg(int fd, CuFileHandleData& handleData);
		void noOpCuFileHandleDereg(CuFileHandleData& handleData);
		void dirModeCuFileHandleReg(int fd, CuFileHandleData& handleData);
		void dirModeCuFileHandleDereg(CuFileHandleData& handleData);

		ssize_t preadWrapper(size_t fileHandleIdx, void* buf, size_t nbytes, off_t offset);
		ssize_t pwriteWrapper(size_t fileHandleIdx, void* buf, size_t nbytes, off_t offset);
		ssize_t pwriteAndReadWrapper(size_t fileHandleIdx, void* buf, size_t nbytes, off_t offset);
		ssize_t pwriteRWMixWrapper(size_t fileHandleIdx, void* buf, size_t nbytes, off_t offset);
		ssize_t cuFileReadWrapper(size_t fileHandleIdx, void* buf, size_t nbytes, off_t offset);
		ssize_t cuFileWriteWrapper(size_t fileHandleIdx, void* buf, size_t nbytes, off_t offset);
		ssize_t cuFileWriteAndReadWrapper(size_t fileHandleIdx, void* buf, size_t nbytes,
			off_t offset);
		ssize_t cuFileRWMixWrapper(size_t fileHandleIdx, void* buf, size_t nbytes, off_t offset);
		ssize_t hdfsReadWrapper(size_t fileHandleIdx, void* buf, size_t nbytes, off_t offset);
		ssize_t hdfsWriteWrapper(size_t fileHandleIdx, void* buf, size_t nbytes, off_t offset);
		ssize_t mmapReadWrapper(size_t fileHandleIdx, void* buf, size_t nbytes, off_t offset);
		ssize_t mmapWriteWrapper(size_t fileHandleIdx, void* buf, size_t nbytes, off_t offset);

		void noOpIntegrityCheck(char* hostIOBuf, char* gpuIOBuf, size_t bufLen, off_t fileOffset);
		void preWriteIntegrityCheckFillBuf(char* hostIOBuf, char* gpuIOBuf, size_t bufLen,
			off_t fileOffset);
		void postReadIntegrityCheckVerifyBuf(char* hostIOBuf, char* gpuIOBuf, size_t bufLen,
			off_t fileOffset);
		void preWriteBufRandRefill(char* hostIOBuf, char* gpuIOBuf, size_t bufLen,
			off_t fileOffset);
		void preWriteBufRandRefillCuda(char* hostIOBuf, char* gpuIOBuf, size_t bufLen,
			off_t fileOffset);

		void aioWritePrepper(struct iocb* iocb, int fd, void* buf, size_t count, long long offset);
		void aioReadPrepper(struct iocb* iocb, int fd, void* buf, size_t count, long long offset);
		void aioRWMixPrepper(struct iocb* iocb, int fd, void* buf, size_t count, long long offset);

        void noOpRateLimiter(size_t rwSize,
            std::atomic_bool& isInterruptionRequested);
        void preRWRateLimiter(size_t rwSize,
            std::atomic_bool& isInterruptionRequested);
        void preRWRateBalanceLimiterForReaders(size_t rwSize,
            std::atomic_bool& isInterruptionRequested);
        void preRWRateBalanceLimiterForWriters(size_t rwSize,
            std::atomic_bool& isInterruptionRequested);
};


#endif /* WORKERS_LOCALWORKER_H_ */
