// SPDX-FileCopyrightText: 2020-2025 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef TOOLKITS_S3CREDENTIALSTORE_H_
#define TOOLKITS_S3CREDENTIALSTORE_H_

#ifdef S3_SUPPORT

#include <aws/core/auth/AWSCredentials.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <mutex>
#include <string>
#include <vector>


class ProgArgs; // forward declaration

/**
 * Stores and manages multiple S3 credentials for multi-user benchmarking.
 * Thread-safe singleton class.
 */
class S3CredentialStore
{
    public:
        struct S3Credential 
        {
            std::string accessKey;
            std::string secretKey;
            
            S3Credential(const std::string& accessKey, const std::string& secretKey) :
                accessKey(accessKey), secretKey(secretKey) {}
        };

        /**
         * Get the singleton instance of the credential store.
         * 
         * @return Reference to the singleton instance
         */
        static S3CredentialStore& getInstance()
        {
            static S3CredentialStore instance;
            return instance;
        }

        /**
         * Load credentials from a file.
         * Each line in the file should be in format: access_key:secret_key
         * Lines starting with # are treated as comments.
         * 
         * @param filePath Path to the credentials file
         * @throw ProgException if file cannot be read or has invalid format
         */
        void loadCredentialsFromFile(const std::string& filePath);

        /**
         * Load credentials from a comma-separated list.
         * List format: "access_key1:secret_key1,access_key2:secret_key2,..."
         * 
         * @param credList Comma-separated list of credentials
         * @throw ProgException if list has invalid format
         */
        void loadCredentialsFromList(const std::string& credList);

        /**
         * Add a single credential pair to the store.
         * 
         * @param accessKey S3 access key
         * @param secretKey S3 secret key
         */
        void addCredential(const std::string& accessKey, const std::string& secretKey);

        /**
         * Get a credential based on worker rank.
         * Credentials are assigned in round-robin fashion based on the worker rank.
         * 
         * @param workerRank Rank of the worker requesting credentials
         * @return AWS credentials object
         * @throw ProgException if no credentials are available
         */
        std::shared_ptr<Aws::Auth::AWSCredentialsProvider> getCredential(size_t workerRank);

        size_t getNumCredentials() const { return credentials.size(); }
        void clear() { credentials.clear(); }

    private:
        S3CredentialStore() {} // private constructor for singleton

        // Prevent copying and assignment
        S3CredentialStore(const S3CredentialStore&) = delete;
        S3CredentialStore& operator=(const S3CredentialStore&) = delete;

        std::vector<S3Credential> credentials; // Store for all loaded credentials
        mutable std::mutex mutex; // Mutex for thread-safe access

        void parseAndAddCredential(const std::string& credStr);
        void validateCredential(const std::string& accessKey, const std::string& secretKey);
};

#endif /* S3_SUPPORT */
#endif /* TOOLKITS_S3CREDENTIALSTORE_H_ */ 