// SPDX-FileCopyrightText: 2020-2025 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef TOOLKITS_BASE64ENCODER_H_
#define TOOLKITS_BASE64ENCODER_H_

#include "Common.h"

/**
 * Encodes and decodes strings to/from base64 format.
 */
class Base64Encoder
{
    public:
        static std::string encode(const std::string& in);
        static std::string decode(const std::string& in);

    private:
        Base64Encoder() {}

    public: // inliners
        /**
         * Check if given string contains only valid base64 encoding characters.
         *
         * @return true if only valild base64 encoding characters found.
         */
        static inline bool isBase64(std::string str)
        {
            for(char& c: str)
            {
                if(isalnum(c) || (c == '+') || (c == '/') )
                    continue;

                return false;
            }

            return true;
        }
};




#endif /* TOOLKITS_BASE64ENCODER_H_ */
