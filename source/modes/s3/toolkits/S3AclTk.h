// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef MODES_S3_S3ACLTK_H_
#define MODES_S3_S3ACLTK_H_

#include <string>

#include "Common.h"
#include "ProgArgs.h"
#include "modes/s3/toolkits/S3Tk.h"
#include "workers/WorkerException.h"

#ifdef S3_SUPPORT
    #include INCLUDE_AWS_S3(model/ObjectCannedACL.h)
    #include INCLUDE_AWS_S3(model/PutBucketAclRequest.h)
    #include INCLUDE_AWS_S3(model/PutObjectAclRequest.h)
#endif // S3_SUPPORT


#ifdef S3_SUPPORT

/**
 * Translation of the user-defined S3 ACL grantee & permissions into S3 requests.
 */
class S3AclTk
{
    public:
        static void getS3ObjectAclGrants(const ProgArgs* progArgs,
            Aws::Vector<S3::Grant>& outGrants);
        static std::string s3AclPermissionToStr(const S3::Permission& s3Permission);

        /**
         * Apply ACL grantee and grants given in progArgs to S3 upload request. Grantee can also
         * be the special name of a canned ACL.
         *
         * @outRequest the s3 object upload request (PutObjectAcl or PutBucketAcl) to which to
         *      apply the user-defined s3 object acl grantee and permission values in progArgs.
         * @throws WorkerException on error (e.g. unknown/invalid user-defined values found).
         */
        template <typename S3CANNEDACLTYPE, typename S3REQUEST>
        static void applyS3PutAclRequestGrants(const ProgArgs* progArgs, S3REQUEST& outRequest)
        {
            const S3ProgArgs& s3Args = progArgs->getS3Args();
            const std::string granteeStr = s3Args.getS3AclGrantee();
            const std::string permissionsStr = s3Args.getS3AclGranteePermissions();

            S3CANNEDACLTYPE cannedAcl =
                (S3CANNEDACLTYPE)S3::ObjectCannedACLMapper::GetObjectCannedACLForName(granteeStr);

            /* note: checking for ::NOT_SET alone here is not enough, because
                GetObjectCannedACLForName() can return other values if granteeStr doesn't match
                another enum value. */
            switch( (S3::ObjectCannedACL)cannedAcl)
            {
                case S3::ObjectCannedACL::private_:
                case S3::ObjectCannedACL::public_read:
                case S3::ObjectCannedACL::public_read_write:
                case S3::ObjectCannedACL::authenticated_read:
                case S3::ObjectCannedACL::aws_exec_read:
                case S3::ObjectCannedACL::bucket_owner_read:
                case S3::ObjectCannedACL::bucket_owner_full_control:
                { // found canned ACL as special grantee
                    outRequest.SetACL(cannedAcl);
                    return;
                }

                case S3::ObjectCannedACL::NOT_SET:
                default:
                { // normal grantee, not a canned ACL
                    break;
                }
            }

            Aws::Vector<S3::Grant> grants;

            getS3ObjectAclGrants(progArgs, grants);

            if(grants.empty() )
                throw WorkerException("Undefined/unknown S3 ACL permission type: "
                    "'" + s3Args.getS3AclGranteePermissions() + "'");

            S3::AccessControlPolicy acp;
            acp.SetGrants(grants);

            outRequest.SetAccessControlPolicy(acp);
        }


        /**
         * Apply ACL grantee and grants given in progArgs to S3 upload request. Grantee can also
         * be the special name of a canned ACL.
         *
         * @outRequest the s3 object upload request (PutObjectRequest or
         *      CreateMultipartUploadRequest) to which to apply the user-defined s3 object acl
         *      grantee and permission values in progArgs.
         * @throws WorkerException on error (e.g. unknown/invalid user-defined values found).
         */
        template <typename S3REQUEST>
        static void applyS3PutObjectAclGrants(const ProgArgs* progArgs, S3REQUEST& outRequest)
        {
            const S3ProgArgs& s3Args = progArgs->getS3Args();
            const std::string granteeStr = s3Args.getS3AclGrantee();
            const std::string permissionsStr = s3Args.getS3AclGranteePermissions();

            // check if grantee matches a canned acl...

            S3::ObjectCannedACL cannedAcl = S3::ObjectCannedACLMapper::GetObjectCannedACLForName(
                granteeStr);

            if(cannedAcl!= S3::ObjectCannedACL::NOT_SET)
            { // found canned ACL as special grantee
                outRequest.SetACL(cannedAcl);
                return;
            }

            // add permissions for given grantee...

            if(permissionsStr.find(ARG_S3ACL_PERM_FULL_NAME) != std::string::npos)
            {
                outRequest.SetGrantFullControl(granteeStr);

                return;
            }

            if(permissionsStr.find(ARG_S3ACL_PERM_NONE_NAME) != std::string::npos)
            {
                // nothing to do
                return;
            }

            if(permissionsStr.find(ARG_S3ACL_PERM_FLAG_READ_NAME) !=
                std::string::npos)
            {
                outRequest.SetGrantRead(granteeStr);
            }

            if(permissionsStr.find(ARG_S3ACL_PERM_FLAG_WRITE_NAME) !=
                std::string::npos)
            {
                throw WorkerException("Setting a write grant is not supported for inline S3 ACLs.");
            }

            if(permissionsStr.find(ARG_S3ACL_PERM_FLAG_READACP_NAME) !=
                std::string::npos)
            {
                outRequest.SetGrantReadACP(granteeStr);
            }

            if(permissionsStr.find(ARG_S3ACL_PERM_FLAG_WRITEACP_NAME) !=
                std::string::npos)
            {
                outRequest.SetGrantWriteACP(granteeStr);
            }
        }

};

#endif // S3_SUPPORT

#endif /* MODES_S3_S3ACLTK_H_ */
