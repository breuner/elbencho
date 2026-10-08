// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef S3RDMA_CLOUDIAN_S3RDMACLOUDIANPLUGIN_H_
#define S3RDMA_CLOUDIAN_S3RDMACLOUDIANPLUGIN_H_

#include "plugins/Plugin.h"

#define ARG_CUOBJHOSTBUFREG_LONG    "cuobjhostbufreg"

/**
 * Plugin "s3rdma_cloudian": S3-over-RDMA via Cloudian's aws-sdk-cpp fork, which adds RDMA
 * variants of the S3 requests on top of NVIDIA cuObject. Contributed by Cloudian.
 *
 * The fork replaces the AWS SDK for the whole executable, so the RDMA request types are in use
 * even while this plugin is inactive (then the fork falls back to TCP). Activating the plugin
 * enables the RDMA transfers. The data path is S3RdmaCloudianTransport.
 */
class S3RdmaCloudianPlugin : public Plugin
{
    public:
        static S3RdmaCloudianPlugin& getInstance()
        {
            static S3RdmaCloudianPlugin instance;
            return instance;
        }

        const char* getName() const override { return "s3rdma_cloudian"; }
        const char* getDescription() const override
            { return "S3-over-RDMA via Cloudian's aws-sdk-cpp fork. Contributed by Cloudian."; }

        void defineArgs(bpo::options_description& desc) override;
        void defineDefaults() override { useCuObjHostBufReg = false; }
        void checkArgs(const ProgArgs& progArgs) override;
        void getAsPropertyTree(bpt::ptree& outTree) const override;
        void setFromPropertyTree(const bpt::ptree& tree) override;

        bool getUseCuObjHostBufReg() const { return useCuObjHostBufReg; }

    private:
        bool useCuObjHostBufReg{false}; // pre-register host memory buffers with cuObject
};

#endif // S3RDMA_CLOUDIAN_S3RDMACLOUDIANPLUGIN_H_
