// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <boost/algorithm/string.hpp>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

#include "Logger.h"
#include "modes/s3/S3ProgArgs.h"
#include "modes/s3/S3Transport.h"
#include "PathStore.h"
#include "ProgArgs.h"
#include "ProgException.h"
#include "toolkits/StringTk.h"
#include "toolkits/TerminalTk.h"
#include "toolkits/TranslatorTk.h"
#include "toolkits/UnitTk.h"

#define S3ENDPOINTS_DELIMITERS      ", \n\r" // delimiters for S3 endpoints list string

#define AWS_SDK_LOGPREFIX_DEFAULT   "aws_sdk_"

#define ENDL                        << std::endl << // just to make help text print lines shorter

#define S3_ENV_ACCESS_KEY           "AWS_ACCESS_KEY_ID" // environment variable for s3 access key
#define S3_ENV_SECRET_KEY           "AWS_SECRET_ACCESS_KEY" // environment variable for s3 secret
#define S3_ENV_SESSION_TOKEN        "AWS_SESSION_TOKEN" // environment variable for s3 session token
#define S3_ENV_ENDPOINT_URL_S3      "AWS_ENDPOINT_URL_S3" // S3-specific endpoint URL (AWS CLI v2)
#define S3_ENV_ENDPOINT_URL         "AWS_ENDPOINT_URL" // generic endpoint URL for all AWS services
#define S3_ENV_REGION               "AWS_REGION" // AWS region (AWS CLI v2 preferred name)
#define S3_ENV_REGION_DEFAULT       "AWS_DEFAULT_REGION" // AWS region (legacy name)

// Names of features for printBuildInfo()
#define FEATURE_NAME_S3_SUPPORT     "s3"
#define FEATURE_NAME_S3_AWSCRT      "s3crt"


/**
 * Define the S3 options. Called by ProgArgs::defineAllowedArgs() at the alphabetical position of
 * the S3 options, so that the help output stays sorted.
 */
