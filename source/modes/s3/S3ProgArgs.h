// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef MODES_S3_S3PROGARGS_H_
#define MODES_S3_S3PROGARGS_H_

#include <atomic>
#include <boost/program_options.hpp>
#include <boost/property_tree/ptree.hpp>
#include <memory>
#include <sstream>
#include <string>

#include "Common.h"
#include "modes/s3/toolkits/S3Tk.h"

namespace bpo = boost::program_options;
namespace bpt = boost::property_tree;

class ProgArgs;

#define ARG_S3ACCESSKEY_LONG             "s3key"
#define ARG_S3ACCESSSECRET_LONG          "s3secret"
#define ARG_S3ACLGET_LONG                "s3aclget"
#define ARG_S3ACLGRANTEE_LONG            "s3aclgrantee"
#define ARG_S3ACLGRANTEETYPE_LONG        "s3aclgtype"
#define ARG_S3ACLGRANTS_LONG             "s3aclgrants"
#define ARG_S3ACLPUT_LONG                "s3aclput"
#define ARG_S3ACLPUTINLINE_LONG          "s3aclputinl"
#define ARG_S3ACLVERIFY_LONG             "s3aclverify"
#define ARG_S3BUCKETACLGET_LONG          "s3baclget"
#define ARG_S3BUCKETACLPUT_LONG          "s3baclput"
#define ARG_S3BUCKETTAG_LONG             "s3btag"
#define ARG_S3BUCKETTAGVERIFY_LONG       "s3btagverify"
#define ARG_S3BUCKETVER_LONG             "s3bversion"
#define ARG_S3BUCKETVERVERIFY_LONG       "s3bversionverify"
#define ARG_S3CLIENTSINGLETON_LONG       "s3single"
#define ARG_S3CREDFILE_LONG              "s3credfile"
#define ARG_S3CREDLIST_LONG              "s3credlist"
#define ARG_S3ENDPOINTS_LONG             "s3endpoints"
#define ARG_S3FASTGET_LONG               "s3fastget"
#define ARG_S3FASTPUT_LONG               "s3fastput"
#define ARG_S3IGNOREERRORS_LONG          "s3ignoreerrors"
#define ARG_S3LISTOBJ_LONG               "s3listobj"
#define ARG_S3LISTOBJPARALLEL_LONG       "s3listobjpar"
#define ARG_S3LISTOBJVERIFY_LONG         "s3listverify"
#define ARG_S3LOGFILEPREFIX_LONG         "s3logprefix"
#define ARG_S3LOGLEVEL_LONG              "s3log"
#define ARG_S3MAXCONNS_LONG              "s3maxconns"
#define ARG_S3MPUSIZEVAR_LONG            "s3mpusizevar"
#define ARG_S3MPUSPLITSIZE_LONG          "s3mpusplit"
#define ARG_S3MPUSHARING_LONG            "s3mpusharing"
#define ARG_S3MPUSHARINGCOMPL_LONG       "s3mpucomplphase" // implicitly set
#define ARG_S3MULTIDELETE_LONG           "s3multidel"
#define ARG_S3MULTI_IGNORE_404           "s3multiignore404"
#define ARG_S3NOCOMPRESS_LONG            "s3nocompress"
#define ARG_S3NOMPCHECK_LONG             "s3nompcheck"
#define ARG_S3NOMPUCOMPLETION_LONG       "s3nompucompl"
#define ARG_S3OBJECTPREFIX_LONG          "s3objprefix"
#define ARG_S3OBJLOCKCFG_LONG            "s3olockcfg"
#define ARG_S3OBJLOCKCFGVERIFY_LONG      "s3olockcfgverify"
#define ARG_S3OBJTAG_LONG                "s3otag"
#define ARG_S3OBJTAGVERIFY_LONG          "s3otagverify"
#define ARG_S3RANDOBJ_LONG               "s3randobj"
#define ARG_S3REGION_LONG                "s3region"
#define ARG_S3SESSION_TOKEN_LONG         "s3sessiontoken"
#define ARG_S3SIGNPAYLOAD_LONG           "s3sign"
#define ARG_S3SSE_LONG                   "s3sse"
#define ARG_S3SSECKEY_LONG               "s3sseckey"
#define ARG_S3CHECKSUM_ALGO_2_LONG       "s3checksumalgo" // compat alias (too long name)
#define ARG_S3CHECKSUM_ALGO_LONG         "s3chksumalgo" // for x-amz-sdk-checksum-algorithm header
#define ARG_S3SSEKMSKEY_LONG             "s3ssekmskey"
#define ARG_S3STATDIRS_LONG              "s3statdirs"
#define ARG_S3TROUGHPUTTARGET_LONG       "s3targetgbps"
#define ARG_S3VIRTADDRESSING_LONG        "s3virtaddr"

