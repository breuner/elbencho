// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifdef SPDK_SUPPORT

#include <iostream>
#include <regex>

#include "Logger.h"
#include "ProgException.h"
#include "toolkits/spdk/SpdkTk.h"
#include "toolkits/StringTk.h"
#include "toolkits/UnitTk.h"


/**
 * Resolve each entry of benchPathsVec (numeric ID, human-friendly name, UUID, NGUID or regex)
 * into one or more namespace IDs, appended to benchPathSpdkNsIdsVec.
 *
 * Regex matches are expanded in place: the regex entry in benchPathsVec is replaced by the resolved
 * literal identifier (UUID/NGUID/name) of each match, so that benchPathsVec stays 1:1 with
 * benchPathSpdkNsIdsVec.
 *
 * @benchPathsVec will be modified if we have regex match to maintain 1:1 relationship with entries
 *      in benchPathSpdkNsIdsVec.
 * @benchPathSpdkNsIdsVec will be filled with the numeric IDs that correspond to each entry in
 *      benchPathsVec.
 * @throw ProgException if an entry doesn't resolve to any known namespace.
 */
void SpdkTk::resolveNamespaceSelection(SpdkNvmeClient& spdkClient, const UInt32Vec& nsIDs,
    StringVec& benchPathsVec, IntVec& benchPathSpdkNsIdsVec)
{
    bool gotRegExMatch = false; // just for log msg

    for(size_t i = 0; i < benchPathsVec.size(); i++)
    {
        const std::string& nsIdStr = benchPathsVec[i];

        // check if user provided numeric ID...

        if(StringTk::hasOnlyDigits(nsIdStr) )
        {
            int nsID = std::atoi(nsIdStr.c_str() );

            std::string nsName = spdkClient.getNamespaceName(nsID);
            if(nsName.empty() )
                throw ProgException("Given numeric namespace ID is invalid: " + nsIdStr);

            benchPathSpdkNsIdsVec.push_back(nsID);

            continue;
        }

        // not numeric ID, so check human-friendly name (e.g. "sys0:ctrl0:ns1"), UUID, NGUID...

        int nsIDNum = -1;

        for(uint32_t candidateNsID : nsIDs)
        {
            if( (spdkClient.getNamespaceName(candidateNsID) == nsIdStr) ||
                (spdkClient.getNamespaceUuid(candidateNsID) == nsIdStr) ||
                (spdkClient.getNamespaceNguid(candidateNsID) == nsIdStr) )
            {
                nsIDNum = candidateNsID;
                break;
            }
        }

        if(nsIDNum != -1)
        {
            benchPathSpdkNsIdsVec.push_back(nsIDNum);
            continue;
        }

        /* no exact match, so try nsIdStr as a regex against every namespace's human-friendly
            name (e.g. "mysubsys:.*" to select all namespaces of that subsystem) */

        UInt32Vec matchedNsIDs = matchNamespacesByRegex(spdkClient, nsIDs, nsIdStr);

        if(matchedNsIDs.empty() )
            throw ProgException("Given namespace name/UUID/NGUID is invalid: " + nsIdStr);

        gotRegExMatch = true;

        /* replace the regex entry in benchPathsVec with the resolved UUID/NGUID so that
            benchPathsVec stays 1:1 with benchPathSpdkNsIdsVec. */

        benchPathSpdkNsIdsVec.push_back(matchedNsIDs[0] );
        benchPathsVec[i] = getResolvedNamespaceIdentifier(spdkClient, matchedNsIDs[0] );

        for(size_t matchIdx = 1; matchIdx < matchedNsIDs.size(); matchIdx++)
        {
            benchPathSpdkNsIdsVec.push_back(matchedNsIDs[matchIdx] );
            benchPathsVec.insert(benchPathsVec.begin() + i + matchIdx,
                getResolvedNamespaceIdentifier(spdkClient, matchedNsIDs[matchIdx] ) );
        }

        i += (matchedNsIDs.size() - 1); // skip over the newly-inserted (already-resolved) entries
    }

    if(gotRegExMatch)
        LOGGER(Log_NORMAL, "NOTE: Number of selected namespaces after regex match: " <<
            benchPathSpdkNsIdsVec.size() << std::endl);
}

/**
 * Match a user-given path argument as a regex against every namespace's human-friendly name
 * (e.g. "mysubsys:.*" to select all namespaces of that subsystem).
 *
 * @return IDs of every namespace whose name matches; empty if the pattern is invalid (same as "no
 *      match" for the caller) or if it matches nothing.
 */
UInt32Vec SpdkTk::matchNamespacesByRegex(SpdkNvmeClient& spdkClient, const UInt32Vec& nsIDs,
    const std::string& regexStr)
{
    UInt32Vec matchedNsIDs;

    std::regex nameRegex;

    try
    {
        nameRegex = std::regex(regexStr);
    }
    catch(const std::regex_error& e)
    {
        return matchedNsIDs; // invalid pattern, treated the same as "no match" by the caller
    }

    for(uint32_t candidateNsID : nsIDs)
        if(std::regex_match(spdkClient.getNamespaceName(candidateNsID), nameRegex) )
            matchedNsIDs.push_back(candidateNsID);

    return matchedNsIDs;
}

/**
 * Get a stable literal identifier for a namespace, e.g. to replace a regex match in benchPathsVec
 * with something that will exactly re-match on a later/different discovery (see
 * ProgArgs::prepareSpdk() ). Prefers UUID, falls back to NGUID, falls back to the human-friendly
 * name (which is always non-empty for a valid nsID).
 */
std::string SpdkTk::getResolvedNamespaceIdentifier(SpdkNvmeClient& spdkClient, uint32_t nsID)
{
    std::string uuid = spdkClient.getNamespaceUuid(nsID);
    if(!uuid.empty() )
        return uuid;

    std::string nguid = spdkClient.getNamespaceNguid(nsID);
    if(!nguid.empty() )
        return nguid;

    return spdkClient.getNamespaceName(nsID);
}

/**
 * Print discovered namespaces to console. Used in standalone "namespace info" mode, i.e. when the
 * user ran without any benchmark path arguments.
 */
void SpdkTk::printNamespaceDiscoveryResult(SpdkNvmeClient& spdkClient, const UInt32Vec& nsIDs)
{
    std::cout << "SPDK NVMe-oF discovery result... "
        "(Namespace; Numeric ID; Size; Format; Model; UUID/NGUID)" << std::endl;

    for(uint32_t nsID : nsIDs)
    {
        std::string nsName = spdkClient.getNamespaceName(nsID);
        uint64_t nsSize = spdkClient.getNamespaceSize(nsID);
        uint32_t nsSectorSize = spdkClient.getNamespaceSectorSize(nsID);
        std::string nsModel = spdkClient.getNamespaceModel(nsID);
        std::string nsUUID = spdkClient.getNamespaceUuid(nsID);
        std::string nsNguid = spdkClient.getNamespaceNguid(nsID);

        // fall back to the NGUID for display only if the target has no real UUID
        bool showNguid = nsUUID.empty() && !nsNguid.empty();

        std::cout << "* " << nsName << "; " <<
            nsID << "; " <<
            UnitTk::numToHumanStrBase2(nsSize) << "; " <<
            nsSectorSize << "B; " <<
            nsModel << "; " <<
            (showNguid ? nsNguid : nsUUID) <<
            std::endl;
    }
}

#endif // SPDK_SUPPORT
