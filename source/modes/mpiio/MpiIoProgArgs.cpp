// SPDX-FileCopyrightText: 2020-2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <climits>
#include <iostream>

#include "Logger.h"
#include "modes/mpiio/MpiIoProgArgs.h"
#include "ProgArgs.h"
#include "toolkits/TerminalTk.h"

#define ENDL                        << std::endl << // just to make help text print lines shorter


void MpiIoProgArgs::defineArgs(bpo::options_description& generic,
    bpo::options_description& hidden)
{
    generic.add_options()
        (ARG_MPICOLL_LONG, bpo::bool_switch(&this->useCollective),
            "Use collective MPI-IO calls (MPI_File_write_at_all/read_at_all) in MPI-IO mode. "
            "Requires a single shared file as benchmark path. All threads with the same index "
            "across the service ranks form one collective group and do the same number of I/Os, "
            "so the file size gets reduced to a multiple of block size times total thread count "
            "if necessary. See \"--" ARG_HELPMPIIO_LONG "\" for details.")
        (ARG_MPIHINTS_LONG, bpo::value(&this->hintsStr),
            "Comma-separated MPI-IO hints as key=value pairs for MPI_File_open in MPI-IO mode, "
            "e.g. \"romio_cb_write=enable,striping_unit=1048576\". Which hints have an effect "
            "depends on the MPI library and the file system.")
        (ARG_MPIIO_LONG, bpo::bool_switch(&this->useMpiIo),
            "Read and write file contents through the MPI-IO API (MPI_File_*). Launch via "
            "mpirun: rank 0 is the coordinator, all other ranks are the service instances that "
            "do the I/O; without mpirun this is a standalone run. See \"--" ARG_HELPMPIIO_LONG
            "\" for details.")
    ;
}

void MpiIoProgArgs::defineDefaults()
{
    this->hintsStr = "";
    this->useCollective = false;
    this->useMpiIo = false;
}

/**
 * Checks on the coordinator and in standalone mode. Services get the same options from the
 * coordinator, so the checks that depend on the benchmark paths are in checkPathDependentArgs().
 *
 * @throw ProgException on invalid combination.
 */
void MpiIoProgArgs::checkArgs(const ProgArgs& progArgs)
{
    if(useCollective && !useMpiIo)
        throw ProgException("\"--" ARG_MPICOLL_LONG "\" requires \"--" ARG_MPIIO_LONG "\".");

    if(!hintsStr.empty() && !useMpiIo)
        throw ProgException("\"--" ARG_MPIHINTS_LONG "\" requires \"--" ARG_MPIIO_LONG "\".");

    if(!useMpiIo)
        return;

#ifndef MPI_SUPPORT
    throw ProgException("MPI-IO mode selected, but this executable was built without MPI "
        "support. (Build with MPI_SUPPORT=1.)");
#endif

    if(progArgs.benchMode != BenchMode_MPIIO)
        throw ProgException("\"--" ARG_MPIIO_LONG "\" cannot be combined with another benchmark "
            "mode (S3, HDFS, netbench, SPDK).");

    if(!progArgs.gpuIDsStr.empty() )
        throw ProgException("GPU buffers are not supported in MPI-IO mode.");

    if(progArgs.ioDepth > 1)
        throw ProgException("\"--" ARG_IODEPTH_LONG "\" is not supported in MPI-IO mode, "
            "because its MPI-IO calls are synchronous.");

    if(progArgs.useMmap)
        throw ProgException("\"--" ARG_MMAP_LONG "\" is not supported in MPI-IO mode.");

    if(progArgs.useDirectIO)
        throw ProgException("\"--" ARG_DIRECTIO_LONG "\" is not available in MPI-IO mode. Use "
            "\"--" ARG_MPIHINTS_LONG "\" for file system specific hints instead.");

    if(progArgs.blockSize > (size_t)INT_MAX)
        throw ProgException("Block size exceeds the maximum for MPI-IO calls. "
            "Max: " + std::to_string(INT_MAX) );

    // hints must be key=value pairs
    for(size_t pos = 0; pos < hintsStr.size(); )
    {
        const size_t end = hintsStr.find(',', pos);
        const std::string pair = hintsStr.substr(pos,
            (end == std::string::npos) ? std::string::npos : (end - pos) );
        const size_t eq = pair.find('=');

        if( (eq == std::string::npos) || !eq || (eq == pair.size() - 1) )
            throw ProgException("Invalid MPI-IO hint, expected \"key=value\": \"" + pair + "\"");

        pos = (end == std::string::npos) ? hintsStr.size() : (end + 1);
    }

    if(!useCollective)
        return;

    /* collective: every thread of a group makes exactly one collective call per block, so
        anything that makes the call sequence differ between threads is out */

    if(progArgs.rwMixReadPercent || progArgs.numRWMixReadThreads)
        throw ProgException("A read/write mix is not possible with collective MPI-IO, because "
            "all threads of a collective group have to make the same calls.");

    if(progArgs.integrityCheckSalt || progArgs.doReadInline)
        throw ProgException("\"--" ARG_INTEGRITYCHECK_LONG "\" and \"--" ARG_READINLINE_LONG "\" "
            "are not possible with collective MPI-IO, because the read-back after each write "
            "would need a second collective call per block. Use independent MPI-IO for "
            "verification.");

    if(progArgs.blockSizeMix.isMix() || progArgs.useRandomUnaligned)
        throw ProgException("Collective MPI-IO requires a single fixed block size, so a block "
            "size mix and \"--" ARG_NORANDOMALIGN_LONG "\" are not possible.");

    if(progArgs.numThreads > 1)
        LOGGER(Log_NORMAL, "NOTE: Collective MPI-IO with more than one thread per service: the "
            "threads of a service make their collective calls concurrently on separate "
            "communicators. Some MPI libraries serialize all MPI-IO calls of a process (e.g. "
            "MPICH's ROMIO), which can make such a run hang. If that happens, use \"-"
            ARG_NUMTHREADS_SHORT " 1\" with more ranks per host instead." << std::endl);
}

