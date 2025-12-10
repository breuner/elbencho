#include "Common.h"
#include "Logger.h"
#include "OpsLogger.h"
#include "ProgArgs.h"
#include "toolkits/Base64Encoder.h"
#include "toolkits/S3CredentialStore.h"
#include "toolkits/S3Tk.h"
#include "toolkits/StringTk.h"
#include "toolkits/TerminalTk.h"
#include "toolkits/UnitTk.h"

#ifdef S3_SUPPORT
    #include <aws/core/auth/AWSCredentialsProvider.h>
    #include <aws/core/auth/AWSCredentialsProviderChain.h>
    #include <aws/core/Aws.h>
    #include <aws/core/utils/crypto/MD5.h>
    #include <aws/core/utils/logging/DefaultLogSystem.h>
    #include <aws/core/utils/logging/AWSLogging.h>
    #include <aws/core/utils/threading/SameThreadExecutor.h>
    #include INCLUDE_AWS_S3(model/ListObjectsV2Request.h)

    #ifdef S3_AWSCRT
        #include <aws/core/utils/logging/CRTLogSystem.h>
    #endif


    /* print a note for AWS CRT with older SDK versions because of known issue with
        SetContinueRequestHandler: https://github.com/aws/aws-sdk-cpp/issues/3639 */
    #if defined(S3_AWSCRT) && !AWS_SDK_AT_LEAST(1, 11, 708)
        #pragma message(" *** NOTE: " \
            "Using AWS CRT S3 with AWS SDK CPP version older than 1.11.708. " \
            "Disabling some convenience features due to known issues.")
    #endif


    std::stringbuf S3MemoryStream::staticZeroStreamBuf;

    bool S3Tk::globalInitCalled = false;
    Aws::SDKOptions* S3Tk::s3SDKOptions = NULL;
#endif // S3_SUPPORT



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
void S3Tk::initS3Global(const ProgArgs* progArgs)
{
#ifdef S3_SUPPORT

    if(globalInitCalled)
    {
        LOGGER(Log_DEBUG, "Skipping repeated S3 SDK init." << std::endl);
        return;
    }

	LOGGER(Log_DEBUG, "Initializing S3 SDK." << std::endl);

	globalInitCalled = true;

    s3SDKOptions = new Aws::SDKOptions;

    if(progArgs->getS3LogLevel() > 0)
    {
        s3SDKOptions->loggingOptions.logLevel =
	        (Aws::Utils::Logging::LogLevel)progArgs->getS3LogLevel();

        s3SDKOptions->loggingOptions.logger_create_fn = [&]()
        {
            return Aws::MakeShared<Aws::Utils::Logging::DefaultLogSystem>(
                "CustomLogSystem", (Aws::Utils::Logging::LogLevel)progArgs->getS3LogLevel(),
                progArgs->getS3LogfilePrefix() );
        };

        #ifdef S3_AWSCRT
            s3SDKOptions->loggingOptions.crt_logger_create_fn = [&]()
            {
                return Aws::MakeShared<Aws::Utils::Logging::DefaultCRTLogSystem>("DebugLogging",
                    (Aws::Utils::Logging::LogLevel)progArgs->getS3LogLevel() );
            };
        #endif // S3_AWSCRT
	}

	Aws::InitAPI(*s3SDKOptions);

	/* note: this is to avoid a long delay for the client config trying to contact the
		AWS instance metadata service to retrieve credentials, although we already set them.
		this way, it can still manually be overrideen through the environment variable. */
	setenv("AWS_EC2_METADATA_DISABLED", "true", 0);

    // Initialize credential store if multi-credentials are specified
    if(!progArgs->getS3CredentialsFile().empty())
    {
        S3CredentialStore::getInstance().loadCredentialsFromFile(progArgs->getS3CredentialsFile());
        LOGGER(Log_DEBUG, "Loaded S3 credentials from file: "
               << progArgs->getS3CredentialsFile() << std::endl);
    }
    else if(!progArgs->getS3CredentialsList().empty())
    {
        S3CredentialStore::getInstance().loadCredentialsFromList(progArgs->getS3CredentialsList());
        LOGGER(Log_DEBUG, "Loaded S3 credentials from command line list" << std::endl);
    }
    else if(!progArgs->getS3AccessKey().empty() || !progArgs->getS3AccessSecret().empty())
    {
        // Add single credential to store if provided
        S3CredentialStore::getInstance().addCredential(
            progArgs->getS3AccessKey(), progArgs->getS3AccessSecret());
        LOGGER(Log_DEBUG, "Using single S3 credential" << std::endl);
    }

#endif // S3_SUPPORT
}