void S3ProgArgs::defineArgs(bpo::options_description& generic, bpo::options_description& hidden)
{
    hidden.add_options()
        (ARG_S3CHECKSUM_ALGO_2_LONG, bpo::value<std::string>(),
            "Hidden compat alias for \"--" ARG_S3CHECKSUM_ALGO_LONG "\".")
    ;

#ifdef S3_SUPPORT
    generic.add_options()
        (ARG_S3ACLGET_LONG, bpo::bool_switch(&this->runS3AclGet),
            "Get S3 object ACLs.")
        (ARG_S3ACLGRANTEE_LONG, bpo::value(&this->s3AclGrantee),
            "S3 object ACL grantee. This can be used with special values to set a canned ACL, in "
            "which case the grantee type and permissions arguments will be ignored: private, "
            "public-read, public-read-write, authenticated-read")
        (ARG_S3ACLGRANTEETYPE_LONG, bpo::value(&this->s3AclGranteeType),
            "S3 object ACL grantee type. Possible values: "
            ARG_S3ACL_GRANTEE_TYPE_ID ", " ARG_S3ACL_GRANTEE_TYPE_EMAIL ", "
            ARG_S3ACL_GRANTEE_TYPE_URI ", " ARG_S3ACL_GRANTEE_TYPE_GROUP)
        (ARG_S3ACLGRANTS_LONG, bpo::value(&this->s3AclGranteePermissions),
            "S3 object ACL grantee permissions. Comma-separated list of these values: "
            ARG_S3ACL_PERM_NONE_NAME ", " ARG_S3ACL_PERM_FULL_NAME ", "
            ARG_S3ACL_PERM_FLAG_READ_NAME ", " ARG_S3ACL_PERM_FLAG_WRITE_NAME ", "
            ARG_S3ACL_PERM_FLAG_READACP_NAME ", " ARG_S3ACL_PERM_FLAG_WRITEACP_NAME)
        (ARG_S3ACLPUT_LONG, bpo::bool_switch(&this->runS3AclPut),
            "Put S3 object ACLs. This requires definition of grantee, grantee type and "
            "permissions.")
        (ARG_S3ACLPUTINLINE_LONG, bpo::bool_switch(&this->doS3AclPutInline),
            "Set S3 object ACL inlined in object upload. This requires definition of grantee and "
            "permissions. Grantee type is specified as part of the grantee name: "
            "\"emailAddress=example@myorg.org\" or \"id=123\" or \"uri=...\".")
        (ARG_S3ACLVERIFY_LONG, bpo::bool_switch(&this->doS3AclVerify),
            "Verify S3 object and bucket ACLs based on given grantee, grantee type and "
            "permissions. This only effective in the corresponding get object or bucket ACL "
            "phase.")
        (ARG_S3BUCKETACLGET_LONG, bpo::bool_switch(&this->runS3BucketAclGet),
            "Get S3 bucket ACLs.")
        (ARG_S3BUCKETACLPUT_LONG, bpo::bool_switch(&this->runS3BucketAclPut),
            "Put S3 bucket ACLs. This requires definition of grantee, grantee type and "
            "permissions.")
        (ARG_S3BUCKETTAG_LONG, bpo::bool_switch(&this->doS3BucketTag),
            "Activate bucket tagging operations.")
        (ARG_S3BUCKETTAGVERIFY_LONG, bpo::bool_switch(&this->doS3BucketTagVerify),
            "Verify the correctness of S3 bucket tagging results. (Requires "
            "\"--" ARG_S3BUCKETTAG_LONG "\")")
        (ARG_S3BUCKETVER_LONG, bpo::bool_switch(&this->doS3BucketVersioning),
            "Activate bucket versioning operations.")
        (ARG_S3BUCKETVERVERIFY_LONG, bpo::bool_switch(&this->doS3BucketVersioningVerify),
            "Verify the correctness of S3 bucket versioning settings. (Requires "
            "\"--" ARG_S3BUCKETVER_LONG "\")")
        (ARG_S3CHECKSUM_ALGO_LONG, bpo::value(&this->s3ChecksumAlgoStr),
            "S3 checksum algorithm to use (CRC32, CRC32C, SHA1, SHA256). This sets the "
            "x-amz-sdk-checksum-algorithm header for S3 operations. (EXPERIMENTAL)")
        (ARG_S3CREDFILE_LONG, bpo::value(&this->s3CredentialsFile),
            "Path to file containing multiple S3 credentials. Each line in format: "
            "access_key:secret_key. Lines starting with # are treated as comments.")
        (ARG_S3CREDLIST_LONG, bpo::value(&this->s3CredentialsList),
            "Comma-separated list of S3 credentials. Each credential in format: "
            "access_key:secret_key")
        (ARG_S3ENDPOINTS_LONG, bpo::value(&this->s3EndpointsStr),
            "Comma-separated list of S3 endpoints. When this argument is used, the given "
            "benchmark paths are used as bucket names. Also see \"--" ARG_S3ACCESSKEY_LONG "\" & "
            "\"--" ARG_S3ACCESSSECRET_LONG "\". (This can also be set via the "
            S3_ENV_ENDPOINT_URL_S3 " or " S3_ENV_ENDPOINT_URL " env variable.) "
            "(Format: [http(s)://]hostname[:port])")
        (ARG_S3FASTGET_LONG, bpo::bool_switch(&this->useS3FastRead),
            "Send downloaded objects directly to /dev/null instead of a memory buffer. This option "
            "is incompatible with any buffer post-processing options like data verification or "
            "GPU data transfer.")
        (ARG_S3FASTPUT_LONG,
            "Reduce CPU overhead for uploads. Enables \"--" ARG_S3SIGNPAYLOAD_LONG "=2 (never)\", "
            "\"--" ARG_S3NOCOMPRESS_LONG "\".")
        (ARG_S3IGNOREERRORS_LONG, bpo::bool_switch(&this->ignoreS3Errors),
            "Ignore any S3 upload/download errors. Useful for stress-testing.")
        (ARG_S3ACCESSKEY_LONG, bpo::value(&this->s3AccessKey),
            "S3 access key. (This can also be set via the " S3_ENV_ACCESS_KEY " env variable.)")
        (ARG_S3LISTOBJ_LONG, bpo::value(&this->runS3ListObjNum),
            "List objects. The given number is the maximum number of objects to retrieve. Use "
            "\"--" ARG_S3OBJECTPREFIX_LONG "\" to start listing with the given prefix. (Multiple "
            "threads will only be effective if multiple buckets are given.)")
        (ARG_S3LISTOBJPARALLEL_LONG, bpo::bool_switch(&this->runS3ListObjParallel),
            "List objects in parallel. Requires a dataset created via \"-" ARG_NUMDIRS_SHORT "\" "
            "and \"-" ARG_NUMFILES_SHORT "\" options and parallelizes by using different S3 "
            "listing prefixes for each thread.")
        (ARG_S3LISTOBJVERIFY_LONG, bpo::bool_switch(&this->doS3ListObjVerify),
            "Verify the correctness of S3 server object listing results in combination with "
            "\"--" ARG_S3LISTOBJPARALLEL_LONG "\". This requires the dataset to be created with "
            "the same values for \"-" ARG_NUMDIRS_SHORT "\" and \"-" ARG_NUMFILES_SHORT "\".")
        (ARG_S3LOGLEVEL_LONG, bpo::value(&this->s3LogLevel),
            "Log level of AWS S3 SDK. See \"--" ARG_S3LOGFILEPREFIX_LONG "\" for filename. "
            "(Default: 0=disabled; Max: 6)")
        (ARG_S3LOGFILEPREFIX_LONG, bpo::value(&this->s3LogfilePrefix),
            "Path and filename prefix of AWS S3 SDK log file. \"DATE.log\" will get appended to "
            "the given filename. "
            "(Default: \"" AWS_SDK_LOGPREFIX_DEFAULT "\" in current working directory)")
        (ARG_S3MAXCONNS_LONG, bpo::value(&this->s3MaxConnections),
            "Max number of connections per S3 client instance. "
            "(Default: Number of threads sharing the instance times iodepth.) "
            "[Not effective for builds with feature " FEATURE_NAME_S3_AWSCRT ".]")
        (ARG_S3MPUSHARING_LONG, bpo::bool_switch(&this->useS3MPUSharing),
            "Use shared multipart upload mode for S3 objects from multiple clients. For this mode, "
            "object names need to be given as parameters (e.g. \"mybucket/myobj[1-10]\").")
        (ARG_S3MPUSIZEVAR_LONG, bpo::value(&this->s3MpuSizeVarianceOrigStr),
            "Maximum number of bytes to subtract from part size of multipart uploads for random "
            "variance in part sizes. The last uploaded part will be correspondingly larger to "
            "meet the full given object size. This only works for plain sequential uploads in "
            "objects-per-thread mode, i.e. in combination with \"-" ARG_NUMFILES_SHORT "\".")
        (ARG_S3MPUSPLITSIZE_LONG, bpo::value(&this->s3MpuSplitSizeOrigStr),
            "Normally, S3 MPU part size is defined via the \"-" ARG_BLOCK_SHORT "\" "
            "parameter and " EXE_NAME " takes care of submitting the individual parts. When "
            "this option is used, then the AWS S3 client object internally takes care of the "
            "splitting and submission of the individual parts based on the given split size. "
            "For this, \"-" ARG_BLOCK_SHORT "\" has to be set to the full object size, but this "
            "also means each thread needs to allocate the full object size in memory. "
            "(Default: 0=disabled) [Only effective for builds with feature "
            FEATURE_NAME_S3_AWSCRT ".]")
        (ARG_S3MULTIDELETE_LONG, bpo::value(&this->runS3MultiDelObjNum),
            "Delete multiple objects in a single DeleteObjects request. This loops on retrieving "
            "a chunk of objects from a listing request and then deleting the retrieved set of "
            "objects in a single request. This makes no assumption about the retrieved object "
            "names and deletes arbitrary object names in the given bucket(s). The given number is "
            "the maximum number of objects to retrieve and delete in a single request. 1000 is a "
            "typical max value. Use \"--" ARG_S3OBJECTPREFIX_LONG "\" to list/delete only objects "
            "with the given prefix. (Multiple threads will only be effecive if multiple buckets "
            "are given.)")
        (ARG_S3MULTI_IGNORE_404, bpo::bool_switch(&this->s3IgnoreMultipartUpload404),
            "Ignore 404 HTTP error code for multipart upload completions, which can happen if the "
            "CompleteMultipartUpload request has to be retried, e.g. because of a connection "
            "failure. Depending on how the S3 backend handles this, it might return a 404 because "
            "the upload was already completed beforehand. "
            "Enabling this will ignore 404 HTTP errors in CompleteMultiPartUpload responses.")
        (ARG_S3NOCOMPRESS_LONG, bpo::bool_switch(&this->s3NoCompression),
            "Disable S3 request compression.")
        (ARG_S3NOMPCHECK_LONG, bpo::bool_switch(&this->ignoreS3PartNum),
            "Don't check for S3 multi-part uploads exceeding 10,000 parts.")
        (ARG_S3NOMPUCOMPLETION_LONG, bpo::bool_switch(&this->s3NoMpuCompletion),
            "Don't send completion message after uploading all parts of a multi-part upload.")
        (ARG_S3OBJECTPREFIX_LONG, bpo::value(&this->s3ObjectPrefix),
            "S3 object prefix. This will be prepended to all object names when the benchmark path "
            "is a bucket. (A sequence of 3 to 16 \"" RAND_PREFIX_MARKS_SUBSTR "\" chars will be "
            "replaced by a random hex string of the same length.)")
        (ARG_S3OBJLOCKCFG_LONG, bpo::bool_switch(&this->doS3ObjectLockCfg),
            "Activate object lock configuration creation.")
        (ARG_S3OBJLOCKCFGVERIFY_LONG, bpo::bool_switch(&this->doS3ObjectLockCfgVerify),
            "Verify the correctness of object lock configurations.")
        (ARG_S3OBJTAG_LONG, bpo::bool_switch(&this->doS3ObjectTag),
            "Activate S3 object tagging.")
        (ARG_S3OBJTAGVERIFY_LONG, bpo::bool_switch(&this->doS3ObjectTagVerify),
            "Verify the correctness of created S3 object tags.")
        (ARG_S3RANDOBJ_LONG, bpo::bool_switch(&this->useS3RandObjSelect),
            "Read at random offsets and randomly select a new object for each S3 block read. Only "
            "effective in read phase and in combination with \"-" ARG_NUMDIRS_SHORT "\" & \"-"
            ARG_NUMFILES_SHORT "\". Read limit for all threads is defined by \"--"
            ARG_RANDOMAMOUNT_LONG "\".")
        (ARG_S3SSE_LONG, bpo::bool_switch(&this->useS3SSE),
            "Server-side encryption of S3 objects using SSE-S3. (EXPERIMENTAL)")
        (ARG_S3SSECKEY_LONG, bpo::value(&this->s3SSECKey),
            "Base64-encoded AES-256 encryption key for S3 SSE-C.")
        (ARG_S3SSEKMSKEY_LONG, bpo::value(&this->s3SSEKMSKey),
            "Key for S3 SSE-KMS. (EXPERIMENTAL)")
        (ARG_S3REGION_LONG, bpo::value(&this->s3Region),
            "S3 region. (This can also be set via the " S3_ENV_REGION " or "
            S3_ENV_REGION_DEFAULT " env variable.)")
        (ARG_S3ACCESSSECRET_LONG, bpo::value(&this->s3AccessSecret),
            "S3 access secret. (This can also be set via the " S3_ENV_SECRET_KEY " env variable.)")
        (ARG_S3SESSION_TOKEN_LONG, bpo::value(&this->s3SessionToken),
             "S3 session token. (Optional. This can also be set via the " S3_ENV_SESSION_TOKEN
             " env variable.)")
        (ARG_S3CLIENTSINGLETON_LONG, bpo::bool_switch(&this->useS3ClientSingleton),
            "Use a single shared S3 client instance for all threads instead of one S3 client "
            "instance per thread. This is recommended when using the AWS S3 CRT libraries. "
            "With this option, giving multiple S3 endpoints will not be effective. "
            "(Hint: See \"--" ARG_VERSION_LONG "\" output to check if this build is using the AWS "
            "S3 CRT libraries.)")
        (ARG_S3SIGNPAYLOAD_LONG, bpo::value(&this->s3SignPolicy),
            "S3 payload signing policy. 0=RequestDependent, 1=Always, 2=Never. Changing this to "
            "'Never' has no effect with current S3 SDK as described in Github issue 3297. "
            "(Default: 0)")
        (ARG_S3STATDIRS_LONG, bpo::bool_switch(&this->runS3StatDirs),
            "Run bucket attributes query phase.")
        (ARG_S3TROUGHPUTTARGET_LONG, bpo::value(&this->s3ThroughputTargetGbps),
            "The throughput target for each S3 client instance in Gbps (gigabits per second) "
            "based on which the client internally calculates the maximum number of "
            "connections. (Default: 100) [Only effective for builds with feature "
            FEATURE_NAME_S3_AWSCRT ".]")
        (ARG_S3VIRTADDRESSING_LONG, bpo::bool_switch(&this->useS3VirtualAddressing),
            "Use S3 virtual addressing, where the bucket name gets prepended as subdomain of "
            "the S3 server DNS name.")
    ;
#endif // S3_SUPPORT
}

