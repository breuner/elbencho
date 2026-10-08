// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef HIPOBJTOKEN_H_
#define HIPOBJTOKEN_H_

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

/**
 * The fixed-width RDMA token of AMD hipObject: 44 bytes as 88 lowercase hex chars, integers in
 * little endian byte order (see docs/interop.rst of hipObject):
 *   transport u8 | qp number u32 | gid 16 bytes | rkey u32 | remote address u64 | length u64 |
 *   port number u8 | lid u16
 * A client may append ":<addr hex>:<size hex>" to the token in the x-amz-rdma-token header.
 */
struct HipObjToken
{
    static constexpr size_t NUM_BYTES = 44;
    static constexpr size_t NUM_HEX_CHARS = 2 * NUM_BYTES;
    static constexpr uint8_t TRANSPORT_DC = 0;
    static constexpr uint8_t TRANSPORT_RC = 1;

    uint8_t transport = TRANSPORT_RC;
    uint32_t qpNum = 0;
    uint8_t gid[16] = {};
    uint32_t rkey = 0;
    uint64_t remoteAddr = 0;
    uint64_t length = 0;
    uint8_t portNum = 1;
    uint16_t lid = 0;

    /**
     * @return true if the header value is a hipObject token (88 hex chars, optionally followed by
     *    the ":<addr>:<size>" suffix), as opposed to cuObject's colon separated descriptor.
     */
    static bool isTokenHeader(const std::string& header)
    {
        const size_t len = std::min(header.find(':'), header.size() );

        if(len != NUM_HEX_CHARS)
            return false;

        for(size_t i = 0; i < len; i++)
            if(!isxdigit( (unsigned char)header[i] ) )
                return false;

        return true;
    }

    static bool decode(const std::string& header, HipObjToken& outToken)
    {
        if(!isTokenHeader(header) )
            return false;

        uint8_t bytes[NUM_BYTES];
        for(size_t i = 0; i < NUM_BYTES; i++)
            bytes[i] = (hexVal(header[2 * i] ) << 4) | hexVal(header[2 * i + 1] );

        outToken.transport = bytes[0];
        outToken.qpNum = readLE(bytes + 1, 4);
        memcpy(outToken.gid, bytes + 5, 16);
        outToken.rkey = readLE(bytes + 21, 4);
        outToken.remoteAddr = readLE(bytes + 25, 8);
        outToken.length = readLE(bytes + 33, 8);
        outToken.portNum = bytes[41];
        outToken.lid = readLE(bytes + 42, 2);

        return true;
    }

    std::string encode() const
    {
        uint8_t bytes[NUM_BYTES];
        bytes[0] = transport;
        writeLE(bytes + 1, qpNum, 4);
        memcpy(bytes + 5, gid, 16);
        writeLE(bytes + 21, rkey, 4);
        writeLE(bytes + 25, remoteAddr, 8);
        writeLE(bytes + 33, length, 8);
        bytes[41] = portNum;
        writeLE(bytes + 42, lid, 2);

        std::string hex;
        char buf[3];

        for(uint8_t byte : bytes)
        {
            snprintf(buf, sizeof(buf), "%02x", byte);
            hex += buf;
        }

        return hex;
    }

    private:
        static unsigned hexVal(char c)
        {
            return isdigit( (unsigned char)c) ?
                (c - '0') : (tolower( (unsigned char)c) - 'a' + 10);
        }

        static uint64_t readLE(const uint8_t* p, unsigned numBytes)
        {
            uint64_t value = 0;

            for(unsigned i = 0; i < numBytes; i++)
                value |= (uint64_t)p[i] << (8 * i);

            return value;
        }

        static void writeLE(uint8_t* p, uint64_t value, unsigned numBytes)
        {
            for(unsigned i = 0; i < numBytes; i++)
                p[i] = (value >> (8 * i) ) & 0xff;
        }
};

#endif // HIPOBJTOKEN_H_
