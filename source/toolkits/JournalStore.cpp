// SPDX-FileCopyrightText: 2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <iomanip>
#include <sys/stat.h>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include "Logger.h"
#include "ProgArgs.h"
#include "ProgException.h"
#include "toolkits/Journal.h"
#include "toolkits/JournalStore.h"

namespace bpt = boost::property_tree;

#define JOURNAL_SIDECAR_SUFFIX     ".journal.json"

JournalStore::JournalStore() {}

JournalStore::~JournalStore()
{
    closeAll();
}

std::vector<JournalStore::SidecarInfo> JournalStore::scanSidecars(
    const std::string& journalDirPath)
{
    std::vector<SidecarInfo> result;

    DIR* dir = opendir(journalDirPath.c_str() );
    if(!dir)
        throw ProgException("Unable to open journal directory. "
            "Path: " + journalDirPath + "; "
            "SysErr: " + strerror(errno) );

    const std::string suffix = JOURNAL_SIDECAR_SUFFIX;

    struct dirent* entry;
    while( (entry = readdir(dir) ) != NULL)
    {
        const std::string name = entry->d_name;

        if( (name.size() <= suffix.size() ) ||
            (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) )
            continue; // not a "*.journal.json" sidecar file

        const std::string sidecarPath = journalDirPath + "/" + name;

        bpt::ptree tree;

        try
        {
            bpt::read_json(sidecarPath, tree);
        }
        catch(const bpt::json_parser_error& e)
        {
            closedir(dir);
            throw ProgException("Unable to parse journal sidecar file. "
                "Path: " + sidecarPath + "; "
                "Error: " + e.what() );
        }

        SidecarInfo info;
        info.sidecarPath = sidecarPath;

        try
        {
            info.target = tree.get<std::string>("target");
            info.journalBlockSize = tree.get<uint64_t>("journalBlockSize");
            info.targetOffset = tree.get<uint64_t>("targetOffset");
            info.targetSize = tree.get<uint64_t>("targetSize");
            info.journalSeed = tree.get<uint64_t>("journalSeed");
            info.binaryFileName = tree.get<std::string>("binaryFileName");
        }
        catch(const bpt::ptree_error& e)
        {
            closedir(dir);
            throw ProgException("Journal sidecar file is missing a required field or has an "
                "invalid value. "
                "Path: " + sidecarPath + "; "
                "Error: " + e.what() );
        }

        info.binaryPath = journalDirPath + "/" + info.binaryFileName;

        result.push_back(info);
    }

    closedir(dir);

    return result;
}

void JournalStore::init(const ProgArgs& progArgs)
{
    const std::string& journalDirPath = progArgs.getJournalDirStr();

    struct stat statBuf;
    if( (stat(journalDirPath.c_str(), &statBuf) == -1) || !S_ISDIR(statBuf.st_mode) )
        throw ProgException("Journal directory (\"--" ARG_JOURNALDIR_LONG "\") does not exist "
            "or is not a directory. "
            "Path: " + journalDirPath );

    std::vector<SidecarInfo> sidecars = scanSidecars(journalDirPath);

    const StringVec& benchPaths = progArgs.getBenchPaths();
    const uint64_t runJournalBlockSize = progArgs.getJournalBlockSize();
    const uint64_t runOffset = progArgs.getFileOffset();
    const uint64_t runEndOffset = runOffset + progArgs.getFileSize();

    journalsVec.clear();
    journalsVec.reserve(benchPaths.size() );

    for(const std::string& targetPath : benchPaths)
    {
        std::vector<const SidecarInfo*> matches;

        for(const SidecarInfo& sidecar : sidecars)
            if(sidecar.target == targetPath)
                matches.push_back(&sidecar);

        if(matches.size() > 1)
            throw ProgException("Ambiguous journal sidecars found for target (more than one "
                "sidecar's \"target\" field matches). "
                "Target: " + targetPath + "; "
                "Num matching sidecars: " + std::to_string(matches.size() ) );

        std::unique_ptr<Journal> journal;

        if(matches.empty() )
        { // no existing journal for this target yet, so create a fresh one
            journal = Journal::createNew(journalDirPath, targetPath, runJournalBlockSize,
                runOffset, progArgs.getFileSize() );

            LOGGER(Log_NORMAL, "NOTE: Created new journal for target. "
                "Target: " << targetPath << std::endl);
        }
        else
        {
            const SidecarInfo& sidecar = *matches[0];

            if(sidecar.journalBlockSize != runJournalBlockSize)
                throw ProgException("Existing journal's block size does not match "
                    "\"--" ARG_JOURNALBLOCK_LONG "\" given for this run. All journals used in a "
                    "run must share the same journal block size. "
                    "Target: " + targetPath + "; "
                    "Existing journal block size: " +
                        std::to_string(sidecar.journalBlockSize) + "; "
                    "Given journal block size: " + std::to_string(runJournalBlockSize) );

            const uint64_t sidecarEndOffset = sidecar.targetOffset + sidecar.targetSize;

            if( (runOffset < sidecar.targetOffset) || (runEndOffset > sidecarEndOffset) )
                throw ProgException("Requested \"--" ARG_FILEOFFSET_LONG "\"/"
                    "\"--" ARG_FILESIZE_LONG "\" range exceeds the range that the existing "
                    "journal for this target was originally created for. "
                    "Target: " + targetPath + "; "
                    "Requested range: [" + std::to_string(runOffset) + ", " +
                        std::to_string(runEndOffset) + "); "
                    "Journal range: [" + std::to_string(sidecar.targetOffset) + ", " +
                        std::to_string(sidecarEndOffset) + ")" );

            journal = Journal::openExisting(sidecar.binaryPath, sidecar.sidecarPath,
                sidecar.journalBlockSize, sidecar.targetOffset, sidecar.targetSize,
                sidecar.journalSeed);

            /* the percentage tells the user how much of this journal can actually be
                verified, which is the interesting part after a crash or a partial run */
            LOGGER(Log_NORMAL, "NOTE: Resumed existing journal for target. "
                "Target: " << targetPath << "; "
                "Verifiable: " << std::setprecision(1) << std::fixed <<
                    journal->getVerifiablePercent() << "%" << std::endl);
        }

        journal->initRunOptions(progArgs.getBlockSizeMix().getMaxSize(),
            progArgs.getJournalSyncEnabled() );

        journalsVec.push_back(std::move(journal) );
    }

    enabled = true;
}

Journal* JournalStore::getJournalForTarget(size_t targetIdx) const
{
    if(!enabled || (targetIdx >= journalsVec.size() ) )
        return nullptr;

    return journalsVec[targetIdx].get();
}

void JournalStore::closeAll()
{
    journalsVec.clear(); /* unique_ptr destructors handle munmap/close per Journal (plus a
        final msync, but only when "--journalsync" was given) */
    enabled = false;
}