/**
 * Set default values for not provided command line args.
 */
void S3ProgArgs::defineDefaults()
{
    this->doS3AclVerify = false;
    this->doS3AclPutInline = false;
    this->doS3BucketTag = false;
    this->doS3BucketTagVerify = false;
    this->doS3BucketVersioning = false;
    this->doS3BucketVersioningVerify = false;
    this->doS3ListObjVerify = false;
    this->doS3ObjectTag = false;
    this->doS3ObjectTagVerify = false;
    this->doS3ObjectLockCfg = false;
    this->doS3ObjectLockCfgVerify = false;
    this->ignoreS3Errors = false;
    this->ignoreS3PartNum = false;
    this->runS3AclGet = false;
    this->runS3AclPut = false;
    this->runS3BucketAclGet = false;
    this->runS3BucketAclPut = false;
    this->runS3ListObjNum = 0;
    this->runS3ListObjParallel = false;
    this->runS3MPUSharingCompletionPhase = false;
    this->runS3MultiDelObjNum = 0;
    this->runS3StatDirs = false;
    this->s3ChecksumAlgoStr = "";  // Default to empty string (resolved as NOT_SET)
    this->s3CredentialsFile = "";
    this->s3CredentialsList = "";
    this->s3LogLevel = 0;
    this->s3LogfilePrefix = AWS_SDK_LOGPREFIX_DEFAULT;
    this->s3MaxConnections = 0;
    this->s3MpuSizeVariance = 0;
    this->s3MpuSizeVarianceOrigStr = "0";
    this->s3MpuSplitSize = 0;
    this->s3MpuSplitSizeOrigStr = "0";
    this->s3NoCompression = false;
    this->s3NoMpuCompletion = false;
    this->s3IgnoreMultipartUpload404 = false;
    this->s3SessionToken = "";
    this->s3SignPolicy = 0;
    this->s3ThroughputTargetGbps = 100;
    this->useS3ClientSingleton = false;
    this->useS3FastRead = false;
    this->useS3MPUSharing = false;
    this->useS3ObjectPrefixRand = false;
    this->useS3RandObjSelect = false;
    this->useS3SSE = false;
    this->useS3VirtualAddressing = false;
}

/**
 * Initialize implicit values that depend on user config. Called by ProgArgs::initImplicitValues().
 */
void S3ProgArgs::initImplicitValues(ProgArgs& progArgs)
{
    /* if not running as service, read s3 environment variables */
    if( ( (progArgs.benchMode == BenchMode_S3) ||
        StringTk::checkForPrefix(progArgs.treeScanPath, BENCHPATH_PREFIX_S3) ) &&
        !progArgs.runAsService)
    {
        if(s3AccessKey.empty() && (getenv(S3_ENV_ACCESS_KEY) != NULL) )
            s3AccessKey = getenv(S3_ENV_ACCESS_KEY);

        if(s3AccessSecret.empty() && (getenv(S3_ENV_SECRET_KEY) != NULL) )
            s3AccessSecret = getenv(S3_ENV_SECRET_KEY);

        if(s3SessionToken.empty() && (getenv(S3_ENV_SESSION_TOKEN) != NULL) )
            s3SessionToken = getenv(S3_ENV_SESSION_TOKEN);

        if(s3Region.empty() )
        {
            if(getenv(S3_ENV_REGION) && strlen(getenv(S3_ENV_REGION) ) )
                s3Region = getenv(S3_ENV_REGION);
            else
            if(getenv(S3_ENV_REGION_DEFAULT) && strlen(getenv(S3_ENV_REGION_DEFAULT) ) )
                s3Region = getenv(S3_ENV_REGION_DEFAULT);
        }

        if(s3EndpointsStr.empty() )
        {
            if(getenv(S3_ENV_ENDPOINT_URL_S3) && strlen(getenv(S3_ENV_ENDPOINT_URL_S3) ) )
                s3EndpointsStr = getenv(S3_ENV_ENDPOINT_URL_S3);
            else
            if(getenv(S3_ENV_ENDPOINT_URL) && strlen(getenv(S3_ENV_ENDPOINT_URL) ) )
                s3EndpointsStr = getenv(S3_ENV_ENDPOINT_URL);
        }
    }

    // service: check for s3 endpoint override
    if( (progArgs.benchMode == BenchMode_S3) && progArgs.runAsService)
    {
        LOGGER(Log_NORMAL, "NOTE: S3 endpoints given. These will be used instead of any endpoints "
            "provided by master." << std::endl);

        s3EndpointsServiceOverrideStr = s3EndpointsStr;
    }

    if(useS3MPUSharing)
    {
        LOGGER(Log_DEBUG, "Serice MPU sharing mode: Disabling normal S3 MPU completion, setting "
            "share size to '1', enabling block round robin assignment and enabling MPU completion "
            "phase." << std::endl);

        s3NoMpuCompletion = true;
        progArgs.fileShareSize = 1;
        progArgs.fileShareSizeOrigStr = "1";
        progArgs.useCustomTreeRoundRobin = true;
        runS3MPUSharingCompletionPhase = true;
    }

    if(progArgs.argsVariablesMap.count(ARG_S3FASTPUT_LONG) )
    {
        s3SignPolicy = 2; /* Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never */
        s3NoCompression = true;
    }

    if(progArgs.argsVariablesMap.count(ARG_S3CHECKSUM_ALGO_2_LONG) )
    { // this is just an old compat alias
        s3ChecksumAlgoStr = progArgs.argsVariablesMap[ARG_S3CHECKSUM_ALGO_2_LONG].as<std::string>();
    }

    useS3ObjectPrefixRand = (s3ObjectPrefix.find(RAND_PREFIX_MARKS_SUBSTR) != std::string::npos);
}

