#ifndef S3UPLOADSTORE_H_
#define S3UPLOADSTORE_H_

#ifdef S3_SUPPORT

#include <atomic>
#include <aws/core/Aws.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/StringUtils.h>
#include INCLUDE_AWS_S3(model/CompleteMultipartUploadRequest.h)
#include INCLUDE_AWS_S3(model/CreateMultipartUploadRequest.h)
#include INCLUDE_AWS_S3(model/UploadPartRequest.h)
#include <map>
#include <mutex>
#include <string>

#include "Logger.h"
#include "workers/WorkerException.h"
#include "toolkits/S3Tk.h"


class OpsLogger; // forward declaration to avoid cyclic include
class ProgArgs; // forward declaration to avoid cyclic include


/**
 * Keys of S3UploadStore
 */
struct S3UploadKey
{
	std::string bucketName;
	std::string objectName;

	S3UploadKey(const std::string& bucketName, const std::string& objectName) :
		bucketName(bucketName), objectName(objectName) {}

	bool operator<(const S3UploadKey& other) const
	{
		if(objectName.length() < other.objectName.length() )
			return true;

		if(objectName.length() > other.objectName.length() )
			return false;

		if(objectName < other.objectName)
			return true;

		if(objectName > other.objectName)
			return false;

		if(bucketName < other.bucketName)
			return true;

		return false;
	}
};

/**
 * Elements of S3UploadStore
 */
struct S3UploadElem
{
	uint64_t numBytesDone{0};
	std::string uploadID;
	std::unique_ptr<Aws::Vector<S3::CompletedPart>> completedParts; // need sort before send
};


typedef std::map<S3UploadKey, S3UploadElem> S3UploadMap;
typedef S3UploadMap::iterator S3UploadMapIter;
typedef S3UploadMap::const_iterator S3UploadMapConstIter;


/**
 * Stores state of S3 multipart uploads that are shared by multiple workers, so that we can track
 * progress to know when to send the upload completion request (or which partial multipart uploads
 * to cancel in case of interruption).
 *
 * ProgArgs pointer needs to be initialized in woker run() prep phase before using this for any
 * S3 request.
 */
class S3UploadStore
{
	public:
        void setProgArgs(const ProgArgs* progArgs);
		std::string getMultipartUploadID(const std::string& bucketName,
			const std::string& objectName, std::shared_ptr<S3Client> s3Client,
			OpsLogger& opsLog);
		std::unique_ptr<Aws::Vector<S3::CompletedPart>> addCompletedPart(
			const std::string& bucketName, const std::string& objectName,
			uint64_t numProgressBytes, uint64_t objectTotalSize,
			const S3::CompletedPart& completedPart);
		void getNextUnfinishedUpload(std::string& outBucketName, std::string& outObjectName,
			std::string& outUploadID);

	private:
		const ProgArgs* progArgs{NULL};
		std::mutex mutex; // to synchronize map and map values access
		std::map<S3UploadKey, S3UploadElem> map; // key: <bucket>/<objectkey>; value: S3UploadElem

};

#endif // S3_SUPPORT

#endif /* S3UPLOADSTORE_H_ */
