// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <cstdlib>
#include <cstring>
#include <exception>
#include <getopt.h>
#include <iostream>
#include <stdexcept>
#include <string>

#include "Log.h"
#include "RdmaS3Client.h"

namespace
{
    struct Config
    {
        std::string endpoint = "http://127.0.0.1:9000";
        std::string bucket;
        std::string key = "obj";
        size_t size = 1024 * 1024; // object size
        uint64_t offset = 0; // GET range start
        unsigned numObjects = 1; // objects "<key>0" .. "<key>N-1"; a single one keeps the key as is
        bool doMkbucket = false;
        bool doPut = false;
        bool doGet = false;
        bool doDelete = false;
        bool doRmbucket = false;
        bool verify = false;
        bool verbose = false;
    };

    void printUsage()
    {
        std::cout <<
            EXE_NAME " - Minimal S3-over-RDMA client for testing tools/s3rdma/server.\n"
            "\n"
            "Speaks the S3-over-RDMA protocol of NVIDIA cuObject like elbencho's s3rdma plugins\n"
            "do: a body-less HTTP request carries the RDMA token, the object data moves over\n"
            "RDMA from/to host memory. The RDMA transport is the cuObject client library this\n"
            "executable was linked against (NVIDIA's, or the RC shim in tools/s3rdma/cuobjclient-shim,\n"
            "which is configured via S3RDMA_RC_ENDPOINT and S3RDMA_RC_ADDR). Requests are not\n"
            "signed.\n"
            "\n"
            "Usage:\n"
            "  " EXE_NAME " --endpoint URL --bucket NAME [--mkbucket] [--put] [--get] [--delete]\n"
            "      [--rmbucket] [OPTIONS]\n"
            "\n"
            "The operations run in the order above. Options:\n"
            "  -e, --endpoint URL    S3 endpoint. (Default: http://127.0.0.1:9000)\n"
            "  -B, --bucket NAME     Bucket name. (Required)\n"
            "  -k, --key NAME        Object key, gets the object index appended when --count is\n"
            "                        above 1. (Default: obj)\n"
            "  -m, --mkbucket        Create the bucket.\n"
            "  -w, --put             Upload the object(s) with a known data pattern.\n"
            "  -r, --get             Download the object(s).\n"
            "  -F, --delete          Delete the object(s).\n"
            "  -D, --rmbucket        Delete the bucket.\n"
            "  -s, --size SIZE       Object size, also the download length. Suffixes k/m/g.\n"
            "                        (Default: 1m)\n"
            "  -o, --offset BYTES    Start offset of the download range. (Default: 0)\n"
            "  -n, --count NUM       Number of objects. (Default: 1)\n"
            "  -V, --verify          Check the downloaded data against the pattern of --put.\n"
            "  -v, --verbose         Log every transfer.\n"
            "  -h, --help            Print this help.\n";
    }

    // "16M" => 16 MiB. Suffixes k/m/g (binary), case-insensitive.
    size_t parseSize(const std::string& str)
    {
        size_t numParsed = 0;
        const unsigned long long number = std::stoull(str, &numParsed);
        unsigned shift = 0;

        if(numParsed < str.size() )
        {
            if(numParsed + 1 != str.size() )
                throw std::invalid_argument("invalid size: " + str);

            switch(tolower(str[numParsed] ) )
            {
                case 'k': shift = 10; break;
                case 'm': shift = 20; break;
                case 'g': shift = 30; break;
                default: throw std::invalid_argument("invalid size suffix: " + str);
            }
        }

        return number << shift;
    }

    // @return false if the program should exit successfully right away.
    bool parseArgs(int argc, char** argv, Config& config)
    {
        const struct option longOpts[] =
        {
            {"endpoint", required_argument, nullptr, 'e'},
            {"bucket", required_argument, nullptr, 'B'},
            {"key", required_argument, nullptr, 'k'},
            {"mkbucket", no_argument, nullptr, 'm'},
            {"put", no_argument, nullptr, 'w'},
            {"get", no_argument, nullptr, 'r'},
            {"delete", no_argument, nullptr, 'F'},
            {"rmbucket", no_argument, nullptr, 'D'},
            {"size", required_argument, nullptr, 's'},
            {"offset", required_argument, nullptr, 'o'},
            {"count", required_argument, nullptr, 'n'},
            {"verify", no_argument, nullptr, 'V'},
            {"verbose", no_argument, nullptr, 'v'},
            {"help", no_argument, nullptr, 'h'},
            {nullptr, 0, nullptr, 0}
        };

        int opt;
        while( (opt = getopt_long(argc, argv, "e:B:k:mwrFDs:o:n:Vvh", longOpts, nullptr) ) != -1)
        {
            switch(opt)
            {
                case 'e': config.endpoint = optarg; break;
                case 'B': config.bucket = optarg; break;
                case 'k': config.key = optarg; break;
                case 'm': config.doMkbucket = true; break;
                case 'w': config.doPut = true; break;
                case 'r': config.doGet = true; break;
                case 'F': config.doDelete = true; break;
                case 'D': config.doRmbucket = true; break;
                case 's': config.size = parseSize(optarg); break;
                case 'o': config.offset = std::stoull(optarg); break;
                case 'n': config.numObjects = std::stoul(optarg); break;
                case 'V': config.verify = true; break;
                case 'v': config.verbose = true; break;
                case 'h': printUsage(); return false;
                default: printUsage(); throw std::invalid_argument("invalid arguments");
            }
        }

        if(optind < argc)
            throw std::invalid_argument(std::string("unexpected argument: ") + argv[optind] );

        if(config.bucket.empty() )
            throw std::invalid_argument("--bucket is required");

        if(!config.size || !config.numObjects)
            throw std::invalid_argument("--size and --count must not be zero");

        return true;
    }