/**
 * Checks that need the benchmark path type, so they also run on the services. For collective
 * I/O this reduces the file size so that all threads get the same number of blocks.
 *
 * @throw ProgException on invalid combination.
 */
void MpiIoProgArgs::checkPathDependentArgs(ProgArgs& progArgs)
{
    if(!useMpiIo)
        return;

    if(progArgs.benchPathType == BenchPathType_BLOCKDEV)
        throw ProgException("Block devices are not supported in MPI-IO mode.");

    if(!useCollective)
        return;

    if(progArgs.benchPathType == BenchPathType_DIR)
        throw ProgException("Collective MPI-IO (\"--" ARG_MPICOLL_LONG "\") requires a shared "
            "file as benchmark path. Files per thread (directory mode) are independent I/O.");

    if(progArgs.benchPathsVec.size() > 1)
        throw ProgException("Collective MPI-IO (\"--" ARG_MPICOLL_LONG "\") requires a single "
            "shared file as benchmark path, because all threads of a collective group have to "
            "work on the same file.");

    if(!progArgs.runCreateFilesPhase && !progArgs.runReadPhase)
        return;

    const uint64_t blockSetSize = progArgs.blockSize * progArgs.numDataSetThreads;

    if(progArgs.fileSize < blockSetSize)
        throw ProgException("File size is too small for collective MPI-IO, where every thread "
            "needs at least one block. "
            "File size: " + std::to_string(progArgs.fileSize) + "; "
            "Block size: " + std::to_string(progArgs.blockSize) + "; "
            "Dataset thread count: " + std::to_string(progArgs.numDataSetThreads) );

    if(progArgs.fileSize % blockSetSize)
    {
        const uint64_t newFileSize = progArgs.fileSize - (progArgs.fileSize % blockSetSize);

        LOGGER(Log_NORMAL, "NOTE: Collective MPI-IO needs the same number of blocks for every "
            "thread. Reducing file size to a multiple of block size times dataset thread count. "
            "Old: " << progArgs.fileSize << "; "
            "New: " << newFileSize << std::endl);

        progArgs.fileSize = newFileSize;
    }
}

