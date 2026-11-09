// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "S3RdmaDdnPlugin.h"

#include "plugins/PluginRegistry.h"
#include "ProgArgs.h"
#include "ProgException.h"

ELB_REGISTER_PLUGIN(S3RdmaDdnPlugin)


void S3RdmaDdnPlugin::defineArgs(bpo::options_description& desc)
{
    desc.add_options()
        (ARG_S3RDMATIMEOUT_LONG, bpo::value(&this->requestTimeoutMS),
            "Timeout in milliseconds for a single RDMA request attempt, including connection "
            "setup and waiting for the response. A request that times out gets retried "
            "according to \"--" ARG_S3RDMARETRIES_LONG "\". "
            "(Default: " STRINGIZE(S3RDMA_DEF_TIMEOUT_MS) ")")
        (ARG_S3RDMACONNTIMEOUT_LONG, bpo::value(&this->connectTimeoutMS),
            "Timeout in milliseconds for establishing the connection of an RDMA request. "
            "(Default: " STRINGIZE(S3RDMA_DEF_CONNTIMEOUT_MS) ")")
        (ARG_S3RDMARETRIES_LONG, bpo::value(&this->numRetries),
            "Number of retries for an RDMA request that failed with a network error, timeout "
            "or a retryable HTTP status (429, 500, 502, 503, 504). 0 disables retries. "
            "Combine with \"--" ARG_S3IGNOREERRORS_LONG "\" to keep running when a request "
            "still fails after all retries. A summary of retries and failures is printed at "
            "the end. (Default: " STRINGIZE(S3RDMA_DEF_RETRIES) ")")
        (ARG_S3RDMARETRYDELAY_LONG, bpo::value(&this->retryDelayMS),
            "Delay in milliseconds before the first retry of an RDMA request. The delay "
            "doubles with every further retry, up to 2000ms. "
            "(Default: " STRINGIZE(S3RDMA_DEF_RETRYDELAY_MS) ")")
    ;
}

void S3RdmaDdnPlugin::defineDefaults()
{
    requestTimeoutMS = S3RDMA_DEF_TIMEOUT_MS;
    connectTimeoutMS = S3RDMA_DEF_CONNTIMEOUT_MS;
    numRetries = S3RDMA_DEF_RETRIES;
    retryDelayMS = S3RDMA_DEF_RETRYDELAY_MS;
}

void S3RdmaDdnPlugin::checkArgs(const ProgArgs& progArgs)
{
    if(!isActive() )
        return;

    const std::string pluginArg = std::string("--" ARG_PLUGINS_LONG " ") + getName();

    if(progArgs.getBenchMode() != BenchMode_S3)
        throw ProgException("Plugin \"" + pluginArg + "\" can only be used with S3.");

    const S3ProgArgs& s3Args = progArgs.getS3Args();

    if(s3Args.getDoS3AclPutInline() || s3Args.getUseS3SSE() || !s3Args.getS3SSECKey().empty() ||
        !s3Args.getS3SSEKMSKey().empty() || !s3Args.getS3ChecksumAlgo().empty() )
        throw ProgException("Plugin \"" + pluginArg + "\" does not support inline ACLs, "
            "server-side encryption or checksum algorithms, because its RDMA control requests "
            "do not carry these options.");

    if(!requestTimeoutMS)
        throw ProgException("Option \"--" ARG_S3RDMATIMEOUT_LONG "\" must not be 0.");

    if(!connectTimeoutMS)
        throw ProgException("Option \"--" ARG_S3RDMACONNTIMEOUT_LONG "\" must not be 0.");
}

void S3RdmaDdnPlugin::getAsPropertyTree(bpt::ptree& outTree) const
{
    outTree.put(ARG_S3RDMATIMEOUT_LONG, requestTimeoutMS);
    outTree.put(ARG_S3RDMACONNTIMEOUT_LONG, connectTimeoutMS);
    outTree.put(ARG_S3RDMARETRIES_LONG, numRetries);
    outTree.put(ARG_S3RDMARETRYDELAY_LONG, retryDelayMS);
}

void S3RdmaDdnPlugin::setFromPropertyTree(const bpt::ptree& tree)
{
    requestTimeoutMS = tree.get<size_t>(ARG_S3RDMATIMEOUT_LONG);
    connectTimeoutMS = tree.get<size_t>(ARG_S3RDMACONNTIMEOUT_LONG);
    numRetries = tree.get<size_t>(ARG_S3RDMARETRIES_LONG);
    retryDelayMS = tree.get<size_t>(ARG_S3RDMARETRYDELAY_LONG);
}