/* permission flags for S3 ACLs.
    note: std::string::find() will be used with these, so make sure each name is unambiguous and not
    a substring of another name. */
#define ARG_S3ACL_PERM_NONE_NAME            "none"
#define ARG_S3ACL_PERM_FULL_NAME            "full"
#define ARG_S3ACL_PERM_FLAG_READ_NAME       "read"
#define ARG_S3ACL_PERM_FLAG_WRITE_NAME      "write"
#define ARG_S3ACL_PERM_FLAG_READACP_NAME    "racp"
#define ARG_S3ACL_PERM_FLAG_WRITEACP_NAME   "wacp"

// grantee type for S3 ACLs
#define ARG_S3ACL_GRANTEE_TYPE_ID           "id"
#define ARG_S3ACL_GRANTEE_TYPE_EMAIL        "email"
#define ARG_S3ACL_GRANTEE_TYPE_URI          "uri"
#define ARG_S3ACL_GRANTEE_TYPE_GROUP        "group"


/**
 * The S3 options of ProgArgs: definition, defaults, checks, help, service transfer and the
 * shared S3 client singleton. ProgArgs owns one instance and calls the methods below at the
 * matching points of its own initialization sequence.
 */
class S3ProgArgs
{
    public:
        void defineArgs(bpo::options_description& generic, bpo::options_description& hidden);
        void defineDefaults();
        void initImplicitValues(ProgArgs& progArgs);
        void convertUnitStrings();
        void parseEndpoints(const ProgArgs& progArgs);
        void updateFileShareSizeForInfiniteLoop(ProgArgs& progArgs);
        void checkArgs(const ProgArgs& progArgs);
        void checkCredentialArgs();
        void checkPathDependentArgs(const ProgArgs& progArgs);
        void convertPathsToCustomTree(ProgArgs& progArgs);
        void scanCustomTree(ProgArgs& progArgs);
        void prepareClientSingleton(ProgArgs& progArgs);
        void precreateMpuSharingUploadIDs(ProgArgs& progArgs);
        void printHelp();
        void printBuildInfo(std::ostringstream& includedStream,
            std::ostringstream& notIncludedStream);
        void setFromPropertyTree(const bpt::ptree& tree);
        void getAsPropertyTree(bpt::ptree& outTree) const;
        void reset();

    private:
#ifdef S3_SUPPORT
        std::shared_ptr<S3Client> s3ClientSingleton; // shared singleton s3 client for workers
        std::atomic_bool s3IsInterruptionRequested{false}; // interrupt for s3 singleton lambdas
        std::string s3SingletonEndpointStr; // endpoint string for singleton s3 client
        StringVec s3MpuSharingUploadIDs; // ProgArgs precreated MPU IDs for mpu sharing mode
#endif // S3_SUPPORT

        // config options in alphabetic order...

