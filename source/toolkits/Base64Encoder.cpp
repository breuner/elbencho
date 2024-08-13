#include "toolkits/Base64Encoder.h"

static const std::string Base64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";


/**
 * Encode given string to base64.
 */
std::string Base64Encoder::encode(const std::string &in)
{
    std::string out;
    int val = 0, valb = -6;

    for(unsigned char c : in)
    {
        val = (val << 8) + c;
        valb += 8;

        while(valb >= 0)
        {
            out.push_back(Base64Alphabet[ (val >> valb) & 0x3F] );
            valb -= 6;
        }
    }

    if(valb > -6)
        out.push_back(Base64Alphabet[ ( (val << 8) >> (valb + 8) ) & 0x3F] );

    // padding
    while(out.size() % 4)
        out.push_back('=');

    return out;
}

/**
 * Decode a base64 string.
 */
std::string Base64Encoder::decode(const std::string &in)
{
    std::string out;
    std::vector<int> T(256, -1);

    for(int i = 0; i < 64; i++)
        T[Base64Alphabet[i]] = i;

    int val = 0, valb = -8;

    for(unsigned char c : in)
    {
        if(T[c] == -1)
            break;

        val = (val << 6) + T[c];
        valb += 6;

        if(valb >= 0)
        {
            out.push_back(char((val >> valb) & 0xFF));
            valb -= 8;
        }
    }

    return out;
}
