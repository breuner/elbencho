#ifndef TOOLKITS_S3TK_H_
#define TOOLKITS_S3TK_H_

#ifdef S3_SUPPORT
	#include <aws/core/Aws.h>
    #include <aws/core/utils/HashingUtils.h>
	#include <aws/core/utils/memory/AWSMemory.h>
	#include <aws/core/utils/memory/stl/AWSStreamFwd.h>
	#include <aws/core/utils/stream/PreallocatedStreamBuf.h>
	#include INCLUDE_AWS_S3(model/CompleteMultipartUploadRequest.h)

#ifdef S3_AWSCRT
    #include INCLUDE_AWS_S3(S3CrtClient.h)

    namespace S3 = Aws::S3Crt::Model;
    using S3Client = Aws::S3Crt::S3CrtClient;
    using S3Errors = Aws::S3Crt::S3CrtErrors;
    using S3ErrorType = Aws::S3Crt::S3CrtError;
    using S3ClientConfiguration = Aws::S3Crt::ClientConfiguration;
    using S3ChecksumAlgorithm = S3::ChecksumAlgorithm;
    namespace S3ChecksumAlgorithmMapper = S3::ChecksumAlgorithmMapper;
#else
    #include INCLUDE_AWS_S3(S3Client.h)

    namespace S3 = Aws::S3::Model;
    using S3Client = Aws::S3::S3Client;
    using S3Errors = Aws::S3::S3Errors;
    using S3ErrorType = Aws::S3::S3Error;
    using S3ClientConfiguration = Aws::Client::ClientConfiguration;
    using S3ChecksumAlgorithm = S3::ChecksumAlgorithm;
    namespace S3ChecksumAlgorithmMapper = S3::ChecksumAlgorithmMapper;
#endif // S3_AWSCRT

	/**
	 * Aws::IOStream derived in-memory stream implementation for S3 object upload/download. The
	 * actual in-memory part comes from the streambuf that gets provided to the constructor.
	 */
	class S3MemoryStream : public Aws::IOStream
	{
		public:
			using Base = Aws::IOStream;

			S3MemoryStream(std::streambuf *buf) : Base(buf) {}

			virtual ~S3MemoryStream() = default;
	};

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
        static Aws::String computeKeyMD5(const Aws::String& key);
		static void scanCustomTree(const ProgArgs* progArgs, std::shared_ptr<S3Client> s3Client,
		    std::string bucketName, std::string objectPrefix, std::string outTreeFilePath);

#endif // S3_SUPPORT

	private:

#ifdef S3_SUPPORT
		static bool globalInitCalled; // to make uninit a no-op if init wasn't called

		static Aws::SDKOptions* s3SDKOptions;

    // inliners
    public:
        /**
         * Calculate checksum for S3 requests for which automatic checksum calculation is not
         * supported by the AWS SDK CPP. This applies IOStream-based requests like PutObjectRequest
         * and UploadPartRequest. For UploadPartRequest, the same checksum also needs to be given in
         * the completion request.
         *
         * @param request the s3 request to which the checksum will be added.
         * @param completedPart completion object for uploadPartRequest; NULL if this is not a
         *      UploadPartRequest.
         * @param s3ChecksumAlgorithm algorithm to add and calculate; this function will be a no-op
         *      if value is S3ChecksumAlgorithm::NOT_SET.
         * @param buf the request buffer for which to calculate the checksum.
         * @param bufLen length of buf in bytes.
         */
        template <typename REQUESTTYPE>
        static void addUploadPartRequestChecksum(REQUESTTYPE& request,
            S3::CompletedPart* completedPart, S3ChecksumAlgorithm s3ChecksumAlgorithm,
            unsigned char* buf, uint64_t bufLen)
        {
            if(s3ChecksumAlgorithm == S3ChecksumAlgorithm::NOT_SET)
                return; // nothing to do

            Aws::Utils::Stream::PreallocatedStreamBuf streamBuf(buf, bufLen);
            S3MemoryStream memStream(&streamBuf);

            switch(s3ChecksumAlgorithm)
            {
                case S3ChecksumAlgorithm::CRC32:
                {
                    Aws::String checksumStr = Aws::Utils::HashingUtils::Base64Encode(
                        Aws::Utils::HashingUtils::CalculateCRC32(memStream) );
                    request.SetChecksumAlgorithm(s3ChecksumAlgorithm);
                    request.SetChecksumCRC32(checksumStr);

                    if(completedPart != NULL)
                        completedPart->SetChecksumCRC32(checksumStr);
                } break;
                case S3ChecksumAlgorithm::CRC32C:
                {
                    Aws::String checksumStr = Aws::Utils::HashingUtils::Base64Encode(
                        Aws::Utils::HashingUtils::CalculateCRC32C(memStream) );
                    request.SetChecksumAlgorithm(s3ChecksumAlgorithm);
                    request.SetChecksumCRC32C(checksumStr);

                    if(completedPart != NULL)
                        completedPart->SetChecksumCRC32C(checksumStr);
                } break;
                case S3ChecksumAlgorithm::SHA1:
                {
                    Aws::String checksumStr = Aws::Utils::HashingUtils::Base64Encode(
                        Aws::Utils::HashingUtils::CalculateSHA1(memStream) );
                    request.SetChecksumAlgorithm(s3ChecksumAlgorithm);
                    request.SetChecksumSHA1(checksumStr);
                    if(completedPart != NULL)
                        completedPart->SetChecksumSHA1(checksumStr);
                } break;
                case S3ChecksumAlgorithm::SHA256:
                {
                    Aws::String checksumStr = Aws::Utils::HashingUtils::Base64Encode(
                        Aws::Utils::HashingUtils::CalculateSHA256(memStream) );
                    request.SetChecksumAlgorithm(s3ChecksumAlgorithm);
                    request.SetChecksumSHA256(checksumStr);
                    if(completedPart != NULL)
                        completedPart->SetChecksumSHA256(checksumStr);
                } break;

                default:
                    throw WorkerException(std::string(
                        "Invalid S3 request checksum algorithm value: ") +
                        std::to_string( (unsigned) s3ChecksumAlgorithm) );

            } // end of switch()
        }

#endif // S3_SUPPORT
};

#endif /* TOOLKITS_S3TK_H_ */
