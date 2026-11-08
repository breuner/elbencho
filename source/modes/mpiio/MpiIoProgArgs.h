// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef MODES_MPIIO_MPIIOPROGARGS_H_
#define MODES_MPIIO_MPIIOPROGARGS_H_

#include <boost/program_options.hpp>
#include <boost/property_tree/ptree.hpp>
#include <sstream>
#include <string>

namespace bpo = boost::program_options;
namespace bpt = boost::property_tree;

class ProgArgs;

#define ARG_MPIIO_LONG                  "mpiio"
#define ARG_MPICOLL_LONG                "mpicoll"
#define ARG_MPIHINTS_LONG               "mpihints"

#define FEATURE_NAME_MPIIO              "mpiio"

/**
 * The MPI-IO benchmark mode options: definition, defaults, checks, help and the transfer to
 * services. Owned by ProgArgs as mpiIoArgs, like the other mode option classes.
 *
 * In MPI-IO mode only the file data path goes through MPI (MPI_File_open/read_at/write_at/close,
 * or the collective "_all" variants with "--mpicoll"); directory phases, stat, delete and the
 * preparation of shared files stay POSIX. Each worker thread is one MPI-IO participant with its
 * own communicator (see MpiIoMode); the ranks are launched by mpirun (see MpiIoLauncher).
 */
class MpiIoProgArgs
{
    public:
        void defineArgs(bpo::options_description& generic, bpo::options_description& hidden);
        void defineDefaults();
        void checkArgs(const ProgArgs& progArgs);
        void checkPathDependentArgs(ProgArgs& progArgs);
        void printHelp();
        void printBuildInfo(std::ostringstream& includedStream,
            std::ostringstream& notIncludedStream);
        void setFromPropertyTree(const bpt::ptree& tree);
        void getAsPropertyTree(bpt::ptree& outTree) const;

    private:
        // config options in alphabetic order...

        std::string hintsStr; // "key=value,..." for the MPI_Info of MPI_File_open
        bool useCollective; // collective MPI_File_*_at_all calls instead of independent ones
        bool useMpiIo; // run the MPI-IO benchmark mode

    // inliners
    public:
        const std::string& getHintsStr() const { return hintsStr; }
        bool getUseCollective() const { return useCollective; }
        bool getUseMpiIo() const { return useMpiIo; }
};

#endif /* MODES_MPIIO_MPIIOPROGARGS_H_ */
