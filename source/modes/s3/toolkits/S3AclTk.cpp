// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "modes/s3/toolkits/S3AclTk.h"

#ifdef S3_SUPPORT

/**
 * Convert grantee and grants given in progArgs into grants for S3 request.
 *
 * @outGrants will be filled based on user-defined s3 object acl grantee and permission values in
 *      progArgs.
 * @throws WorkerException on error (e.g. unknown/invalid user-defined values found).
 */
void S3AclTk::getS3ObjectAclGrants(const ProgArgs* progArgs, Aws::Vector<S3::Grant>& outGrants)
{
    const S3ProgArgs& s3Args = progArgs->getS3Args();

    const std::string granteeStr = s3Args.getS3AclGrantee();
    const std::string permissionsStr = s3Args.getS3AclGranteePermissions();

    // set grantee...

    S3::Grantee grantee;

    if(s3Args.getS3AclGranteeType() == ARG_S3ACL_GRANTEE_TYPE_EMAIL)
    {
        grantee.SetType(S3::Type::AmazonCustomerByEmail);
        grantee.SetEmailAddress(granteeStr);
    }
    else if(s3Args.getS3AclGranteeType() == ARG_S3ACL_GRANTEE_TYPE_ID)
    {
        grantee.SetType(S3::Type::CanonicalUser);
        grantee.SetID(granteeStr);
    }
    else if(s3Args.getS3AclGranteeType() == ARG_S3ACL_GRANTEE_TYPE_URI)
    {
        grantee.SetType(S3::Type::CanonicalUser);
        grantee.SetURI(granteeStr);
    }
    else if(s3Args.getS3AclGranteeType() == ARG_S3ACL_GRANTEE_TYPE_GROUP)
    {
        grantee.SetType(S3::Type::Group);
        grantee.SetURI(granteeStr);
    }
    else
        throw WorkerException("Undefined/unknown S3 ACL grantee type: "
                "'" + s3Args.getS3AclGranteeType() + "'");


    // add grantee's permissions to outGrants...

    if(permissionsStr.find(ARG_S3ACL_PERM_FULL_NAME) != std::string::npos)
    {
        S3::Grant grant;

        grant.SetGrantee(grantee);
        grant.SetPermission(S3::Permission::FULL_CONTROL);

        outGrants.push_back(grant);

        return;
    }

    if(permissionsStr.find(ARG_S3ACL_PERM_NONE_NAME) != std::string::npos)
    {
        S3::Grant grant;

        grant.SetGrantee(grantee);
        grant.SetPermission(S3::Permission::NOT_SET);

        outGrants.push_back(grant);

        return;
    }

    if(permissionsStr.find(ARG_S3ACL_PERM_FLAG_READ_NAME) !=
        std::string::npos)
    {
        S3::Grant grant;

        grant.SetGrantee(grantee);
        grant.SetPermission(S3::Permission::READ);

        outGrants.push_back(grant);
    }

    if(permissionsStr.find(ARG_S3ACL_PERM_FLAG_WRITE_NAME) !=
        std::string::npos)
    {
        S3::Grant grant;

        grant.SetGrantee(grantee);
        grant.SetPermission(S3::Permission::WRITE);

        outGrants.push_back(grant);
    }

    if(permissionsStr.find(ARG_S3ACL_PERM_FLAG_READACP_NAME) !=
        std::string::npos)
    {
        S3::Grant grant;

        grant.SetGrantee(grantee);
        grant.SetPermission(S3::Permission::READ_ACP);

        outGrants.push_back(grant);
    }

    if(permissionsStr.find(ARG_S3ACL_PERM_FLAG_WRITEACP_NAME) !=
        std::string::npos)
    {
        S3::Grant grant;

        grant.SetGrantee(grantee);
        grant.SetPermission(S3::Permission::WRITE_ACP);

        outGrants.push_back(grant);
    }
}



std::string S3AclTk::s3AclPermissionToStr(const S3::Permission& s3Permission)
{
    switch(s3Permission)
    {
        case S3::Permission::NOT_SET:
            return ARG_S3ACL_PERM_NONE_NAME;
        case S3::Permission::FULL_CONTROL:
            return ARG_S3ACL_PERM_FULL_NAME;
        case S3::Permission::WRITE:
            return ARG_S3ACL_PERM_FLAG_WRITE_NAME;
        case S3::Permission::WRITE_ACP:
            return ARG_S3ACL_PERM_FLAG_WRITEACP_NAME;
        case S3::Permission::READ:
            return ARG_S3ACL_PERM_FLAG_READ_NAME;
        case S3::Permission::READ_ACP:
            return ARG_S3ACL_PERM_FLAG_READACP_NAME;
        default:
            return "unknown";
    }
}

#endif // S3_SUPPORT
