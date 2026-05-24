// SPDX-FileCopyrightText: 2020-2025 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifdef S3_SUPPORT

#include <memory>

#include "Logger.h"
#include "PathStore.h"
#include "ProgArgs.h"
#include "S3UploadStore.h"
#include "toolkits/OpsLogger.h"
#include "toolkits/TranslatorTk.h"
#include "workers/WorkerException.h"

/**
 * Initialize progArgs pointer. This has to be done before doing an S3 request.
 *
 * This also loads the mpu sharing file (if called by local worker rank 0) and uses the custom tree
 * if service mpu sharing is set in progArgs.
 */
void S3UploadStore::setProgArgs(const ProgArgs* progArgs, size_t workerRank)
{
    // note: no mutex needed here because each worker will set to same progArgs in prep phase
    this->progArgs = progArgs;

    // only first local worker needs to populate the mpu store
    size_t workerRankLocal = workerRank - progArgs->getRankOffset();
	if(progArgs->getUseS3MPUSharing() && !workerRankLocal)
		loadSvcMPUSharingFile();
}

/**
 * Load services MPU sharing file and progArgs treefile (which contains all objects and is not
 * randomized) to populate internal MPU IDs.
 *
 * @throw WorkerException on error, e.g. failed server communication.
 */
 void S3UploadStore::loadSvcMPUSharingFile()
{
    const PathStore& pathStore = progArgs->getCustomTreeFilesShared();
    const PathList& pathList = pathStore.getPaths();
    const std::string bucketName = progArgs->getBenchPaths()[0];
    const std::string objectPrefix = progArgs->getS3ObjectPrefix();
	const unsigned short servicePort = progArgs->getServicePort();

    // (sanity check; should never happen)
    if(pathList.empty() )
        throw WorkerException("Found empty shared paths list in MPU sharing mode.");

    std::ifstream file(SERVICE_UPLOAD_BASEPATH(servicePort) + "/" + SERVICE_UPLOAD_MPUSHARINGFILE);
    if(!file.is_open() )
        throw WorkerException(std::string("Opening MPU IDs file failed: ") +
            SERVICE_UPLOAD_MPUSHARINGFILE);

    // load shared MPU IDs from line (newline-separated)...

    PathListCIter pathListIter = pathList.begin();
    std::string fileLineStr;

    while(std::getline(file, fileLineStr) )
    {
        // ignore empty lines
        if(fileLineStr.empty() )
            continue;

        // path list length must match mpu ids list length
        if(pathListIter == pathList.end() )
            throw WorkerException("Shared MPU IDs list is longer than treefile list");

        // insert new elem with its uploadID...

        std::string objectName = objectPrefix + pathListIter->path;
        S3UploadKey key(bucketName, objectName);

        S3UploadElem& elem = map[key];

        elem.uploadID = fileLineStr;
        elem.completedParts = std::make_unique<Aws::Vector<S3::CompletedPart>>();

		pathListIter++;
    }

    // path list length must match mpu ids list length
    if(pathListIter != pathList.end() )
        throw WorkerException("Treefile list longer than shared MPU IDs list");
}

/**
 * Return existing multipart uploadID (if it was previously created by another worker) or
 * get a new uploadID from S3 server.
 *
 * @throw WorkerException on error, e.g. failed server communication.
 */
