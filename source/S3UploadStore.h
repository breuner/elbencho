#ifndef S3UPLOADSTORE_H_
#define S3UPLOADSTORE_H_

#ifdef S3_SUPPORT

#include <atomic>
#include <aws/core/Aws.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/s3/model/CompleteMultipartUploadRequest.h>
#include <aws/s3/model/CreateMultipartUploadRequest.h>
#include <aws/s3/model/UploadPartRequest.h>
#include <aws/s3/S3Client.h>
#include <map>
#include <mutex>
#include <string>
#include "Logger.h"
#include "workers/WorkerException.h"


namespace S3 = Aws::S3::Model;


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
 */
class S3UploadStore
{
	public:
		std::string getMultipartUploadID(const std::string& bucketName,
			const std::string& objectName, std::shared_ptr<Aws::S3::S3Client> s3Client);
		std::unique_ptr<Aws::Vector<S3::CompletedPart>> addCompletedPart(
			const std::string& bucketName, const std::string& objectName,
			uint64_t numProgressBytes, uint64_t objectTotalSize,
			const S3::CompletedPart& completedPart);
		void getNextUnfinishedUpload(std::string& outBucketName, std::string& outObjectName,
			std::string& outUploadID);

	private:
		std::mutex mutex; // to synchronize map and map values access
		std::map<S3UploadKey, S3UploadElem> map; // key: <bucket>/<objectkey>; value: S3UploadElem

};

#endif // S3_SUPPORT

#endif /* S3UPLOADSTORE_H_ */