    std::string objectKey(const Config& config, unsigned index)
    {
        return (config.numObjects == 1) ? config.key : config.key + std::to_string(index);
    }

    // The data pattern of an object: depends on the object's offset and index, so that a mix-up
    // of objects or of ranges gets noticed by --verify.
    unsigned char patternByte(uint64_t offset, unsigned objectIndex)
    {
        return (unsigned char)( (offset * 7 + objectIndex * 131 + 13) % 251);
    }

    void fillPattern(char* buf, size_t len, unsigned objectIndex)
    {
        for(size_t i = 0; i < len; i++)
            buf[i] = patternByte(i, objectIndex);
    }

    // @return offset of the first mismatch, or -1 if the buffer matches the pattern.
    ssize_t findMismatch(const char* buf, size_t len, uint64_t offset, unsigned objectIndex)
    {
        for(size_t i = 0; i < len; i++)
            if( (unsigned char)buf[i] != patternByte(offset + i, objectIndex) )
                return i;

        return -1;
    }
}

int main(int argc, char** argv)
{
    Config config;

    try
    {
        if(!parseArgs(argc, argv, config) )
            return EXIT_SUCCESS;
    }
    catch(const std::exception& e)
    {
        std::cerr << "ERROR: " << e.what() << " (see --help)" << std::endl;
        return EXIT_FAILURE;
    }

    Log::verbose = config.verbose;

    void* bufVoid = nullptr;
    if(posix_memalign(&bufVoid, 4096, config.size) )
    {
        Log::error("Allocation of the I/O buffer failed. Size: " + std::to_string(config.size) );
        return EXIT_FAILURE;
    }

    char* buf = static_cast<char*>(bufVoid);
    RdmaS3Client client(config.endpoint);

    if(!client.isConnected() || !client.registerBuffer(buf, config.size) )
        return EXIT_FAILURE;

    if(config.doMkbucket && !client.createBucket(config.bucket) )
        return EXIT_FAILURE;

    if(config.doPut)
        for(unsigned i = 0; i < config.numObjects; i++)
        {
            fillPattern(buf, config.size, i);

            if(!client.putObject(config.bucket, objectKey(config, i), buf, config.size) )
                return EXIT_FAILURE;
        }

    if(config.doGet)
        for(unsigned i = 0; i < config.numObjects; i++)
        {
            memset(buf, 0, config.size);

            const ssize_t numBytes = client.getObject(config.bucket, objectKey(config, i), buf,
                config.size, config.offset);

            if(numBytes < 0)
                return EXIT_FAILURE;

            if( (size_t)numBytes != config.size)
            {
                Log::error("Server reported " + std::to_string(numBytes) + " transferred bytes "
                    "instead of " + std::to_string(config.size) + " for " + objectKey(config, i) );
                return EXIT_FAILURE;
            }

            const ssize_t mismatch = config.verify ?
                findMismatch(buf, config.size, config.offset, i) : -1;

            if(mismatch >= 0)
            {
                Log::error("Data verification failed for " + objectKey(config, i) + " at offset " +
                    std::to_string(config.offset + mismatch) );
                return EXIT_FAILURE;
            }
        }

    if(config.doDelete)
        for(unsigned i = 0; i < config.numObjects; i++)
            if(!client.deleteObject(config.bucket, objectKey(config, i) ) )
                return EXIT_FAILURE;

    if(config.doRmbucket && !client.deleteBucket(config.bucket) )
        return EXIT_FAILURE;

    Log::info("Done" + std::string(config.verify && config.doGet ? ", data verified." : ".") );

    free(buf);

    return EXIT_SUCCESS;
}
