// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "S3RdmaMinioPlugin.h"

#include "plugins/PluginRegistry.h"
#include "ProgArgs.h"
#include "ProgException.h"

ELB_REGISTER_PLUGIN(S3RdmaMinioPlugin)

#define CUOBJ_MAX_BLOCKSIZE (4095ULL << 20) // cuObject cannot register a 4 GiB buffer

/**
 * The RDMA path moves one block per request with one request in flight, needs a buffer to
 * receive into and its control request carries none of the per-request S3 options. Objects
 * larger than the block size are uploaded as multipart with each part over RDMA and downloaded
 * as ranged RDMA GETs, so the block size cap only limits the transfer size, not the object size.
 */
void S3RdmaMinioPlugin::checkArgs(const ProgArgs& progArgs)
{
    if(!isActive() )
        return;

    const std::string pluginArg = std::string("--" ARG_PLUGINS_LONG " ") + getName();

    if(progArgs.getBenchMode() != BenchMode_S3)
        throw ProgException("Plugin \"" + pluginArg + "\" can only be used with S3.");

    if(progArgs.getIODepth() != 1)
        throw ProgException("Plugin \"" + pluginArg + "\" requires \"--" ARG_IODEPTH_LONG "=1\", "
            "because async/multi-depth transfers are not supported on the RDMA path.");

    if(progArgs.getS3Args().getUseS3FastRead() )
        throw ProgException("Plugin \"" + pluginArg + "\" cannot be used together with "
            "\"--" ARG_S3FASTGET_LONG "\", which discards downloaded data and thus has no buffer "
            "to receive the RDMA transfer.");

    if(progArgs.getBlockSize() > CUOBJ_MAX_BLOCKSIZE)
        throw ProgException("Plugin \"" + pluginArg + "\" requires a block size (\"-"
            ARG_BLOCK_SHORT "\") of at most 4095 MiB, because cuObject cannot register a 4 GiB "
            "buffer. Larger objects get uploaded as multipart with each part over RDMA. "
            "Block size: " + std::to_string(progArgs.getBlockSize() ) );

    const S3ProgArgs& s3Args = progArgs.getS3Args();

    if(s3Args.getDoS3AclPutInline() || s3Args.getUseS3SSE() || !s3Args.getS3SSECKey().empty() ||
        !s3Args.getS3SSEKMSKey().empty() || !s3Args.getS3ChecksumAlgo().empty() )
        throw ProgException("Plugin \"" + pluginArg + "\" does not support inline ACLs, "
            "server-side encryption or checksum algorithms, because its RDMA control requests "
            "do not carry these options.");
}
