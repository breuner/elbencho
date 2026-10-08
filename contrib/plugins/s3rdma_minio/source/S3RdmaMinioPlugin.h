// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef S3RDMA_MINIO_S3RDMAMINIOPLUGIN_H_
#define S3RDMA_MINIO_S3RDMAMINIOPLUGIN_H_

#include "plugins/Plugin.h"

/**
 * Plugin "s3rdma_minio": GPU-direct S3-over-RDMA via NVIDIA cuObject. Contributed by MinIO.
 *
 * Single-part object GET/PUT move their payload out-of-band over RDMA (directly to/from GPU
 * memory when "--gpuids" is given, otherwise host memory), while a body-less HTTP control
 * request carries the x-amz-rdma-* headers. The data path is S3RdmaMinioTransport.
 */
class S3RdmaMinioPlugin : public Plugin
{
    public:
        static S3RdmaMinioPlugin& getInstance()
        {
            static S3RdmaMinioPlugin instance;
            return instance;
        }

        const char* getName() const override { return "s3rdma_minio"; }
        const char* getDescription() const override
            { return "GPU-direct S3-over-RDMA via NVIDIA cuObject. Contributed by MinIO."; }

        void checkArgs(const ProgArgs& progArgs) override;
};

#endif // S3RDMA_MINIO_S3RDMAMINIOPLUGIN_H_