        bool doS3AclPutInline; // set object acl during PutObject
        bool doS3AclVerify; // verify that acl contains given grantee and permissions
        bool doS3ListObjVerify; // verify object listing (requires "-n" / "-N")
        bool doS3BucketVersioning;  // allow to toggle bucket versioning
        bool doS3BucketVersioningVerify;  // verify that the correct versioning status was set
        bool doS3BucketTag; // add bucket tagging ops during different bucket operations
        bool doS3BucketTagVerify; // do bucket tagging verification.
        bool doS3ObjectTag; // add object tagging ops during different object operations
        bool doS3ObjectTagVerify; // do bucket tagging verification.
        bool doS3ObjectLockCfg; // do S3 object lock configuration
        bool doS3ObjectLockCfgVerify; // do S3 object lock configuration verification
        bool ignoreS3Errors; // ignore S3 get/put errors, useful for stress-testing
        bool ignoreS3PartNum; // don't check for >10K parts in multi-part uploads
        bool runS3AclGet; // retrieve object acl
        bool runS3AclPut; // change object acl
        bool runS3BucketAclGet; // retrieve bucket acl
        bool runS3BucketAclPut; // change bucket acl
        bool runS3StatDirs; // HeadBucket (and other bucket MD ops, goes well with doS3BucketTag)
        uint64_t runS3ListObjNum; // run seq list objects phase if >0, given number is listing limit
        bool runS3ListObjParallel; // multi-threaded object listing (requires "-n" / "-N")
        bool runS3MPUSharingCompletionPhase; // run separate mpu compl phase after svc mpu sharing
        uint64_t runS3MultiDelObjNum; // run S3 multi del phase if >0; number is multi del limit
        std::string s3AccessKey; // s3 access key
        std::string s3AccessSecret; // s3 access secret
        std::string s3AclGrantee; // s3 acl grantee
        std::string s3AclGranteeType; // s3 acl grantee type
        std::string s3AclGranteePermissions; // s3 acl grantee permission flags (ARG_S3_ACL_...)
        std::string s3CredentialsFile; // path to file containing multiple S3 credentials
        std::string s3CredentialsList; // comma-separated list of S3 credentials
        std::string s3EndpointsServiceOverrideStr; // override of s3EndpointStr in service mode
        StringVec s3EndpointsVec; // s3 endpoints broken down into individual elements
        std::string s3EndpointsStr; // user-given s3 endpoints; elem format: [http(s)://]host[:port]
        bool s3IgnoreMultipartUpload404; // Ignore 404 on retries of MPU completion
        std::string s3LogfilePrefix; // dir and name prefix of aws sdk log file
        unsigned short s3LogLevel; // log level for AWS SDK
        unsigned s3MaxConnections; // max conns per s3 client instance (not eff. for S3CrtClient)
        size_t s3MpuSizeVariance; // random subtract variance in bytes for part sizes of MPU
        std::string s3MpuSizeVarianceOrigStr; // original s3MpuSizeVariance str from user with unit
        size_t s3MpuSplitSize; // mpu split size by client instead of by blockSize
        std::string s3MpuSplitSizeOrigStr; // original s3MpuSplitSize str from user with unit
        bool s3NoCompression; // disable request compression of aws sdk cpp
        bool s3NoMpuCompletion; // don't send finalizing multi-part upload completion message
        std::string s3ObjectPrefix; // object name/path prefix for s3 "directory mode"
        std::string s3Region; // s3 region
        std::string s3SessionToken; // s3 session token (same as secret token)
        unsigned short s3SignPolicy; /* Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy; note:
            "2=never" is ignored, because as of aws sdk cpp v1.11.486 signing is always done. */
        std::string s3SSECKey;  // S3 SSE-C key for encryption
        std::string s3SSEKMSKey;  // S3 SSE-KMS key for encryption
        unsigned s3ThroughputTargetGbps; // S3CrtClient throughput target for number of conns (Gbps)
        bool useS3ClientSingleton; // use singleton S3 client for all threads
        bool useS3MPUSharing; // use s3 shared mpu mode from multiple clients
        bool useS3ObjectPrefixRand; // implicit based on RAND_PREFIX_MARKS_SUBSTR in s3ObjectPrefix
        bool useS3RandObjSelect; // random object selection for each read
        bool useS3FastRead; /* get objects to /dev/null instead of buffer (i.e. no post processing
                                via buffer possible, such as GPU copy or data verification) */
        bool useS3SSE; // use SSE-S3 encryption method for S3
        bool useS3VirtualAddressing; // true to use virtual addressing for S3
        std::string s3ChecksumAlgoStr;  /* Stores the S3 checksum algorithm value (e.g. "CRC32",
                                            "CRC32C", "SHA1", "SHA256") */

    // inliners
    public:
        // methods related to shared s3 client singleton for workers
#ifdef S3_SUPPORT
        std::shared_ptr<S3Client> getS3ClientSingleton() const { return s3ClientSingleton; }
        void setS3InterruptionRequested() { s3IsInterruptionRequested = true; }
        std::string getS3SingletonEndpointStr() const { return s3SingletonEndpointStr; }
#else // !S3_SUPPORT
        void setS3InterruptionRequested() { /* no-op */ }
#endif // S3_SUPPORT

        // getters in alphabetic order...

