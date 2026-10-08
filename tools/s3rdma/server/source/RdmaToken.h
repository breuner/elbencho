// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef RDMATOKEN_H_
#define RDMATOKEN_H_

#include <cstdint>
#include <string>

/**
 * The token of the "x-amz-rdma-token" header of an object request. The server only needs to know
 * where the client buffer starts and how large it is; the rest of the token is handed unchanged to
 * the transport that understands it. Two formats are recognized:
 *
 * cuObject's RDMA descriptor (DC transport), colon separated hex numbers:
 *   "<buffer address, 16 hex>:<buffer size, 8 hex>:<rkey>:<lid>:<dctn>:<gid present>:<gid>..."
 *
 * The token of the RC client shim (../cuobjclient-shim), which refers to a connection that was
 * set up beforehand via the "/.s3rdma-rc/connect" endpoint:
 *   "rc:<connection id, 32 hex>:<buffer address, hex>:<buffer size, hex>:<rkey, hex>"
 */
struct RdmaToken
{
    static constexpr const char* RC_PREFIX = "rc:";

    std::string descr; // the complete token as received
    uint64_t remoteAddr = 0; // start of the client buffer
    uint64_t size = 0; // size of the client buffer, i.e. the number of bytes to transfer

    bool isRc() const { return descr.compare(0, 3, RC_PREFIX) == 0; }

    /**
     * @return false if the string is in neither of the formats above.
     */
    static bool parse(const std::string& str, RdmaToken& outToken)
    {
        const bool isRc = (str.compare(0, 3, RC_PREFIX) == 0);
        const size_t start = isRc ? 36 : 0; // where the address field starts

        if(isRc && (str.size() <= start || str[start - 1] != ':') )
            return false; // (connection id is 32 hex chars)

        const size_t colon1 = str.find(':', start);
        if(colon1 == std::string::npos || colon1 == start || (!isRc && colon1 > 16) )
            return false; // (a longer first field means a different token scheme)

        const size_t colon2 = str.find(':', colon1 + 1);
        if(colon2 == std::string::npos || colon2 == colon1 + 1)
            return false;

        if(!parseHex(str.substr(start, colon1 - start), outToken.remoteAddr) ||
            !parseHex(str.substr(colon1 + 1, colon2 - colon1 - 1), outToken.size) )
            return false;

        outToken.descr = str;
        return true;
    }

    private:
        static bool parseHex(const std::string& str, uint64_t& outValue)
        {
            try
            {
                size_t numParsed = 0;
                outValue = std::stoull(str, &numParsed, 16);
                return numParsed == str.size();
            }
            catch(const std::exception&)
            {
                return false;
            }
        }
};

#endif // RDMATOKEN_H_