/**
 * Globally uninitialize the AWS SDK. Call this only once for the whole application lifetime. (See
 * S3Tk::initS3Global comments for why this may only be called once.)
 */
void S3Tk::uninitS3Global(const ProgArgs* progArgs)
{
#ifdef S3_SUPPORT

	if(!globalInitCalled)
		return; // nothing to do if init wasn't called

	LOGGER(Log_DEBUG, "Shutting down S3 SDK." << std::endl);

	Aws::ShutdownAPI(*s3SDKOptions);

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
 * * @isInterruptionRequested will be given to S3 retry strategy object to stop retries if
 *     interruption was requested; can be NULL.
 * @outS3EndpointStr will be set to the selected s3 endpoint from progArgs vec; can be NULL.
 */
std::shared_ptr<S3Client> S3Tk::initS3Client(const ProgArgs* progArgs,
    size_t workerRank, std::atomic_bool* isInterruptionRequested, std::string* outS3EndpointStr)
{
    if(progArgs->getS3EndpointsVec().empty() )
        throw ProgException(std::string(__func__) + " cannot init S3 client if no S3 endpoints are "
            "provided.");

    S3ClientConfiguration config;

    size_t numParallelRequests = progArgs->getUseS3ClientSingleton() ?
        progArgs->getNumThreads() * progArgs->getIODepth() : progArgs->getIODepth();

    /* note: DefaultExecutor creates a new temporary thread for each async request, so is unbounded
        and has overhead for thread creation. Thus, we only use it without I/O depth (i.e. no async
        I/O) to avoid dynamic creation of separate threads. PooledThreadExecutor has a fixed pool of
        threads. */
    /* note: config.executor is not used by S3CrtClient, it has config.clientBootstrap for that,
        which defaults to one EventLoop thread per cpu core, but config.executor is used for
        offloading callbacks/lambdas to separate threads. */
    config.executor = (numParallelRequests > 1) ?
        (std::shared_ptr<Aws::Utils::Threading::Executor>)
            std::make_shared<Aws::Utils::Threading::PooledThreadExecutor>(numParallelRequests) :
        (std::shared_ptr<Aws::Utils::Threading::Executor>)
            std::make_shared<Aws::Utils::Threading::DefaultExecutor>();

    config.verifySSL = false; // to avoid self-signed certificate errors; ignored by S3CrtClient
    config.enableEndpointDiscovery = false; // to avoid delays for discovery
    config.maxConnections = numParallelRequests; /* max tcp conns; ignored by S3CrtClient, uses
        throughputTargetGbps for implicit calculation */
    config.retryStrategy = std::make_shared<InterruptibleRetryStrategy>(
        isInterruptionRequested); // note: restryStrategy is not used by S3CrtClient
    config.connectTimeoutMs = 5000;
    config.requestTimeoutMs = 300000;
    config.disableExpectHeader = true;
    config.enableTcpKeepAlive = true;
    config.requestCompressionConfig.requestMinCompressionSizeBytes = 1;
    config.requestCompressionConfig.useRequestCompression = (progArgs->getS3NoCompression() ?
        Aws::Client::UseRequestCompression::DISABLE : Aws::Client::UseRequestCompression::ENABLE);
    config.checksumConfig.requestChecksumCalculation =
        Aws::Client::RequestChecksumCalculation::WHEN_REQUIRED;


#ifdef S3_AWSCRT
    config.useVirtualAddressing = false; /* only exists in config of s3-crt and not effective in
        client constructor (but effective there for non-crt s3 client) */
    config.partSize = progArgs->getBlockSize() ? progArgs->getBlockSize() : 5 * 1024 * 1024; /*
        S3CrtClient would internally split into smaller MPU parts if smaller than blockSize */
    config.throughputTargetGbps = 100; /* used for implicit calculation of max connections, as
        S3CrtClient has no explicit number of max connections; see aws-c-s3/source/s3_client.c
        s_get_ideal_connection_number_from_throughput() */
#endif // S3_AWSCRT

    if(!progArgs->getS3Region().empty() )
        config.region = progArgs->getS3Region();

    // select endpoint...

    const StringVec& endpointsVec = progArgs->getS3EndpointsVec();
    size_t numEndpoints = endpointsVec.size();
    std::string endpoint = endpointsVec[workerRank % numEndpoints];

    config.endpointOverride = endpoint;

    if(outS3EndpointStr)
        *outS3EndpointStr = endpoint;

    // set credentials...

    /* note: just passing Aws::Auth::AWSCredentials to the s3client constructor doesn't override
        credentials from profiles in home directory, so we need to pass a CredentialsProvider. */

    std::shared_ptr<Aws::Auth::AWSCredentialsProvider> credentialsProvider;

    if(!progArgs->getS3AccessKey().empty() || !progArgs->getS3AccessSecret().empty())
    {
        // Single credential mode
        credentialsProvider = std::make_shared<Aws::Auth::SimpleAWSCredentialsProvider>(
            progArgs->getS3AccessKey(), progArgs->getS3AccessSecret());

        LOGGER(Log_DEBUG, "Using single S3 credential. "
            "Worker rank: " << workerRank << std::endl);
    }
    else if(!progArgs->getS3CredentialsFile().empty() || !progArgs->getS3CredentialsList().empty())
    {
        // Multi-credential mode (round-robin)
        credentialsProvider = S3CredentialStore::getInstance().getCredential(workerRank);

        LOGGER(Log_DEBUG, "Using multi-credential from store. "
            "Worker rank: " << workerRank << std::endl);
    }
    else
    {
        // Fallback: use default chain
        credentialsProvider = std::make_shared<Aws::Auth::DefaultAWSCredentialsProviderChain>();

        LOGGER(Log_DEBUG, "Using default AWS credential chain. "
            "Worker rank: " << workerRank << std::endl);
    }

    // create s3 client for this worker
    std::shared_ptr<S3Client> s3Client = std::make_shared<S3Client>(credentialsProvider,
        config, (Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy)progArgs->getS3SignPolicy(),
        false);

    return s3Client;
}

/**
 * @brief Compute MD5 hash of the given base64 encoded key.
 * @param encodedKey base64 encoded key.
 * @return md5 of the given key.
 */
Aws::String S3Tk::computeKeyMD5(const Aws::String& encodedKey)
{
    Aws::Utils::Crypto::MD5 md5;

    auto decodedKey = Aws::Utils::HashingUtils::Base64Decode(encodedKey);

    Aws::Utils::Crypto::HashResult md5Hash = md5.Calculate(
        Aws::String(reinterpret_cast<const char*>(decodedKey.GetUnderlyingData() ),
        decodedKey.GetLength() ) );

    return Aws::Utils::HashingUtils::Base64Encode(md5Hash.GetResult() );
}

/**
 *  List all entries under the given S3 path and write them to the given file in custom tree file
 *  format.
 *
 *  @bucketName s3 bucket to scan
 *  @objectPrefix object prefix to scan under given bucket; can be empty.
 *  @outTreeFilePath path to output file in custom tree format.
 */
void S3Tk::scanCustomTree(const ProgArgs* progArgs, std::shared_ptr<S3Client> s3Client,
    std::string bucketName, std::string objectPrefix, std::string outTreeFilePath)
{
    const unsigned numObjectsPerRequest = 1000;
    const bool isLiveStatsDisabled = progArgs->getDisableLiveStats();
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

            for(const S3::Object& obj : listOutcome.GetResult().GetContents() )
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
