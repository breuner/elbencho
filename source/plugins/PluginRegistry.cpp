// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "plugins/PluginRegistry.h"

/**
 * @return NULL if no plugin with the given name is compiled in.
 */
Plugin* PluginRegistry::find(const std::string& name)
{
    for(Plugin* plugin : getAll() )
        if(name == plugin->getName() )
            return plugin;

    return NULL;
}

/**
 * @return names of all compiled-in plugins or "-" if there are none.
 */
std::string PluginRegistry::getNamesStr(const std::string& separator)
{
    std::string names;

    for(Plugin* plugin : getAll() )
        names += (names.empty() ? "" : separator) + plugin->getName();

    return names.empty() ? "-" : names;
}
