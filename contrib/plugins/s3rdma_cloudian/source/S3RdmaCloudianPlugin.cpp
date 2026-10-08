// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "S3RdmaCloudianPlugin.h"

#include "plugins/PluginRegistry.h"
#include "ProgArgs.h"
#include "ProgException.h"

ELB_REGISTER_PLUGIN(S3RdmaCloudianPlugin)

void S3RdmaCloudianPlugin::defineArgs(bpo::options_description& desc)
{
    desc.add_options()
        (ARG_CUOBJHOSTBUFREG_LONG, bpo::bool_switch(&this->useCuObjHostBufReg),
            "Pre-register host memory buffers with cuObject for RDMA.")
    ;
}

void S3RdmaCloudianPlugin::checkArgs(const ProgArgs& progArgs)
{
    const std::string pluginArg = std::string("--" ARG_PLUGINS_LONG " ") + getName();

    // (the RDMA requests of the SDK fork need a target buffer, also on their TCP fallback path)
    if(progArgs.getS3Args().getUseS3FastRead() )
        throw ProgException("Option \"--" ARG_S3FASTGET_LONG "\" is not available in a build with "
            "plugin " + std::string(getName() ) + ".");

    if(!isActive() )
    {
        if(useCuObjHostBufReg)
            throw ProgException("Option \"--" ARG_CUOBJHOSTBUFREG_LONG "\" requires \"" +
                pluginArg + "\".");

        return;
    }

    if(progArgs.getBenchMode() != BenchMode_S3)
        throw ProgException("Plugin \"" + pluginArg + "\" can only be used with S3.");

    if(progArgs.getUseCuFile() && !progArgs.getUseGPUBufReg() )
        throw ProgException("Plugin \"" + pluginArg + "\" with \"--" ARG_CUFILE_LONG "\" requires "
            "\"--" ARG_GDSBUFREG_LONG "\" (e.g. via \"--" ARG_GPUDIRECTSSTORAGE_LONG "\"), because "
            "the GPU buffers are transferred directly.");

    if(useCuObjHostBufReg && progArgs.getUseGPUBufReg() )
        throw ProgException("Option \"--" ARG_CUOBJHOSTBUFREG_LONG "\" cannot be used together "
            "with \"--" ARG_GDSBUFREG_LONG "\".");

    if(useCuObjHostBufReg && progArgs.getUseCuHostBufReg() )
        throw ProgException("Option \"--" ARG_CUOBJHOSTBUFREG_LONG "\" cannot be used together "
            "with \"--" ARG_CUHOSTBUFREG_LONG "\".");
}

void S3RdmaCloudianPlugin::getAsPropertyTree(bpt::ptree& outTree) const
{
    outTree.put(ARG_CUOBJHOSTBUFREG_LONG, useCuObjHostBufReg);
}

void S3RdmaCloudianPlugin::setFromPropertyTree(const bpt::ptree& tree)
{
    useCuObjHostBufReg = tree.get<bool>(ARG_CUOBJHOSTBUFREG_LONG);
}
