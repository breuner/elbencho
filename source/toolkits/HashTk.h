// SPDX-FileCopyrightText: 2020-2025 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef TOOLKITS_HASHTK_H_
#define TOOLKITS_HASHTK_H_

#include "Common.h"

/**
 * Provides hash functions.
 */
class HashTk
{
    public:
        static std::string simple128(const std::string& input);

    private:
        HashTk() {}

        static uint64_t simple128CombineHelper(uint64_t x, uint64_t y);
};


#endif /* TOOLKITS_HASHTK_H_ */
