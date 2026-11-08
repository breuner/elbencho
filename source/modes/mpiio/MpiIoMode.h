// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef MODES_MPIIO_MPIIOMODE_H_
#define MODES_MPIIO_MPIIOMODE_H_

#include <string>
#include <sys/types.h>
#include <vector>

#include "Common.h"

#ifdef MPI_SUPPORT
    #include <mpi.h>
#endif

class LocalWorker;
class MpiIoProgArgs;
class ProgArgs;

/**
 * The MPI-IO mode of a worker thread: opens the files through MPI_File_open and moves the data
 * through MPI_File_write_at/read_at or the collective "_all" variants. Works on the worker's
 * buffers and stats; everything else (offsets, phases, directory handling) stays in LocalWorker.
 *
 * Each worker thread has its own communicator: the threads with the same index on all I/O ranks
 * form one group, which opens shared files collectively (see getThreadComm). Dir mode files
 * (one file per thread) are opened on MPI_COMM_SELF.
 *
 * Collective phases need every thread of a group to make the same number of calls, which the
 * ProgArgs checks ensure for the full phase. A thread that ends early (interruption, error)
 * finishes its planned call count with empty calls in finishCollectivePhase(). In an infinite I/O
 * loop, the group decides together after each repeat whether to start another one
 * (continueRepeat()).
 */
class MpiIoMode
{
    public:
        explicit MpiIoMode(LocalWorker& worker);

        void init();
        void uninit();
        void initPhase();
        void finishCollectivePhase();
        bool continueRepeat(bool stop);

        ssize_t readAt(size_t fileHandleIdx, void* buf, size_t nbytes, off_t offset);
        ssize_t writeAt(size_t fileHandleIdx, void* buf, size_t nbytes, off_t offset);

        void setCurrentFileIdx(size_t fileIdx);
        void openDirModeFile(const std::string& path, int openFlags);
        void prepDirModeFile(const std::string& path, uint64_t fileLen);
        void closeCurrentFile(const std::string& path, bool throwOnError);
        void statCurrentFile(const std::string& path);

    private:
        LocalWorker& worker;
        ProgArgs* progArgs; // shortcut for worker member
        const MpiIoProgArgs& mpiIoArgs; // shortcut for progArgs member
        size_t workerRank; // shortcut for worker member

#ifdef MPI_SUPPORT
        typedef int (*MpiReadFn)(MPI_File, MPI_Offset, void*, int, MPI_Datatype, MPI_Status*);
        typedef int (*MpiWriteFn)(MPI_File, MPI_Offset, const void*, int, MPI_Datatype,
            MPI_Status*);

        MPI_Comm comm{MPI_COMM_NULL}; // this thread's group communicator (process-wide cache)
        MPI_Info info{MPI_INFO_NULL}; // hints for MPI_File_open
        std::vector<MPI_File> files; // file mode: the open bench paths, indexed like in progArgs
        MPI_File currentFile{MPI_FILE_NULL}; // dir mode and sequential file mode: current file
        MPI_File* handles{nullptr}; // per-phase lookup by fileHandleIdx (like fdVecPtr)
        MpiReadFn readFn{nullptr}; // independent or collective variant, set per phase
        MpiWriteFn writeFn{nullptr};
        const char* readOpName{""}; // for the ops log
        const char* writeOpName{""};
        bool collectivePhase{false}; // current phase does collective I/O on the shared file
        bool phaseIsWrite{false};
        uint64_t plannedOps{0}; // collective phase: calls every thread of the group makes
        uint64_t numOpsDone{0}; // collective phase: calls made so far by this thread
        bool repeatVotePending{false}; // infinite loop: group vote for this repeat still due

        void openFile(const std::string& path, int amode, MPI_Comm fileComm, MPI_File* outFile);
        void closeFile(MPI_File* file, const std::string& path, bool throwOnError);
        void createInfo();
        static std::string errStr(int mpiErr);
        static MPI_Comm getThreadComm(size_t threadIdx);
#endif // MPI_SUPPORT
};

#endif /* MODES_MPIIO_MPIIOMODE_H_ */