        bool getS3BucketMetadataRequested() const
            { return doS3BucketTag || doS3ObjectLockCfg || doS3BucketVersioning; }
        bool getS3ObjectMetadataRequested() const { return doS3ObjectTag; }
        bool getDoS3BucketVersioning() const { return doS3BucketVersioning; }
        bool getDoS3BucketVersioningVerify() const { return doS3BucketVersioningVerify; }
        bool getDoS3BucketTagging() const { return doS3BucketTag; }
        bool getDoS3BucketTaggingVerify() const { return doS3BucketTagVerify; }
        bool getDoS3ObjectTagging() const { return doS3ObjectTag; }
        bool getDoS3ObjectTaggingVerify() const { return doS3ObjectTagVerify; }
        bool getDoS3ObjectLockConfiguration() const { return doS3ObjectLockCfg; }
        bool getDoS3ObjectLockConfigurationVerify() const { return doS3ObjectLockCfgVerify; }
        bool getDoS3AclPutInline() const { return doS3AclPutInline; }
        bool getDoS3AclVerify() const { return doS3AclVerify; }
        bool getDoListObjVerify() const { return doS3ListObjVerify; }
        bool getIgnoreS3Errors() const { return ignoreS3Errors; }
        bool getIgnoreS3PartNum() const { return ignoreS3PartNum; }
        bool getRunListObjParallelPhase() const { return runS3ListObjParallel; }
        bool getRunS3MPUSharingCompletionPhase() const { return runS3MPUSharingCompletionPhase; }
        bool getRunListObjPhase() const { return (runS3ListObjNum > 0); }
        bool getRunMultiDelObjPhase() const { return (runS3MultiDelObjNum > 0); }
        bool getRunS3AclPut() const { return runS3AclPut; }
        bool getRunS3AclGet() const { return runS3AclGet; }
        bool getRunS3BucketAclPut() const { return runS3BucketAclPut; }
        bool getRunS3BucketAclGet() const { return runS3BucketAclGet; }
        bool getRunS3StatDirs() const { return runS3StatDirs; }
        std::string getS3AccessKey() const { return s3AccessKey; }
        std::string getS3AccessSecret() const { return s3AccessSecret; }
        std::string getS3AclGrantee() const { return s3AclGrantee; }
        std::string getS3AclGranteeType() const { return s3AclGranteeType; }
        std::string getS3AclGranteePermissions() const { return s3AclGranteePermissions; }
        std::string getS3CredentialsFile() const { return s3CredentialsFile; }
        std::string getS3CredentialsList() const { return s3CredentialsList; }
        std::string getS3EndpointsServiceOverride() const { return s3EndpointsServiceOverrideStr; }
        std::string getS3EndpointsStr() const { return s3EndpointsStr; }
        const StringVec& getS3EndpointsVec() const { return s3EndpointsVec; }
        bool getS3IgnoreMultipartUpload404() const { return s3IgnoreMultipartUpload404; }
        uint64_t getS3ListObjNum() const { return runS3ListObjNum; }
        unsigned short getS3LogLevel() const { return s3LogLevel; }
        std::string getS3LogfilePrefix() const { return s3LogfilePrefix; }
        size_t getS3MpuSizeVariance() const { return s3MpuSizeVariance; }
        bool getS3NoCompression() const { return s3NoCompression; };
        bool getS3NoMpuCompletion() const { return s3NoMpuCompletion; };
        unsigned getS3MaxConnections() const { return s3MaxConnections; }
        size_t getS3MpuSplitSize() const { return s3MpuSplitSize; }
        uint64_t getS3MultiDelObjNum() const { return runS3MultiDelObjNum; }
        const std::string& getS3ObjectPrefix() const { return s3ObjectPrefix; }
        std::string getS3Region() const { return s3Region; }
        std::string getS3SessionToken() const { return s3SessionToken; }
        unsigned short getS3SignPolicy() const { return s3SignPolicy; }
        std::string getS3SSECKey() const { return s3SSECKey; }
        std::string getS3SSEKMSKey() const { return s3SSEKMSKey; }
        unsigned getS3ThroughputTargetGbps() const { return s3ThroughputTargetGbps; }
        bool getUseS3ClientSingleton() const { return useS3ClientSingleton; }
        bool getUseS3FastRead() const { return useS3FastRead; }
        bool getUseS3MPUSharing() const { return useS3MPUSharing; }
        bool getUseS3ObjectPrefixRand() const { return useS3ObjectPrefixRand; }
        bool getUseS3RandObjSelect() const { return useS3RandObjSelect; }
        bool getUseS3SSE() const { return useS3SSE; }
        bool getUseS3VirtualAddressing() const { return useS3VirtualAddressing; }
        std::string getS3ChecksumAlgo() const { return s3ChecksumAlgoStr; }
};


#endif /* MODES_S3_S3PROGARGS_H_ */
