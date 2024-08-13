#ifndef TOOLKITS_S3TK_H_
#define TOOLKITS_S3TK_H_

#include "ProgArgs.h"

#ifdef S3_SUPPORT
	#include <aws/core/Aws.h>
    #include <aws/s3/S3Client.h>
#endif

class ProgArgs; // forward declaration

class S3Tk
{
	public:
		static void initS3Global(const ProgArgs& progArgs);
		static void uninitS3Global(const ProgArgs& progArgs);

#ifdef S3_SUPPORT
        static std::shared_ptr<Aws::S3::S3Client> initS3Client(
            const ProgArgs& progArgs, size_t workerRank =
                std::chrono::system_clock::now().time_since_epoch().count() );
		static void scanCustomTree(const ProgArgs& progArgs,
		    std::shared_ptr<Aws::S3::S3Client> s3Client, std::string bucketName,
		    std::string objectPrefix, std::string outTreeFilePath);
#endif // S3_SUPPORT

	private:

	#ifdef S3_SUPPORT
		static bool globalInitCalled; // to make uninit a no-op if init wasn't called

		static Aws::SDKOptions* s3SDKOptions;
	#endif
};

#endif /* TOOLKITS_S3TK_H_ */