void S3ProgArgs::convertUnitStrings()
{
    s3MpuSizeVariance = UnitTk::numHumanToBytesBinary(s3MpuSizeVarianceOrigStr, false);
    s3MpuSplitSize = UnitTk::numHumanToBytesBinary(s3MpuSplitSizeOrigStr, false);
}

/**
 * Parse S3 endpoints string to fill s3EndpointsVec. Do nothing if s3 endpoints string is empty.
 *
 * @throw ProgException if a problem is found, e.g. s3 endpoints string was not empty, but parsed
 *      result is empty.
 */
void S3ProgArgs::parseEndpoints(const ProgArgs& progArgs)
{
#ifndef S3_SUPPORT
    if(progArgs.benchMode == BenchMode_S3)
        throw ProgException("S3 mode selected, but built without S3 support.");
#endif // S3_SUPPORT

    if( (progArgs.benchMode != BenchMode_S3) &&
        !StringTk::checkForPrefix(progArgs.treeScanPath, BENCHPATH_PREFIX_S3) )
        return; // nothing to do

    if(!s3EndpointsServiceOverrideStr.empty() && progArgs.runAsService)
        s3EndpointsStr = s3EndpointsServiceOverrideStr; // user specified override for service

    // if we get here then it's either s3 bench mode or s3 treescan or service s3 override...

    // split by given delimiters and expand lists/ranges in square brackets
    TranslatorTk::splitAndExpandStr(s3EndpointsStr, S3ENDPOINTS_DELIMITERS, s3EndpointsVec);

    // delete empty string elements from vec (they come from delimiter use at beginning or end)
    TranslatorTk::eraseEmptyStringsFromVec(s3EndpointsVec);

    if(!s3EndpointsStr.empty() && s3EndpointsVec.empty() )
        throw ProgException("S3 endpoints defined, but parsing resulted in an empty list: " +
            s3EndpointsStr);

    if(s3EndpointsVec.empty() )
        LOGGER(Log_DEBUG, __func__ << ": " <<
            "No S3 endpoint explicitly defined, so relying on AWS profile settings. " << std::endl);
}

/**
 * Infinite loop cannot be combined with S3 shared object upload, so disable object sharing.
 */
void S3ProgArgs::updateFileShareSizeForInfiniteLoop(ProgArgs& progArgs)
{
    /* note: this check/update is here because it has to be after convertPathsToCustomTree()
        (called from parseAndCheckPaths() ) and before loadCustomTreeFile(), so that the update to
        fileShareSize is still effective. */
    if(progArgs.doInfiniteIOLoop && (progArgs.fileShareSize != ~0ULL) &&
        (progArgs.numThreads > 1) && !progArgs.treeFilePath.empty() &&
        (progArgs.benchMode == BenchMode_S3) && progArgs.runCreateFilesPhase)
    {
        LOGGER(Log_NORMAL, "NOTE: Infinite loop cannot be used with S3 shared object "
            "write/upload. Disabling object sharing (\"--" ARG_FILESHARESIZE_LONG "=-1\"). "
            "Consider object-per-thread mode as an alternative "
            "(e.g. \"--" ARG_NUMFILES_LONG "=1\")." << std::endl);

        progArgs.fileShareSize = ~0ULL;
    }
}

/**
 * Check parsed S3 args. Called by ProgArgs::checkArgs() after the custom tree is loaded.
 *
 * @throw ProgException if a problem is found.
 */
void S3ProgArgs::checkArgs(const ProgArgs& progArgs)
{
    if(progArgs.blockSizeMix.isMix() && progArgs.useRandomOffsets &&
        (progArgs.benchMode == BenchMode_S3) && progArgs.runCreateFilesPhase)
        throw ProgException("A block size mix (\"--" ARG_BLOCK_LONG "\") cannot be combined with "
            "random offsets for S3 uploads, because S3 uploads with random offsets implicitly "
            "fall back to reverse sequential access, which does not support a block size mix.");

    if(progArgs.blockSizeMix.isMix() && (progArgs.benchMode == BenchMode_S3) &&
        progArgs.runCreateFilesPhase &&
        (useS3MPUSharing || progArgs.customTree.filesShared.getNumPaths() ) )
        throw ProgException("A block size mix (\"--" ARG_BLOCK_LONG "\") cannot be combined with "
            "S3 shared multipart uploads (\"--" ARG_S3MPUSHARING_LONG "\" or objects large "
            "enough to be split across threads via \"--" ARG_FILESHARESIZE_LONG "\"), because "
            "part numbers cannot be derived from byte offsets when block sizes vary.");

    if(progArgs.useRandomOffsets && (progArgs.benchMode == BenchMode_S3) &&
        progArgs.runCreateFilesPhase)
        LOGGER(Log_NORMAL, "NOTE: S3 write/upload cannot be used with random offsets. "
            "Falling back to \"--" ARG_REVERSESEQOFFSETS_LONG "\"." << std::endl);

    if(progArgs.doInfiniteIOLoop && (progArgs.numThreads > 1) &&
        progArgs.customTree.filesShared.getNumPaths() &&
        (progArgs.benchMode == BenchMode_S3) && progArgs.runCreateFilesPhase)
    {
        /* note: just an additional sanity check after customTree.filesShared has been generated.
            the actual note and auto-disable of object sharing is done in another check before
            customTree.filesShared is generated. */
        throw ProgException("S3 write/upload cannot be used with infinite loop for shared "
            "objects.");
    }

    if(!ignoreS3PartNum && (progArgs.benchMode == BenchMode_S3) && progArgs.fileSize &&
        progArgs.blockSize && progArgs.runCreateFilesPhase &&
        ( (progArgs.fileSize / progArgs.blockSizeMix.getMinSize() ) > 10000) )
        throw ProgException("The specified multi-part upload would exceed 10,000 parts per object. "
            "This exceeds the S3 specification and thus is likely to get rejected by the server. "
            "Consider a smaller object size (\"-" ARG_FILESIZE_SHORT "\") or a larger part block "
            "size (\"-" ARG_BLOCK_SHORT "\"). "
            "The option \"--" ARG_S3NOMPCHECK_LONG "\" disables this check. "
            "Object size: " +  std::to_string(progArgs.fileSize) + "; "
            "Smallest part block size in mix: " +
                std::to_string(progArgs.blockSizeMix.getMinSize() ) + "; "
            "Resulting number of parts: " +
            std::to_string(progArgs.fileSize / progArgs.blockSizeMix.getMinSize() ) );

    S3Transport::checkArgs(progArgs);

    if(progArgs.hasUserSetRWMixPercent() && (progArgs.benchMode == BenchMode_S3) )
        throw ProgException("Option \"--" ARG_RWMIXPERCENT_LONG "\" cannot be used with S3. "
            "Consider \"--" ARG_RWMIXTHREADS_LONG "\" as alternative.");

    if(progArgs.fileOffset && (progArgs.benchMode == BenchMode_S3) && progArgs.runCreateFilesPhase)
        throw ProgException("S3 object upload does not support "
            "\"--" ARG_FILEOFFSET_LONG "\". (Reading a given offset range of existing "
            "objects is supported.)");
}

