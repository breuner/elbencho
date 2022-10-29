#include "Common.h"
#include "S3Tk.h"
#include "Logger.h"

#ifdef S3_SUPPORT
	#include <aws/core/Aws.h>
	#include <aws/core/utils/logging/DefaultLogSystem.h>
	#include <aws/core/utils/logging/AWSLogging.h>
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

	LOGGER(Log_DEBUG, "Initializing S3 SDK." << std::endl);

	globalInitCalled = true;

	if(progArgs.getS3LogLevel() > 0)
		Aws::Utils::Logging::InitializeAWSLogging(
			Aws::MakeShared<Aws::Utils::Logging::DefaultLogSystem>("DebugLogging",
				(Aws::Utils::Logging::LogLevel)progArgs.getS3LogLevel(), "aws_sdk_"));

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

