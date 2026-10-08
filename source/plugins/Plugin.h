// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef PLUGINS_PLUGIN_H_
#define PLUGINS_PLUGIN_H_

#include <boost/program_options.hpp>
#include <boost/property_tree/ptree.hpp>

namespace bpo = boost::program_options;
namespace bpt = boost::property_tree;

class ProgArgs;


/**
 * Interface of a contributed plugin (see contrib/plugins/README.md).
 *
 * A plugin gets compiled in via its ELB_PLUGIN_<NAME>=1 make option and registers itself with
 * the PluginRegistry. At runtime it is inactive until the user selects it via "--plugins". Its
 * options are parsed together with the core options and get transferred to service instances
 * when the plugin is active. None of these methods is called in the I/O path; a plugin that
 * changes the I/O path does so at compile time (e.g. via ELB_S3_TRANSPORT_HEADER).
 */
class Plugin
{
    public:
        virtual ~Plugin() = default;

        virtual const char* getName() const = 0; // as given by the user with "--plugins"
        virtual const char* getDescription() const = 0; // one line for help & version output

        virtual void defineArgs(bpo::options_description& desc) {}
        virtual void defineDefaults() {}

        /**
         * Called for every compiled-in plugin after the core args have been checked, also when
         * this plugin is not active.
         *
         * @throw ProgException if a problem is found.
         */
        virtual void checkArgs(const ProgArgs& progArgs) {}

        // service transfer of this plugin's options, only called when the plugin is active
        virtual void getAsPropertyTree(bpt::ptree& outTree) const {}
        virtual void setFromPropertyTree(const bpt::ptree& tree) {}

        bool isActive() const { return active; }
        void setActive(bool active) { this->active = active; }

    private:
        bool active{false};
};


#endif /* PLUGINS_PLUGIN_H_ */
