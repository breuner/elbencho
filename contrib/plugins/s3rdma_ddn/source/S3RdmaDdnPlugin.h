// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef S3RDMA_DDN_S3RDMADDNPLUGIN_H_
#define S3RDMA_DDN_S3RDMADDNPLUGIN_H_

#include "plugins/Plugin.h"

#define ARG_S3RDMATIMEOUT_LONG      "s3rdmatimeout"
#define ARG_S3RDMACONNTIMEOUT_LONG  "s3rdmaconntimeout"
#define ARG_S3RDMARETRIES_LONG      "s3rdmaretries"
#define ARG_S3RDMARETRYDELAY_LONG   "s3rdmaretrydelay"

#define S3RDMA_DEF_TIMEOUT_MS       30000
#define S3RDMA_DEF_CONNTIMEOUT_MS   5000
#define S3RDMA_DEF_RETRIES          3
#define S3RDMA_DEF_RETRYDELAY_MS    100


class S3RdmaDdnPlugin : public Plugin
{
    public:
        static S3RdmaDdnPlugin& getInstance()
        {
            static S3RdmaDdnPlugin instance;
            return instance;
        }

        const char* getName() const override { return "s3rdma_ddn"; }
        const char* getDescription() const override
            { return "S3-over-RDMA via NVIDIA cuObject. Contributed by DDN."; }

        void defineArgs(bpo::options_description& desc) override;
        void defineDefaults() override;
        void checkArgs(const ProgArgs& progArgs) override;
        void getAsPropertyTree(bpt::ptree& outTree) const override;
        void setFromPropertyTree(const bpt::ptree& tree) override;

        size_t getRequestTimeoutMS() const { return requestTimeoutMS; }
        size_t getConnectTimeoutMS() const { return connectTimeoutMS; }
        size_t getNumRetries() const { return numRetries; }
        size_t getRetryDelayMS() const { return retryDelayMS; }

    private:
        size_t requestTimeoutMS{S3RDMA_DEF_TIMEOUT_MS}; // total deadline per request attempt
        size_t connectTimeoutMS{S3RDMA_DEF_CONNTIMEOUT_MS}; // connection establishment deadline
        size_t numRetries{S3RDMA_DEF_RETRIES}; // retries after the first failed attempt
        size_t retryDelayMS{S3RDMA_DEF_RETRYDELAY_MS}; // base backoff, doubled per retry
};

#endif // S3RDMA_DDN_S3RDMADDNPLUGIN_H_