/**
 * Check credential args. Only for standalone mode, because the master does not need them.
 */
void S3ProgArgs::checkCredentialArgs()
{
    // Check that only one credential source is specified
    if(!s3CredentialsFile.empty() && !s3CredentialsList.empty())
        throw ProgException("Only one of --" ARG_S3CREDFILE_LONG " or --"
            ARG_S3CREDLIST_LONG " may be specified.");

    // If using multi-credentials, s3AccessKey and s3AccessSecret must be empty
    if((!s3CredentialsFile.empty() || !s3CredentialsList.empty()) &&
        (!s3AccessKey.empty() || !s3AccessSecret.empty()))
        throw ProgException("Cannot specify both multi-credentials and single credential options.");
}

/**
 * Called by ProgArgs::checkPathDependentArgs().
 *
 * @throw ProgException if a problem is found.
 */
void S3ProgArgs::checkPathDependentArgs(const ProgArgs& progArgs)
{
    if(runS3ListObjNum && (progArgs.benchPathType != BenchPathType_DIR) )
        throw ProgException("Object listing requires a bucket name as benchmark path.");

    if(runS3ListObjNum && s3EndpointsVec.empty() )
        throw ProgException("Object listing requires S3 endpoints definition.");

    if( (runS3AclPut || runS3AclGet || runS3BucketAclPut || runS3BucketAclGet) &&
        s3EndpointsVec.empty() )
        throw ProgException("Putting/getting bucket or object ACLs requires S3 endpoints "
            "definition.");

    if(!s3EndpointsVec.empty() && s3MpuSizeVariance &&
        (progArgs.doReverseSeqOffsets || progArgs.useRandomOffsets) && progArgs.runCreateFilesPhase)
        throw ProgException("S3 MPU size variance can only be used with sequential upload.");

    if( (progArgs.hasUserSetRWMixPercent() || progArgs.hasUserSetRWMixReadThreads() ) &&
        (progArgs.benchMode == BenchMode_S3) &&
        !progArgs.treeFilePath.empty() )
        throw ProgException("Options \"--" ARG_RWMIXPERCENT_LONG "\" & "
            "\"--" ARG_RWMIXTHREADS_LONG "\" cannot be used with S3 custom tree.");

    if(runS3ListObjNum && !progArgs.treeFilePath.empty() )
        LOGGER(Log_NORMAL, "NOTE: Ignoring custom tree file for object listing." << std::endl);
}

/**
 * If S3 endpoints are given and also paths with slashes, then user wants to do shared uploads/
 * downloads. The logic for this is slightly complicated, especially for shared uploads from
 * multiple hosts. So instead of implementing this logic again, we just convert this to a custom
 * tree file and run this as custom tree mode.
 *
 * This assumes that benchPathsVec has already been set and that the custom tree file will be
 * loaded afterwards.
 *
 * This assumes that all paths use the same bucketName or throws exception otherwise.
 *
 * @throw ProgException on error.
 */
void S3ProgArgs::convertPathsToCustomTree(ProgArgs& progArgs)
{
    // nothing to do if not s3 mode or already a treefile specified by user
    if( (progArgs.benchMode != BenchMode_S3) ||
        !progArgs.treeFilePath.empty() ||
        progArgs.benchPathsVec.empty() )
        return;

    // nothing to do if paths don't contain slashes. (search non-leading slashes, hence pos "1")
    if(progArgs.benchPathsVec[0].find("/", 1) == std::string::npos)
        return;

    LOGGER(Log_VERBOSE,
        "Implicit conversion of given S3 paths to custom tree mode for shared upload/download "
        "support. File: " << S3_IMPLICIT_TREEFILE_PATH << std::endl);

    std::ofstream fileStream(S3_IMPLICIT_TREEFILE_PATH, std::ofstream::trunc);
    if(!fileStream)
        throw ProgException("Opening output tree file failed: " + S3_IMPLICIT_TREEFILE_PATH);

    std::string bucketName;

    for(std::string& currentPath : progArgs.benchPathsVec)
    {
        StringVec currentPathVec;
        std::string currentPathCopy = currentPath;

        // remove multiple leading/trailing slashes
        boost::trim_if(currentPath, boost::is_any_of("/") );

        boost::split(currentPathVec, currentPath, boost::is_any_of("/"), boost::token_compress_on);

        // ensure we have a bucketName and objectName in each user-given path
        if(currentPathVec.size() < 2)
            throw ProgException("Conversion to S3 custom tree mode failed because a path without "
                "elements after slash was found: " + currentPathCopy);

        // ensure all paths have the same bucketName
        if(bucketName.empty() )
            bucketName = currentPathVec[0];
        else
        if(bucketName != currentPathVec[0])
            throw ProgException("Different bucket names are not suppported in this mode. "
                "BucketName1: " + bucketName + "; "
                "BucketName2: " + currentPathVec[0] );

        std::string objectName;

        // re-assemble objectName without bucketName
        for(unsigned i=1; i < currentPathVec.size(); i++)
        {
            if(i > 1)
                objectName += "/";

            objectName += currentPathVec[i];
        }

        // write new line to treefile
        fileStream << PathStore::generateFileLine(objectName, progArgs.fileSize);
    }

    // set bucket name as only path
    progArgs.benchPathsVec.resize(1);
    progArgs.benchPathsVec[0] = bucketName;

    // set treefile path
    progArgs.treeFilePath = S3_IMPLICIT_TREEFILE_PATH;
}

/**
 * Scan an S3 bucket with optional prefix ("s3://mybucket/myprefix") to use instead of providing a
 * treefile. Called by ProgArgs::scanCustomTree().
 */
void S3ProgArgs::scanCustomTree(ProgArgs& progArgs)
{
#ifndef S3_SUPPORT
    throw ProgException("S3 bucket scan requested, but this build does not include S3 support.");
#else

    LOGGER(Log_DEBUG, "Starting S3 tree scan..." << std::endl);

    StringTk::checkAndErasePrefix(progArgs.treeScanPath, BENCHPATH_PREFIX_S3);

    // isolate objectPrefix (if any)

    std::string scanObjectPrefix;

    size_t slashPos = progArgs.treeScanPath.find("/", 1);

    if(slashPos != std::string::npos)
    { // we have a non-trailing slash => extract objectPrefix
        scanObjectPrefix = progArgs.treeScanPath.substr(slashPos); // copy object prefix
        scanObjectPrefix.erase(0, 1); // remove leading slash of object prefix

        progArgs.treeScanPath.erase(slashPos); // remove object prefix from bucket name
    }

    S3Tk::initS3Global(&progArgs);

    std::shared_ptr<S3Client> s3Client = S3Tk::initS3Client(&progArgs);

    S3Tk::scanCustomTree(&progArgs, s3Client, progArgs.treeScanPath, scanObjectPrefix,
        progArgs.treeFilePath);

    s3Client.reset(); // std::shared_ptr, so reset() deletes the s3 client object

#endif // S3_SUPPORT
}

/**
 * Prepare singleton S3 client to be shared by all worker threads. Cleanup is done in
 * resetBenchPath() after all worker threads have finished.
 */
