// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef TOOLKITS_SPDKTK_H_
#define TOOLKITS_SPDKTK_H_

#include "Common.h"

#ifdef SPDK_SUPPORT

#include "toolkits/spdk/SpdkNvmeClient.h"

/**
 * Static helpers for SPDK.
 */
class SpdkTk
{
    public:
        static void resolveNamespaceSelection(SpdkNvmeClient& spdkClient, const UInt32Vec& nsIDs,
            StringVec& benchPathsVec, IntVec& benchPathSpdkNsIdsVec);
        static UInt32Vec matchNamespacesByRegex(SpdkNvmeClient& spdkClient,
            const UInt32Vec& nsIDs, const std::string& regexStr);
        static std::string getResolvedNamespaceIdentifier(SpdkNvmeClient& spdkClient,
            uint32_t nsID);
        static void printNamespaceDiscoveryResult(SpdkNvmeClient& spdkClient,
            const UInt32Vec& nsIDs);
};

#endif // SPDK_SUPPORT

#endif /* TOOLKITS_SPDKTK_H_ */
