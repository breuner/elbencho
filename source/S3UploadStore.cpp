#ifdef S3_SUPPORT

#include "S3UploadStore.h"

/**
 * Return existing multipart uploadID (if it was previously created by another worker) or
 * get a new uploadID from S3 server.
 *
 * @throw WorkerException on error, e.g. failed server communication.
 */
std::string S3UploadStore::getMultipartUploadID(const std::string& bucketName,
	const std::string& objectName, std::shared_ptr<Aws::S3::S3Client> s3Client)
{
	std::unique_lock<std::mutex> lock(mutex); // L O C K (scoped)

	S3UploadKey key(bucketName, objectName);

	S3UploadMapIter iter = map.find(key);
	if(iter != map.end() )
		return iter->second.uploadID;

	// no uploadID for this object yet, so get one from S3 server

	S3::CreateMultipartUploadRequest createMultipartUploadRequest;
	createMultipartUploadRequest.SetBucket(bucketName);
	createMultipartUploadRequest.SetKey(objectName);

	auto createMultipartUploadOutcome = s3Client->CreateMultipartUpload(
		createMultipartUploadRequest);

	if(!createMultipartUploadOutcome.IsSuccess() )
	{
		auto s3Error = createMultipartUploadOutcome.GetError();

		throw WorkerException(std::string("Multipart upload creation failed. ") +
			"Bucket: " + bucketName + "; "
			"Exception: " + s3Error.GetExceptionName() + "; " +
			"Message: " + s3Error.GetMessage() );
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

	if(elem.numBytesDone < objectTotalSize)
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


#endif // S3_SUPPORT
