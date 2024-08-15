#include <iomanip>
#include "toolkits/HashTk.h"

/**
 * Helper function to combine 64-bit values.
 */
uint64_t HashTk::simple128CombineHelper(uint64_t x, uint64_t y)
{
    return x ^ (y * 0x9E3779B97F4A7C15ull);
}

/**
 * Calculate a simple 128-bit hash of the given string.
 */
std::string HashTk::simple128(const std::string& input)
{
    uint64_t hash1 = 0xC6A4A7935BD1E995ull;
    uint64_t hash2 = 0xDEADBEEFCAFEBABEull;
    const uint64_t multiplier = 0x87C37B91114253D5ull;
    const uint64_t seed = 0x4CF5AD432745937Full;

    for(char c : input)
    {
        uint64_t value = static_cast<uint64_t>(c);
        hash1 = simple128CombineHelper(hash1, value * multiplier);
        hash2 = simple128CombineHelper(hash2, value * seed);
    }

    // Convert the 128-bit hash (hash1 and hash2) to a hexadecimal string
    std::ostringstream result;
    result << std::hex << std::setw(16) << std::setfill('0') << hash1;
    result << std::hex << std::setw(16) << std::setfill('0') << hash2;

    return result.str();
}