void MpiIoProgArgs::printHelp()
{
    std::cout <<
        "I/O through the MPI-IO API, independent or collective." ENDL
        std::endl <<
        "Usage:" ENDL
        "  Launch via mpirun with one rank more than the number of I/O ranks. Rank 0 is the" ENDL
        "  coordinator, all other ranks run an elbencho service and do the actual I/O, each" ENDL
        "  with \"-" ARG_NUMTHREADS_SHORT "\" threads:" ENDL
        "    $ mpirun -np <N+1> " EXE_NAME " --" ARG_MPIIO_LONG " [OPTIONS] PATH" ENDL
        "  Without mpirun, this is a standalone run with a single I/O rank." ENDL
        std::endl <<
        "Each worker thread is a separate MPI-IO participant: the threads with the same index" ENDL
        "on all I/O ranks form one group with its own communicator and open the file" ENDL
        "collectively, so a run with 4 I/O ranks and \"-t 8\" has 8 groups of 4 participants." ENDL
        "Only the file data path uses MPI-IO; directory, stat and delete phases stay POSIX." ENDL
        std::endl;

    bpo::options_description basicDescription(
        "Basic Options", TerminalTk::getTerminalLineLength(80) );

    basicDescription.add_options()
        (ARG_MPIIO_LONG, bpo::bool_switch(),
            "Read and write file contents through MPI_File_write_at/read_at.")
        (ARG_MPICOLL_LONG, bpo::bool_switch(),
            "Collective I/O through MPI_File_write_at_all/read_at_all. Requires a single shared "
            "file as benchmark path. Every thread of a collective group makes the same number of "
            "calls, so the file size is reduced to a multiple of block size times total thread "
            "count if necessary. A \"--" ARG_TIMELIMITSECS_LONG "\" ends the phase after the "
            "limit with empty collective calls for the remaining blocks, so that no thread waits "
            "forever for its group.")
        (ARG_MPIHINTS_LONG, bpo::value<std::string>(),
            "Comma-separated key=value hints for MPI_File_open, e.g. "
            "\"romio_cb_write=enable,striping_unit=1048576\".")
        (ARG_CREATEFILES_LONG "," ARG_CREATEFILES_SHORT, bpo::bool_switch(),
            "Write phase. In file mode the shared file is created if it does not exist.")
        (ARG_READ_LONG "," ARG_READ_SHORT, bpo::bool_switch(),
            "Read phase.")
        (ARG_FILESIZE_LONG "," ARG_FILESIZE_SHORT, bpo::value<std::string>(),
            "Shared file size, or size per file in directory mode. (Default: 0)")
        (ARG_BLOCK_LONG "," ARG_BLOCK_SHORT, bpo::value<std::string>(),
            "Bytes per MPI-IO call. (Default: 1M)")
        (ARG_NUMTHREADS_LONG "," ARG_NUMTHREADS_SHORT, bpo::value<size_t>(),
            "Number of threads per I/O rank. (Default: 1)")
        (ARG_STRIDEDACCESS_LONG, bpo::bool_switch(),
            "Interleave the blocks of the threads instead of giving each thread a contiguous "
            "range of the shared file, i.e. thread N writes blocks N, N+T, N+2T, ... for T "
            "total threads. With --" ARG_MPICOLL_LONG " each collective group owns a contiguous "
            "region of the file and only its members interleave within that region, so each "
            "collective call covers one adjacent run of blocks and a group's calls follow each "
            "other without gaps (see docs/mpiio.md).")
        (ARG_RANDOMOFFSETS_LONG, bpo::bool_switch(),
            "Random offsets within each thread's range of the shared file.")
    ;

    std::cout << basicDescription << std::endl;

    std::cout <<
        "Examples:" ENDL
        "  Shared file, 4 I/O ranks with 8 threads each, independent writes and reads:" ENDL
        "    $ mpirun -np 5 " EXE_NAME " --" ARG_MPIIO_LONG " -" ARG_CREATEFILES_SHORT
            " -" ARG_READ_SHORT " -" ARG_NUMTHREADS_SHORT " 8 -" ARG_BLOCK_SHORT " 1m "
            "-" ARG_FILESIZE_SHORT " 32g /mnt/shared/file" ENDL
        std::endl <<
        "  The same with collective I/O and interleaved blocks:" ENDL
        "    $ mpirun -np 5 " EXE_NAME " --" ARG_MPIIO_LONG " --" ARG_MPICOLL_LONG
            " --" ARG_STRIDEDACCESS_LONG " -" ARG_CREATEFILES_SHORT " -" ARG_READ_SHORT
            " -" ARG_NUMTHREADS_SHORT " 8 \\" ENDL
        "        -" ARG_BLOCK_SHORT " 1m -" ARG_FILESIZE_SHORT " 32g /mnt/shared/file" ENDL
        std::endl <<
        "  One file per thread (file-per-process pattern), 100 files of 1 MiB per thread:" ENDL
        "    $ mpirun -np 5 " EXE_NAME " --" ARG_MPIIO_LONG " -" ARG_CREATEDIRS_SHORT
            " -" ARG_CREATEFILES_SHORT " -" ARG_READ_SHORT " -" ARG_DELETEFILES_SHORT
            " -" ARG_DELETEDIRS_SHORT " -" ARG_NUMTHREADS_SHORT " 8 -" ARG_NUMDIRS_SHORT " 1 "
            "-" ARG_NUMFILES_SHORT " 100 \\" ENDL
        "        -" ARG_FILESIZE_SHORT " 1m -" ARG_BLOCK_SHORT " 1m /mnt/shared/dir" ENDL
        std::endl <<
        "  Persistent services: start one \"--" ARG_RUNASSERVICE_LONG " --" ARG_MPIIO_LONG
            "\" rank per host via mpirun, then run" ENDL
        "  benchmarks against them from any host with \"--" ARG_MPIIO_LONG " --" ARG_HOSTS_LONG
            " ...\" like in distributed mode." ENDL
        std::endl <<
        "See \"--" ARG_HELPLARGE_LONG "\" for the shared file options and \"--"
            ARG_HELPMULTIFILE_LONG "\" for the" ENDL
        "directory mode options, which all apply here as well." << std::endl;
}

void MpiIoProgArgs::printBuildInfo(std::ostringstream& includedStream,
    std::ostringstream& notIncludedStream)
{
#ifdef MPI_SUPPORT
    includedStream << FEATURE_NAME_MPIIO << " ";
#else
    notIncludedStream << FEATURE_NAME_MPIIO << " ";
#endif
}

void MpiIoProgArgs::setFromPropertyTree(const bpt::ptree& tree)
{
    hintsStr = tree.get<std::string>(ARG_MPIHINTS_LONG);
    useCollective = tree.get<bool>(ARG_MPICOLL_LONG);
    useMpiIo = tree.get<bool>(ARG_MPIIO_LONG);
}

void MpiIoProgArgs::getAsPropertyTree(bpt::ptree& outTree) const
{
    outTree.put(ARG_MPIHINTS_LONG, hintsStr);
    outTree.put(ARG_MPICOLL_LONG, useCollective);
    outTree.put(ARG_MPIIO_LONG, useMpiIo);
}
