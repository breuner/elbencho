#include "Common.h"
#include "Logger.h"
#include "OpsLogger.h"
#include "toolkits/Base64Encoder.h"
#include "toolkits/S3Tk.h"
#include "toolkits/StringTk.h"
#include "toolkits/UnitTk.h"
#include "toolkits/TerminalTk.h"

#ifdef S3_SUPPORT
    #include <aws/core/auth/AWSCredentialsProvider.h>
	#include <aws/core/Aws.h>
	#include <aws/core/utils/logging/DefaultLogSystem.h>
	#include <aws/core/utils/logging/AWSLogging.h>
    #include <aws/s3/model/ListObjectsV2Request.h>
#endif

#ifdef S3_SUPPORT
	bool S3Tk::globalInitCalled = false;
	Aws::SDKOptions* S3Tk::s3SDKOptions = NULL; // needed for init and again for uninit later
#endif


/**
 * Globally initialize the AWS SDK. Run this before any other threads are running and only once
 * for the whole application lifetime.
 *
 * This is a no-op if this executable was built without S3 support.
 *
 * Note: The SDK initializer contains (amongst others) curl_global_init(), which must be called
 * before any threads are started. The SDK uninit method also does not reset the init-done-already
 * flag in ShutdownAPI(), which is why initialization can only be done once and will not work again
 * after ShutdownAPI was called.
 */
void S3Tk::initS3Global(const ProgArgs& progArgs)
{
#ifdef S3_SUPPORT

    if(globalInitCalled)
    {
        LOGGER(Log_DEBUG, "Skipping repeated S3 SDK init." << std::endl);
        return;
    }

	LOGGER(Log_DEBUG, "Initializing S3 SDK." << std::endl);

	globalInitCalled = true;

	if(progArgs.getS3LogLevel() > 0)
		Aws::Utils::Logging::InitializeAWSLogging(
			Aws::MakeShared<Aws::Utils::Logging::DefaultLogSystem>("DebugLogging",
				(Aws::Utils::Logging::LogLevel)progArgs.getS3LogLevel(),
				progArgs.getS3LogfilePrefix() ) );

	s3SDKOptions = new Aws::SDKOptions;
	Aws::InitAPI(*s3SDKOptions);

	/* note: this is to avoid a long delay for the client config trying to contact the
		AWS instance metadata service to retrieve credentials, although we already set them.
		this way, it can still manually be overrideen through the environment variable. */
	setenv("AWS_EC2_METADATA_DISABLED", "true", 0);

#endif // S3_SUPPORT
}

/**
 * Globally uninitialize the AWS SDK. Call this only once for the whole application lifetime. (See
 * S3Tk::initS3Global comments for why this may only be called once.)
 */
void S3Tk::uninitS3Global(const ProgArgs& progArgs)
{
#ifdef S3_SUPPORT

	if(!globalInitCalled)
		return; // nothing to do if init wasn't called

	LOGGER(Log_DEBUG, "Shutting down S3 SDK." << std::endl);

	Aws::ShutdownAPI(*s3SDKOptions);

	if(progArgs.getS3LogLevel() > 0)
		Aws::Utils::Logging::ShutdownAWSLogging();

#endif // S3_SUPPORT
}

#ifdef S3_SUPPORT

/**
 * Initialize AWS S3 SDK & S3 client object. Each client can only connect to a single S3 endpoint
 * IP address, so typically there is one S3 client object per thread to use multiple endpoints.
 *
 * S3 endpoints get assigned round-robin to workers based on workerRank.
 *
 * For uninit/cleanup later, just use the reset() method of the returned std::shared_ptr to free
 * the associated client object.
 *
 * @workerRank to select s3 endpoint if multiple endpoints are available in progArgs; otherwise
 *     a clock-based random number will be chosen.
 */
std::shared_ptr<Aws::S3::S3Client> S3Tk::initS3Client(const ProgArgs& progArgs,
    size_t workerRank)
{
    if(progArgs.getS3EndpointsVec().empty() )
        throw ProgException(std::string(__func__) + " cannot init S3 client if no S3 endpoints are "
            "provided.");

    Aws::Client::ClientConfiguration config;

    config.verifySSL = false; // to avoid self-signed certificate errors
    config.enableEndpointDiscovery = false; // to avoid delays for discovery
    config.maxConnections = progArgs.getIODepth();
    config.connectTimeoutMs = 5000;
    config.requestTimeoutMs = 300000;
    config.executor = std::make_shared<Aws::Utils::Threading::PooledThreadExecutor>(1);
    config.disableExpectHeader = true;
    config.enableTcpKeepAlive = true;
    config.requestCompressionConfig.requestMinCompressionSizeBytes = 1;
    config.requestCompressionConfig.useRequestCompression = (progArgs.getS3NoCompression() ?
        Aws::Client::UseRequestCompression::DISABLE : Aws::Client::UseRequestCompression::ENABLE);

    if(!progArgs.getS3Region().empty() )
        config.region = progArgs.getS3Region();

    // select endpoint...

    const StringVec& endpointsVec = progArgs.getS3EndpointsVec();
    size_t numEndpoints = endpointsVec.size();
    std::string endpoint = endpointsVec[workerRank % numEndpoints];

    config.endpointOverride = endpoint;

    // set credentials...

    Aws::Auth::AWSCredentials credentials;

    if(!progArgs.getS3AccessKey().empty() )
        credentials.SetAWSAccessKeyId(progArgs.getS3AccessKey() );

    if(!progArgs.getS3AccessSecret().empty() )
        credentials.SetAWSSecretKey(progArgs.getS3AccessSecret() );

    // create s3 client for this worker
    std::shared_ptr<Aws::S3::S3Client> s3Client = std::make_shared<Aws::S3::S3Client>(credentials,
        config, (Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy)progArgs.getS3SignPolicy(),
        false);

    return s3Client;

}