std::string S3UploadStore::getMultipartUploadID(const std::string& bucketName,
	const std::string& objectName, std::shared_ptr<S3Client> s3Client, OpsLogger& opsLog)
{
	std::unique_lock<std::mutex> lock(mutex); // L O C K (scoped)

	S3UploadKey key(bucketName, objectName);

	S3UploadMapIter iter = map.find(key);
	if(iter != map.end() )
		return iter->second.uploadID;

	// no uploadID for this object yet, so get one from S3 server...

    const bool doS3AclPutInline = progArgs->getDoS3AclPutInline();

	S3::CreateMultipartUploadRequest createMultipartUploadRequest;
	createMultipartUploadRequest.SetBucket(bucketName);
	createMultipartUploadRequest.SetKey(objectName);

    if(doS3AclPutInline)
        TranslatorTk::applyS3PutObjectAclGrants(progArgs, createMultipartUploadRequest);

    OPLOG_PRE_OP("S3CreateMultipartUpload", bucketName + "/" + objectName, 0, 0);

	auto createMultipartUploadOutcome = s3Client->CreateMultipartUpload(
		createMultipartUploadRequest);

    OPLOG_POST_OP("S3CreateMultipartUpload", bucketName + "/" + objectName, 0, 0,
        !createMultipartUploadOutcome.IsSuccess() );

	if(!createMultipartUploadOutcome.IsSuccess() )
	{
		auto s3Error = createMultipartUploadOutcome.GetError();

        throw WorkerException(std::string("Multipart upload creation failed. ") +
            "Bucket: " + bucketName + "; "
            "Exception: " + s3Error.GetExceptionName() + "; " +
            "Message: " + s3Error.GetMessage() + "; " +
            "HTTP Error Code: " + std::to_string( (int)s3Error.GetResponseCode() ) + " (" +
                TranslatorTk::httpErrorCodeToHumanStr( (int)s3Error.GetResponseCode() ) + "); "
            "Request ID: " + s3Error.GetRequestId() );
    }

	// insert new elem with its uploadID

	S3UploadElem& elem = map[key];

	elem.uploadID = createMultipartUploadOutcome.GetResult().GetUploadId();
	elem.completedParts = std::make_unique<Aws::Vector<S3::CompletedPart>>();

	return elem.uploadID;
}

/**
 * Add completed part of shared multipart upload. When objectTotalSize is reached by
 * given numProgressBytes, then the CompletedMultipartUpload object is returned and is
 * owned by the caller and needs to be sent to the S3 server by the caller after sorting to have
 * ascending order of part numbers.
 *
 * @return either a NULL pointer or all completed parts which the caller then needs
 * 		to send to the S3 server after sorting to have ascending parts number.
 */
std::unique_ptr<Aws::Vector<S3::CompletedPart>> S3UploadStore::addCompletedPart(
	const std::string& bucketName, const std::string& objectName,
	uint64_t numProgressBytes, uint64_t objectTotalSize,
	const S3::CompletedPart& completedPart)
{
	std::unique_lock<std::mutex> lock(mutex); // L O C K (scoped)

	S3UploadKey key(bucketName, objectName);

	S3UploadMapIter iter = map.find(key);

	if(iter == map.end() )
	{
		LOGGER(Log_DEBUG, "Rejecting part add of aborted upload. "
			"Bucket: " << bucketName << "; "
			"Object: " << objectName << "; " << std::endl);

		return std::unique_ptr<Aws::Vector<S3::CompletedPart>>(nullptr);
	}

	S3UploadElem& elem = iter->second;

	elem.completedParts->push_back(completedPart); // temp vec to sort before add to actual request

	elem.numBytesDone += numProgressBytes;

    LOGGER_DEBUG_BUILD(__func__ << ":" << __LINE__ << ": "
		"elem.numBytesDone: " << elem.numBytesDone << "; "
        "objectTotalSize: " << objectTotalSize << std::endl);

	if( (elem.numBytesDone < objectTotalSize) || progArgs->getS3NoMpuCompletion() )
		return std::unique_ptr<Aws::Vector<S3::CompletedPart>>(nullptr); // not finished yet

	// ready for completion => remove object from map and transfer parts vec ownership to caller

	auto allCompletedParts = std::move(elem.completedParts);

	map.erase(key);

	return allCompletedParts;
}

/**
 * Iteratively get remaining unfinished multipart uploads to cancel them e.g. after worker
 * interruption.
 *
 * This might hand out uploadIDs that are still being processed by other workers, but that's ok,
 * since this is only called in case of error, so the others don't need to finish their uploads
 * successfully.
 *
 * All out-parameters will be left empty if no unfinished upload is left. Otherwise caller
 * is responsible for sending abort request to server.
 */
void S3UploadStore::getNextUnfinishedUpload(std::string& outBucketName, std::string& outObjectName,
	std::string& outUploadID)
{
	std::unique_lock<std::mutex> lock(mutex); // L O C K (scoped)

	if(map.empty() )
		return;

	S3UploadMapConstIter iter = map.begin();

	outBucketName = iter->first.bucketName;
	outObjectName = iter->first.objectName;
	outUploadID = iter->second.uploadID;

	map.erase(iter);
}

/**
 * Called by ProgArgs to reset/clear store for services after benchmark completion.
 *
 * Note: Not thread-safe, so can only be called while no workers are running.
 */
void S3UploadStore::reset()
{
    map.clear();
}

#endif // S3_SUPPORT