void S3ProgArgs::prepareClientSingleton(ProgArgs& progArgs)
{
#ifdef S3_SUPPORT
    if(s3EndpointsVec.empty() )
        return; // nothing to do

    if(!useS3ClientSingleton)
        return; // nothing to do

    S3Tk::initS3Global(&progArgs);

    // init singleton
    s3ClientSingleton = S3Tk::initS3Client(&progArgs,
        std::chrono::system_clock::now().time_since_epoch().count(),
        &s3IsInterruptionRequested,
        &s3SingletonEndpointStr);

#endif // S3_SUPPORT
}

/**
 * Precreate the s3 mpu upload IDs for shared mpu mode between services.
 *
 * This generates an MPU ID for each file in the treefile, so treeFilePath needs to be processed
 * before calling this.
 */
void S3ProgArgs::precreateMpuSharingUploadIDs(ProgArgs& progArgs)
{
    if(!useS3MPUSharing)
        return; // nothing to do

    if(progArgs.benchMode != BenchMode_S3)
        throw ProgException("S3 MPU sharing mode selected but benchmark mode is not S3.");

    if(!progArgs.runCreateFilesPhase)
        throw ProgException("S3 MPU sharing mode selected but write phase not selected.");

    if(progArgs.hostsVec.empty() )
        throw ProgException("S3 MPU sharing mode selected but no service instances given.");

    if(progArgs.treeFilePath.empty() )
        throw ProgException("S3 MPU sharing mode selected but no shared objects defined.");

    if(progArgs.fileSize <= progArgs.blockSize) // no simple puts in svc mpu sharing mode
        throw ProgException("S3 MPU sharing mode selected but object size is not larger than "
            "part block size");

    // at least one part per thread
    if(progArgs.fileSize < (progArgs.blockSize * progArgs.hostsVec.size() * progArgs.numThreads) )
        throw ProgException("S3 MPU sharing mode selected but object size is less than "
            "one part for each thread. (size < block_size x num_hosts x num_threads)");

    // no non-mpu uploads in svc mpu sharing mode
    if(progArgs.customTree.filesNonShared.getNumPaths() )
        throw ProgException("Found non-shared objects in custom tree. This is not allowed in MPU "
            "sharing mode.");

    if(!progArgs.customTree.filesShared.getNumPaths() ) // mpu sharing requires shared objs
        throw ProgException("Missing shared objects in custom tree. This is not allowed in MPU "
            "sharing mode.");

#ifndef S3_SUPPORT
    throw ProgException("S3 MPU sharing mode selected, but built without S3 support.");
#else

    S3Tk::initS3Global(&progArgs);

    std::shared_ptr<S3Client> s3Client = S3Tk::initS3Client(&progArgs);

    const PathList& pathList = progArgs.customTree.filesShared.getPaths();

    S3Tk::precreateMpuIDs(&progArgs, s3Client, progArgs.benchPathsVec[0], s3ObjectPrefix,
        pathList, s3MpuSharingUploadIDs);

    s3Client.reset(); // std::shared_ptr, so reset() deletes the s3 client object

    if(s3MpuSharingUploadIDs.empty() )
        throw ProgException("S3 MPU IDs list is empty after precreation step.");

    std::ofstream fileStream(S3_IMPLICIT_MPUSHAING_PATH, std::ofstream::trunc);
    if(!fileStream)
        throw ProgException("Opening output MPU IDs file failed: " + S3_IMPLICIT_MPUSHAING_PATH);

    for(const std::string& mpuID : s3MpuSharingUploadIDs)
        fileStream << mpuID << std::endl;

#endif // S3_SUPPORT
}

void S3ProgArgs::printHelp()
{
    std::cout <<
        "S3 object storage testing. (The options here are intentionally similar to" ENDL
        "\"--" ARG_HELPMULTIFILE_LONG "\" to enable multi-protocol storage testing.)" ENDL
        std::endl <<
        "Usage: ./" EXE_NAME " [OPTIONS] BUCKET [MORE_BUCKETS]" ENDL
        std::endl;

    bpo::options_description argsS3ServiceArgsDescription(
        "S3 Service Arguments", TerminalTk::getTerminalLineLength(80) );

    argsS3ServiceArgsDescription.add_options()
        (ARG_S3CREDFILE_LONG, bpo::value<std::string>(),
            "Path to file containing multiple S3 credentials. Each line in format: "
            "access_key:secret_key. Lines starting with # are treated as comments.")
        (ARG_S3CREDLIST_LONG, bpo::value<std::string>(),
            "Comma-separated list of S3 credentials. Each credential in format: "
            "access_key:secret_key")
        (ARG_S3ENDPOINTS_LONG, bpo::value<std::string>(),
            "Comma-separated list of S3 endpoints. (Format: [http(s)://]hostname[:port]) "
            "(This can also be set via the " S3_ENV_ENDPOINT_URL_S3 " or "
            S3_ENV_ENDPOINT_URL " env variable.)")
        (ARG_S3ACCESSKEY_LONG, bpo::value<std::string>(),
            "S3 access key. (This can also be set via the " S3_ENV_ACCESS_KEY " env variable.)")
        (ARG_S3ACCESSSECRET_LONG, bpo::value<std::string>(),
            "S3 access secret. (This can also be set via the " S3_ENV_SECRET_KEY " env variable.)")
        (ARG_S3SESSION_TOKEN_LONG, bpo::value<std::string>(),
            "S3 session token. (Optional. This can also be set via the " S3_ENV_SESSION_TOKEN
            " env variable.)")
    ;

    std::cout << argsS3ServiceArgsDescription << std::endl;

    bpo::options_description argsS3BasicDescription(
        "Basic Options", TerminalTk::getTerminalLineLength(80) );

    argsS3BasicDescription.add_options()
        (ARG_CREATEDIRS_LONG "," ARG_CREATEDIRS_SHORT, bpo::bool_switch(),
            "Create buckets. (Already existing buckets are not treated as error.)")
        (ARG_CREATEFILES_LONG "," ARG_CREATEFILES_SHORT,
            bpo::bool_switch(),
            "Write/upload objects.")
        (ARG_READ_LONG "," ARG_READ_SHORT, bpo::bool_switch(),
            "Read/download objects.")
        (ARG_STATFILES_LONG, bpo::bool_switch(),
            "Read object status attributes (size etc).")
        (ARG_DELETEFILES_LONG "," ARG_DELETEFILES_SHORT,
            bpo::bool_switch(),
            "Delete objects.")
        (ARG_DELETEDIRS_LONG "," ARG_DELETEDIRS_SHORT, bpo::bool_switch(),
            "Delete buckets.")
        (ARG_NUMTHREADS_LONG "," ARG_NUMTHREADS_SHORT, bpo::value<size_t>(),
            "Number of I/O worker threads. (Default: 1)")
        (ARG_NUMDIRS_LONG "," ARG_NUMDIRS_SHORT, bpo::value<size_t>(),
            "Number of directories per I/O worker thread. Directories are slash-separated object "
            "key prefixes. This can be 0 to disable creation of any subdirs. (Default: 1)")
        (ARG_NUMFILES_LONG "," ARG_NUMFILES_SHORT, bpo::value<std::string>(),
            "Number of objects per thread per directory. (Default: 1) Example: \""
            "-" ARG_NUMTHREADS_SHORT "2 -" ARG_NUMDIRS_SHORT "3 -" ARG_NUMFILES_SHORT "4\" will "
            "use 2x3x4=24 objects.")
        (ARG_FILESIZE_LONG "," ARG_FILESIZE_SHORT, bpo::value<size_t>(),
            "Object size. (Default: 0)")
        (ARG_BLOCK_LONG "," ARG_BLOCK_SHORT, bpo::value<size_t>(),
            "The part block size for uploads and the ranged read size for downloads. Multipart "
            "upload will automatically be used if object size is larger than part block size. "
            "Each thread needs to keep one block in RAM (or multiple blocks if "
            "\"--" ARG_IODEPTH_LONG "\" is used), so be careful with large block sizes. "
            "(Default: 1M)")
    ;

    std::cout << argsS3BasicDescription << std::endl;

    bpo::options_description argsS3FrequentDescription(
        "Frequently Used Options", TerminalTk::getTerminalLineLength(80) );

    argsS3FrequentDescription.add_options()
        (ARG_S3FASTGET_LONG, bpo::bool_switch(),
            "Send downloaded objects directly to /dev/null instead of a memory buffer. This option "
            "is incompatible with any buffer post-processing options like data verification or "
            "GPU data transfer.")
        (ARG_TREEFILE_LONG, bpo::value<std::string>(),
            "The path to a treefile containing a list of object names to use for shared upload or "
            "download if the object size exceeds \"--" ARG_FILESHARESIZE_LONG "\".")
        (ARG_LATENCY_LONG, bpo::bool_switch(),
            "Show minimum, average and maximum latency for PUT/GET operations and for complete "
            "upload/download in case of chunked transfers.")
    ;

    std::cout << argsS3FrequentDescription << std::endl;

    bpo::options_description argsS3MiscDescription(
        "Miscellaneous Options", TerminalTk::getTerminalLineLength(80) );

    argsS3MiscDescription.add_options()
        (ARG_FILESHARESIZE_LONG, bpo::value<std::string>(),
            "In custom tree mode or when object keys are given directly as arguments, this defines "
            "the object size as of which multiple threads are used to upload/download an object. "
            "(Default: 0, which means " FILESHAREBLOCKFACTOR_STR " x blocksize)")
        (ARG_S3REGION_LONG, bpo::value<std::string>(),
            "S3 region. (This can also be set via the " S3_ENV_REGION " or "
            S3_ENV_REGION_DEFAULT " env variable.)")
        (ARG_NUMAZONES_LONG, bpo::value<std::string>(),
            "Comma-separated list of NUMA zones to bind this process to. If multiple zones are "
            "given, then worker threads are bound round-robin to the zones. "
            "(Hint: See 'lscpu' for available NUMA zones.)")
    ;

    std::cout << argsS3MiscDescription << std::endl;

    std::cout <<
        "Examples:" ENDL
        "  Create bucket \"mybucket\":" ENDL
        "    $ " EXE_NAME " --s3endpoints http://S3SERVER --s3key S3KEY --s3secret S3SECRET \\" ENDL
        "        -d s3://mybucket" ENDL
        std::endl <<
        "  Test 2 threads, each creating 3 directories with 4 10MiB objects inside:" ENDL
        "    $ " EXE_NAME " --s3endpoints http://S3SERVER --s3key S3KEY --s3secret S3SECRET \\" ENDL
        "        -w -t 2 -n 3 -N 4 -s 10m -b 5m s3://mybucket" ENDL
        std::endl <<
        "  Delete objects and bucket created by example above:" ENDL
        "    $ " EXE_NAME " --s3endpoints http://S3SERVER --s3key S3KEY --s3secret S3SECRET \\" ENDL
        "        -D -F -t 2 -n 3 -N 4 s3://mybucket" ENDL
        std::endl <<
        "  Shared upload of 4 1GiB objects via 8 threads in 16MiB blocks:" ENDL
        "    $ " EXE_NAME " --s3endpoints http://S3SERVER --s3key S3KEY --s3secret S3SECRET \\" ENDL
        "        -w -t 8 -s 1g -b 16m \"s3://mybucket/myobject[1-4]\"" <<
        std::endl;
}

