#ifndef TOOLKITS_S3TK_H_
#define TOOLKITS_S3TK_H_

#ifdef S3_SUPPORT
	#include <aws/core/Aws.h>

#ifdef S3_AWSCRT
    #include INCLUDE_AWS_S3(S3CrtClient.h)

    namespace S3 = Aws::S3Crt::Model;
    using S3Client = Aws::S3Crt::S3CrtClient;
    using S3Errors = Aws::S3Crt::S3CrtErrors;
    using S3ErrorType = Aws::S3Crt::S3CrtError;
    using S3ClientConfiguration = Aws::S3Crt::ClientConfiguration;
    using S3ChecksumAlgorithm = Aws::S3Crt::Model::ChecksumAlgorithm;
    namespace S3ChecksumAlgorithmMapper = Aws::S3Crt::Model::ChecksumAlgorithmMapper;
#else
    #include INCLUDE_AWS_S3(S3Client.h)

    namespace S3 = Aws::S3::Model;
    using S3Client = Aws::S3::S3Client;
    using S3Errors = Aws::S3::S3Errors;
    using S3ErrorType = Aws::S3::S3Error;
    using S3ClientConfiguration = Aws::Client::ClientConfiguration;
    using S3ChecksumAlgorithm = Aws::S3::Model::ChecksumAlgorithm;
    namespace S3ChecksumAlgorithmMapper = Aws::S3::Model::ChecksumAlgorithmMapper;
#endif // S3_AWSCRT

#endif // S3_SUPPORT


class ProgArgs; // forward declaration


class S3Tk
{
	public:
		static void initS3Global(const ProgArgs* progArgs);
		static void uninitS3Global(const ProgArgs* progArgs);

#ifdef S3_SUPPORT
        static std::shared_ptr<S3Client> initS3Client(
            const ProgArgs* progArgs, size_t workerRank =
                std::chrono::system_clock::now().time_since_epoch().count(),
            std::string* outS3EndpointStr = NULL);
		static void scanCustomTree(const ProgArgs* progArgs, std::shared_ptr<S3Client> s3Client,
		    std::string bucketName, std::string objectPrefix, std::string outTreeFilePath);

        static Aws::String computeKeyMD5(const Aws::String& key);

#endif // S3_SUPPORT

	private:

#ifdef S3_SUPPORT
		static bool globalInitCalled; // to make uninit a no-op if init wasn't called

		static Aws::SDKOptions* s3SDKOptions;
#endif // S3_SUPPORT
};

#endif /* TOOLKITS_S3TK_H_ */
