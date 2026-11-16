// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef TOOLKITS_MPI_MPILAUNCHER_H_
#define TOOLKITS_MPI_MPILAUNCHER_H_

#include <functional>

#ifdef MPI_SUPPORT
    #include <mpi.h>
#endif

/**
 * Maps MPI ranks to elbencho roles when elbencho runs under an MPI launcher with "--mpilaunch" on
 * the command line (implied by "--mpiio"): rank 0 is the coordinator, every other rank starts an
 * elbencho service on an OS-chosen port and sends its address to rank 0, which runs the benchmark
 * against them as "--hosts" and releases them through a barrier when it is done. This is
 * independent of the benchmark mode. With a single rank (no mpirun) or with "--service" on the
 * command line nothing is remapped: the process runs standalone or as one persistent service, and
 * all ranks do I/O.
 *
 * Everything here is a no-op when built without MPI_SUPPORT or when neither option is given.
 */
class MpiLauncher
{
    public:
        static void init(int& argc, char**& argv);
        static void serviceListening(unsigned short port, std::function<void()> stopServer);
        static void joinServiceWaiter();
        static void finish();

        static bool isActive();
        static bool isAutoService();

#ifdef MPI_SUPPORT
        static MPI_Comm getIoComm();
#endif

        /**
         * Calls finish() when it goes out of scope, so that the other ranks never wait forever
         * for a rank that failed.
         */
        struct Guard
        {
            ~Guard() { MpiLauncher::finish(); }
        };
};

#endif /* TOOLKITS_MPI_MPILAUNCHER_H_ */