void S3ProgArgs::printBuildInfo(std::ostringstream& includedStream,
    std::ostringstream& notIncludedStream)
{
#if defined(S3_SUPPORT) && defined(S3_AWSCRT)
    includedStream << FEATURE_NAME_S3_SUPPORT << " ";
    includedStream << FEATURE_NAME_S3_AWSCRT << " ";
#elif defined(S3_SUPPORT)
    includedStream << FEATURE_NAME_S3_SUPPORT << " ";
    notIncludedStream << FEATURE_NAME_S3_AWSCRT << " ";
#else
    notIncludedStream << FEATURE_NAME_S3_SUPPORT << " ";
    notIncludedStream << FEATURE_NAME_S3_AWSCRT << " ";
#endif
}

/**
 * Sets the S3 arguments for a new benchmark in service mode.
 */
void S3ProgArgs::setFromPropertyTree(const bpt::ptree& tree)
{
    doS3AclPutInline = tree.get<bool>(ARG_S3ACLPUTINLINE_LONG);
    doS3AclVerify = tree.get<bool>(ARG_S3ACLVERIFY_LONG);
    doS3ListObjVerify = tree.get<bool>(ARG_S3LISTOBJVERIFY_LONG);
    doS3BucketTag = tree.get<bool>(ARG_S3BUCKETTAG_LONG);
    doS3BucketTagVerify = tree.get<bool>(ARG_S3BUCKETTAGVERIFY_LONG);
    doS3BucketVersioning = tree.get<bool>(ARG_S3BUCKETVER_LONG);
    doS3BucketVersioningVerify = tree.get<bool>(ARG_S3BUCKETVERVERIFY_LONG);
    doS3ObjectTag = tree.get<bool>(ARG_S3OBJTAG_LONG);
    doS3ObjectTagVerify = tree.get<bool>(ARG_S3OBJTAGVERIFY_LONG);
    doS3ObjectLockCfg = tree.get<bool>(ARG_S3OBJLOCKCFG_LONG);
    doS3ObjectLockCfgVerify = tree.get<bool>(ARG_S3OBJLOCKCFGVERIFY_LONG);
    ignoreS3Errors = tree.get<bool>(ARG_S3IGNOREERRORS_LONG);
    runS3AclGet = tree.get<bool>(ARG_S3ACLGET_LONG);
    runS3AclPut = tree.get<bool>(ARG_S3ACLPUT_LONG);
    runS3BucketAclGet = tree.get<bool>(ARG_S3BUCKETACLGET_LONG);
    runS3BucketAclPut = tree.get<bool>(ARG_S3BUCKETACLPUT_LONG);
    runS3ListObjNum = tree.get<uint64_t>(ARG_S3LISTOBJ_LONG);
    runS3ListObjParallel = tree.get<bool>(ARG_S3LISTOBJPARALLEL_LONG);
    runS3MPUSharingCompletionPhase = tree.get<bool>(ARG_S3MPUSHARINGCOMPL_LONG);
    runS3MultiDelObjNum = tree.get<uint64_t>(ARG_S3MULTIDELETE_LONG);
    s3IgnoreMultipartUpload404 = tree.get<bool>(ARG_S3MULTI_IGNORE_404);
    runS3StatDirs = tree.get<bool>(ARG_S3STATDIRS_LONG);
    s3AccessKey = tree.get<std::string>(ARG_S3ACCESSKEY_LONG);
    s3AccessSecret = tree.get<std::string>(ARG_S3ACCESSSECRET_LONG);
    s3ChecksumAlgoStr = tree.get<std::string>(ARG_S3CHECKSUM_ALGO_LONG);
    s3AclGrantee = tree.get<std::string>(ARG_S3ACLGRANTEE_LONG);
    s3AclGranteePermissions = tree.get<std::string>(ARG_S3ACLGRANTS_LONG);
    s3AclGranteeType = tree.get<std::string>(ARG_S3ACLGRANTEETYPE_LONG);
    s3CredentialsFile = tree.get<std::string>(ARG_S3CREDFILE_LONG);
    s3CredentialsList = tree.get<std::string>(ARG_S3CREDLIST_LONG);
    s3EndpointsStr = tree.get<std::string>(ARG_S3ENDPOINTS_LONG);
    s3MaxConnections = tree.get<unsigned>(ARG_S3MAXCONNS_LONG);
    s3MpuSizeVariance = tree.get<size_t>(ARG_S3MPUSIZEVAR_LONG);
    s3MpuSplitSize = tree.get<size_t>(ARG_S3MPUSPLITSIZE_LONG);
    s3NoCompression = tree.get<bool>(ARG_S3NOCOMPRESS_LONG);
    s3NoMpuCompletion = tree.get<bool>(ARG_S3NOMPUCOMPLETION_LONG);
    s3ObjectPrefix = tree.get<std::string>(ARG_S3OBJECTPREFIX_LONG);
    s3Region = tree.get<std::string>(ARG_S3REGION_LONG);
    s3SessionToken = tree.get<std::string>(ARG_S3SESSION_TOKEN_LONG);
    s3SignPolicy = tree.get<unsigned short>(ARG_S3SIGNPAYLOAD_LONG);
    s3SSECKey = tree.get<std::string>(ARG_S3SSECKEY_LONG);
    s3SSEKMSKey = tree.get<std::string>(ARG_S3SSEKMSKEY_LONG);
    s3ThroughputTargetGbps = tree.get<unsigned>(ARG_S3TROUGHPUTTARGET_LONG);
    useS3ClientSingleton = tree.get<bool>(ARG_S3CLIENTSINGLETON_LONG);
    useS3FastRead = tree.get<bool>(ARG_S3FASTGET_LONG);
    useS3MPUSharing = tree.get<bool>(ARG_S3MPUSHARING_LONG);
    useS3RandObjSelect = tree.get<bool>(ARG_S3RANDOBJ_LONG);
    useS3SSE = tree.get<bool>(ARG_S3SSE_LONG);
    useS3VirtualAddressing = tree.get<bool>(ARG_S3VIRTADDRESSING_LONG);

    useS3ObjectPrefixRand = (s3ObjectPrefix.find(RAND_PREFIX_MARKS_SUBSTR) != std::string::npos);
}

