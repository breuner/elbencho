// SPDX-FileCopyrightText: 2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef TOOLKITS_JOURNALSTORE_H_
#define TOOLKITS_JOURNALSTORE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class ProgArgs; // forward declaration
class Journal; // forward declaration

/**
 * Run-scoped owner of all per-target Journal instances (see "--journaldir"). Embedded as a member
 * of ProgArgs, the only object with full run-long visibility of the target list; LocalWorker
 * threads reach individual Journal instances through ProgArgs::getJournalForTarget().
 */
class JournalStore
{
    public:
        // constructor/destructor defined in .cpp: needs Journal's complete type for journalsVec
        JournalStore();
        ~JournalStore();

        JournalStore(const JournalStore&) = delete;
        JournalStore& operator=(const JournalStore&) = delete;

        /**
         * Scans progArgs' journal directory for existing sidecars, matches them against
         * progArgs' current target list by the sidecar's "target" field, opens matches
         * (validating journal block size and offset/size containment), and creates fresh
         * journal+sidecar pairs for targets with no matching sidecar.
         *
         * A target with no matching sidecar is not an error - that is how a journal comes into
         * existence. A sidecar whose binary journal file is gone is one.
         *
         * @throw ProgException on any error (corrupt or incomplete sidecar, missing binary
         *  journal, ambiguous match, inconsistent journal block size, requested range outside
         *  the journal's stored range).
         */
        void init(const ProgArgs& progArgs);

        // nullptr if journaling is not enabled or targetIdx is out of range
        Journal* getJournalForTarget(size_t targetIdx) const;

        bool isEnabled() const { return enabled; }

        void closeAll(); // idempotent

    private:
        bool enabled{false};
        std::vector<std::unique_ptr<Journal> > journalsVec; // parallel to ProgArgs::benchPathsVec

        struct SidecarInfo
        {
            std::string sidecarPath;
            std::string binaryPath;
            std::string binaryFileName;
            std::string target;
            uint64_t journalBlockSize{0};
            uint64_t targetOffset{0};
            uint64_t targetSize{0};
            uint64_t journalSeed{0};
        };

        std::vector<SidecarInfo> scanSidecars(const std::string& journalDirPath);
};

#endif /* TOOLKITS_JOURNALSTORE_H_ */