#endif // S3_SUPPORT

#ifdef S3_SUPPORT

/**
 *  List all entries under the given S3 path and write them to the given file in custom tree file
 *  format.
 *
 *  @bucketName s3 bucket to scan
 *  @objectPrefix object prefix to scan under given bucket; can be empty.
 *  @outTreeFilePath path to output file in custom tree format.
 */
void S3Tk::scanCustomTree(const ProgArgs& progArgs, std::shared_ptr<Aws::S3::S3Client> s3Client,
    std::string bucketName, std::string objectPrefix, std::string outTreeFilePath)
{
    const unsigned numObjectsPerRequest = 1000;
    const bool isLiveStatsDisabled = progArgs.getDisableLiveStats();
    const time_t consoleUpdateIntervalT = 2; // time_t, so unit is seconds
    const time_t startT = time(NULL); // seconds since the epoch (for elapsed time)
    time_t consoleLastUpdateT = startT; // seconds since the epoch (for console updates)

    std::string nextContinuationToken;
    bool isTruncated; // true if S3 server reports more objects left to retrieve

    size_t numObjsFound = 0;
    uint64_t numBytesFound = 0;

    std::ofstream fileStream;

    fileStream.open(outTreeFilePath, std::ofstream::out | std::ofstream::trunc);

    if(!fileStream)
        throw ProgException("Opening tree scan results file failed: " + outTreeFilePath);

    // add base64 encoding header to file
    fileStream << TREEFILE_BASE64ENCODING_HEADER << std::endl;

    // try-block to clean-up console buffering on error
    try
    {
        isLiveStatsDisabled || TerminalTk::disableConsoleBuffering();

        // receive all objects under this bucket that match prefix and write to file...
        do
        {
            // receive a batch of object names through listing...

            S3::ListObjectsV2Request listRequest;
            listRequest.SetBucket(bucketName);
            listRequest.SetPrefix(objectPrefix);
            listRequest.SetMaxKeys(numObjectsPerRequest);

            if(!nextContinuationToken.empty() )
                listRequest.SetContinuationToken(nextContinuationToken);

            S3::ListObjectsV2Outcome listOutcome = s3Client->ListObjectsV2(listRequest);

            IF_UNLIKELY(!listOutcome.IsSuccess() )
            {
                auto s3Error = listOutcome.GetError();

                throw WorkerException(std::string("Object listing v2 failed. ") +
                    "Bucket: " + bucketName + "; "
                    "Prefix: " + objectPrefix + "; "
                    "ContinuationToken: " + nextContinuationToken + "; "
                    "NumObjectsPerRequest: " + std::to_string(numObjectsPerRequest) + "; "
                    "Exception: " + s3Error.GetExceptionName() + "; " +
                    "Message: " + s3Error.GetMessage() + "; " +
                    "HTTP Error Code: " + std::to_string( (int)s3Error.GetResponseCode() ) );
            }

            // write a line in treefile format for each received object

            std::string currentEntry;

            for(const Aws::S3::Model::Object& obj : listOutcome.GetResult().GetContents() )
            {
                currentEntry = obj.GetKey().substr(objectPrefix.size(), std::string::npos);

                numObjsFound++;
                numBytesFound += obj.GetSize();

                fileStream << PATHSTORE_FILE_LINE_PREFIX " " << obj.GetSize() << " " <<
                    Base64Encoder::encode(currentEntry) << std::endl;
            }

            nextContinuationToken = listOutcome.GetResult().GetNextContinuationToken();
            isTruncated = listOutcome.GetResult().GetIsTruncated();

            if( (time(NULL) - consoleLastUpdateT) >= consoleUpdateIntervalT)
            { // time to update console live stats line
                StringTk::eraseControlChars(currentEntry); // avoid '\n' and such in console output

                std::ostringstream stream;
                stream << "Bucket scan in progress... "
                    "Objects: " << UnitTk::numToHumanStrBase10(numObjsFound) << "; "
                    "Bytes: " << UnitTk::numToHumanStrBase2(numBytesFound) << "; "
                    "Elapsed: " << UnitTk::elapsedSecToHumanStr(time(NULL) - startT) << "; "
                    "Current: " << currentEntry;

                isLiveStatsDisabled || TerminalTk::rewriteConsoleLine(stream.str() );
                consoleLastUpdateT = time(NULL); // update time for next console update
            }

        } while(isTruncated); // end of while numObjectsLeft loop
    }
    catch(...)
    {
        isLiveStatsDisabled || TerminalTk::resetConsoleBuffering();
        throw;
    }

    isLiveStatsDisabled || TerminalTk::clearConsoleLine();
    isLiveStatsDisabled || TerminalTk::resetConsoleBuffering();

    std::cout << "NOTE: Bucket scan finished. "
        "Objects: " << numObjsFound << "; "
        "Elapsed: " << (time(NULL) - startT) << "s" << std::endl;

}

#endif // S3_SUPPORT