/**
 * Gets the S3 arguments for a new benchmark in service mode.
 */
void S3ProgArgs::getAsPropertyTree(bpt::ptree& outTree) const
{
    outTree.put(ARG_S3ACCESSKEY_LONG, s3AccessKey);
    outTree.put(ARG_S3ACCESSSECRET_LONG, s3AccessSecret);
    outTree.put(ARG_S3ACLGET_LONG, runS3AclGet);
    outTree.put(ARG_S3ACLGRANTEE_LONG, s3AclGrantee);
    outTree.put(ARG_S3ACLGRANTEETYPE_LONG, s3AclGranteeType);
    outTree.put(ARG_S3ACLGRANTS_LONG, s3AclGranteePermissions);
    outTree.put(ARG_S3ACLPUT_LONG, runS3AclPut);
    outTree.put(ARG_S3ACLPUTINLINE_LONG, doS3AclPutInline);
    outTree.put(ARG_S3ACLVERIFY_LONG, doS3AclVerify);
    outTree.put(ARG_S3BUCKETACLGET_LONG, runS3BucketAclGet);
    outTree.put(ARG_S3BUCKETACLPUT_LONG, runS3BucketAclPut);
    outTree.put(ARG_S3BUCKETTAG_LONG, doS3BucketTag);
    outTree.put(ARG_S3BUCKETTAGVERIFY_LONG, doS3BucketTagVerify);
    outTree.put(ARG_S3BUCKETVER_LONG, doS3BucketVersioning);
    outTree.put(ARG_S3BUCKETVERVERIFY_LONG, doS3BucketVersioningVerify);
    outTree.put(ARG_S3CHECKSUM_ALGO_LONG, s3ChecksumAlgoStr);
    outTree.put(ARG_S3CLIENTSINGLETON_LONG, useS3ClientSingleton);
    outTree.put(ARG_S3CREDFILE_LONG, s3CredentialsFile);
    outTree.put(ARG_S3CREDLIST_LONG, s3CredentialsList);
    outTree.put(ARG_S3ENDPOINTS_LONG, s3EndpointsStr);
    outTree.put(ARG_S3FASTGET_LONG, useS3FastRead);
    outTree.put(ARG_S3IGNOREERRORS_LONG, ignoreS3Errors);
    outTree.put(ARG_S3LISTOBJ_LONG, runS3ListObjNum);
    outTree.put(ARG_S3LISTOBJPARALLEL_LONG, runS3ListObjParallel);
    outTree.put(ARG_S3LISTOBJVERIFY_LONG, doS3ListObjVerify);
    outTree.put(ARG_S3MAXCONNS_LONG, s3MaxConnections);
    outTree.put(ARG_S3MPUSHARING_LONG, useS3MPUSharing);
    outTree.put(ARG_S3MPUSHARINGCOMPL_LONG, runS3MPUSharingCompletionPhase);
    outTree.put(ARG_S3MPUSIZEVAR_LONG, s3MpuSizeVariance);
    outTree.put(ARG_S3MPUSPLITSIZE_LONG, s3MpuSplitSize);
    outTree.put(ARG_S3MULTIDELETE_LONG, runS3MultiDelObjNum);
    outTree.put(ARG_S3MULTI_IGNORE_404, s3IgnoreMultipartUpload404);
    outTree.put(ARG_S3NOCOMPRESS_LONG, s3NoCompression);
    outTree.put(ARG_S3NOMPUCOMPLETION_LONG, s3NoMpuCompletion);
    outTree.put(ARG_S3OBJECTPREFIX_LONG, s3ObjectPrefix);
    outTree.put(ARG_S3OBJLOCKCFG_LONG, doS3ObjectLockCfg);
    outTree.put(ARG_S3OBJLOCKCFGVERIFY_LONG, doS3ObjectLockCfgVerify);
    outTree.put(ARG_S3OBJTAG_LONG, doS3ObjectTag);
    outTree.put(ARG_S3OBJTAGVERIFY_LONG, doS3ObjectTagVerify);
    outTree.put(ARG_S3RANDOBJ_LONG, useS3RandObjSelect);
    outTree.put(ARG_S3REGION_LONG, s3Region);
    outTree.put(ARG_S3SESSION_TOKEN_LONG, s3SessionToken);
    outTree.put(ARG_S3SIGNPAYLOAD_LONG, s3SignPolicy);
    outTree.put(ARG_S3SSE_LONG, useS3SSE);
    outTree.put(ARG_S3SSECKEY_LONG, s3SSECKey);
    outTree.put(ARG_S3SSEKMSKEY_LONG, s3SSEKMSKey);
    outTree.put(ARG_S3STATDIRS_LONG, runS3StatDirs);
    outTree.put(ARG_S3TROUGHPUTTARGET_LONG, s3ThroughputTargetGbps);
    outTree.put(ARG_S3VIRTADDRESSING_LONG, useS3VirtualAddressing);
}

/**
 * Release the client singleton and derived values. Called by ProgArgs::resetBenchPath().
 */
void S3ProgArgs::reset()
{
#ifdef S3_SUPPORT
    s3ClientSingleton.reset();
    s3IsInterruptionRequested = false;
    s3SingletonEndpointStr.clear();
#endif // S3_SUPPORT

    s3EndpointsVec.clear();
}
