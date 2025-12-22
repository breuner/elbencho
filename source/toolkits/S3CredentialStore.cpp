// SPDX-FileCopyrightText: 2020-2025 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifdef S3_SUPPORT

#include <boost/algorithm/string.hpp>
#include <fstream>
#include <sstream>
#include "Common.h"
#include "Logger.h"
#include "ProgException.h"
#include "toolkits/S3CredentialStore.h"
#include "toolkits/StringTk.h"

/**
 * Load credentials from a file where each line contains a credential pair in format:
 * access_key:secret_key
 */
void S3CredentialStore::loadCredentialsFromFile(const std::string& filePath)
{
    std::ifstream file(filePath);
    if(!file)
        throw ProgException("Unable to open S3 credentials file: " + filePath);

    std::string line;
    while(std::getline(file, line))
    {
        if(line.empty() || line[0] == '#') // skip empty lines and comments
            continue;

        try
        {
            parseAndAddCredential(line);
        }
        catch(const ProgException& e)
        {
            LOGGER(Log_NORMAL, "Warning: Skipping invalid credential in file. "
                    << e.what() << std::endl);
        }
    }

    if(credentials.empty())
        throw ProgException("No valid credentials found in file: " + filePath);
}

/**
 * Load credentials from a comma-separated list in format:
 * access_key1:secret_key1,access_key2:secret_key2,...
 */
void S3CredentialStore::loadCredentialsFromList(const std::string& credList)
{
    StringVec credVec;
    boost::split(credVec, credList, boost::is_any_of(","));

    if(credVec.empty())
        throw ProgException("Empty credentials list provided");

    for(const std::string& credStr : credVec)
    {
        try
        {
            parseAndAddCredential(credStr);
        }
        catch(const ProgException& e)
        {
            LOGGER(Log_NORMAL, "Warning: Skipping invalid credential in list. "
                    << e.what() << std::endl);
        }
    }

    if(credentials.empty())
        throw ProgException("No valid credentials found in provided list");
}

/**
 * Add a single credential pair.
 */
void S3CredentialStore::addCredential(const std::string& accessKey, const std::string& secretKey)
{
    validateCredential(accessKey, secretKey);

    std::lock_guard<std::mutex> lock(mutex);
    credentials.emplace_back(accessKey, secretKey);
}

/**
 * Get AWS credentials for a given worker rank. Uses round-robin distribution.
 */
std::shared_ptr<Aws::Auth::AWSCredentialsProvider> S3CredentialStore::getCredential(size_t workerRank)
{
    std::lock_guard<std::mutex> lock(mutex);

    if(credentials.empty())
        throw ProgException("No S3 credentials available");

    size_t index = workerRank % credentials.size();
    const S3Credential& cred = credentials[index];

    return std::make_shared<Aws::Auth::SimpleAWSCredentialsProvider>(cred.accessKey, cred.secretKey);
}

/**
 * Parse a credential string in format "access_key:secret_key" and add it to the store.
 */
void S3CredentialStore::parseAndAddCredential(const std::string& credStr)
{
    StringVec keyPair;
    boost::split(keyPair, credStr, boost::is_any_of(":"));

    if(keyPair.size() != 2)
        throw ProgException("Invalid credential format. Expected 'access_key:secret_key', got: " + credStr);

    std::string accessKey = keyPair[0];
    std::string secretKey = keyPair[1];
    boost::trim(accessKey);
    boost::trim(secretKey);

    validateCredential(accessKey, secretKey);

    std::lock_guard<std::mutex> lock(mutex);
    credentials.emplace_back(accessKey, secretKey);
}

/**
 * Validate that the provided credentials are not empty.
 */
void S3CredentialStore::validateCredential(const std::string& accessKey, const std::string& secretKey)
{
    if(accessKey.empty())
        throw ProgException("S3 access key cannot be empty");

    if(secretKey.empty())
        throw ProgException("S3 secret key cannot be empty");
}

#endif /* S3_SUPPORT */ 