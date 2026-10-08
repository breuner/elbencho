// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef HEXTK_H_
#define HEXTK_H_

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>

/**
 * Hex and decimal number formatting/parsing for the x-amz-rdma-* protocol headers.
 */
class HexTk
{
    public:
        static std::string toHex(uint64_t value, int minDigits = 0)
        {
            char buf[24];
            snprintf(buf, sizeof(buf), "%0*llx", minDigits, (unsigned long long)value);
            return buf;
        }

        // Strict hex number of 1..maxDigits digits, no prefix.
        static bool parseHex(const std::string& str, size_t maxDigits, uint64_t& outValue)
        {
            if(str.empty() || str.size() > maxDigits)
                return false;

            for(char c : str)
                if(!isxdigit( (unsigned char)c) )
                    return false;

            outValue = strtoull(str.c_str(), nullptr, 16);
            return true;
        }

        static bool parseDec(const std::string& str, uint64_t& outValue)
        {
            if(str.empty() || str.size() > 19)
                return false;

            for(char c : str)
                if(!isdigit( (unsigned char)c) )
                    return false;

            outValue = strtoull(str.c_str(), nullptr, 10);
            return true;
        }

        // numBytes random bytes as lowercase hex, e.g. for session ids.
        static std::string randomHex(unsigned numBytes)
        {
            static thread_local std::mt19937_64 randGen{std::random_device{}() };
            std::string str;

            for(unsigned i = 0; i < numBytes; i++)
                str += toHex(randGen() & 0xff, 2);

            return str;
        }

        // A random 24 bit packet sequence number in 1..0xffffff, as the RC handshakes need it.
        static uint32_t randomPsn()
        {
            return 1 + (strtoul(randomHex(3).c_str(), nullptr, 16) % 0xfffffe);
        }

    private:
        HexTk() {}
};

#endif // HEXTK_H_
