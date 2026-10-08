// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef PLUGINS_PLUGINREGISTRY_H_
#define PLUGINS_PLUGINREGISTRY_H_

#include <string>
#include <vector>

#include "plugins/Plugin.h"

typedef std::vector<Plugin*> PluginVec;

/**
 * Registers the plugin instance at program start. Use once in the plugin's .cpp file; the plugin
 * class needs a static getInstance() method.
 */
#define ELB_REGISTER_PLUGIN(PLUGINCLASS) \
    static const bool PLUGINCLASS ## Registered = \
        PluginRegistry::add(&PLUGINCLASS::getInstance() );


/**
 * All plugins compiled into this binary, in registration order.
 */
class PluginRegistry
{
    public:
        static Plugin* find(const std::string& name);
        static std::string getNamesStr(const std::string& separator = ", ");

        static PluginVec& getAll()
        {
            static PluginVec plugins; // (function-local to be safe for static init order)
            return plugins;
        }

        static bool add(Plugin* plugin)
        {
            getAll().push_back(plugin);
            return true;
        }
};


#endif /* PLUGINS_PLUGINREGISTRY_H_ */
