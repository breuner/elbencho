// SPDX-FileCopyrightText: 2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#ifndef TOOLKITS_JOURNAL_H_
#define TOOLKITS_JOURNAL_H_

#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

class Journal;

/**
 * RAII holder for the journal locks acquired for one journal block range (see
 * Journal::acquireRangeExclusive()/acquireRangeShared() ).
 *
 * A journal's lock regions always span at least one max-size I/O (see Journal::initRunOptions() ),
 * so a single I/O can never cover more than two of them. Thus this holds at most two locks in a
 * fixed-size array and needs no heap allocation at all in the hot I/O path.
 */
class JournalRangeGuard
{
    public:
        JournalRangeGuard() = default;
        ~JournalRangeGuard() { release(); }

        JournalRangeGuard(const JournalRangeGuard&) = delete;
        JournalRangeGuard& operator=(const JournalRangeGuard&) = delete;

        void release()
        {
            for(unsigned i = 0; i < numLocks; i++)
            {
                if(isExclusive)
                    locks[i]->unlock();
                else
                    locks[i]->unlock_shared();
            }

            numLocks = 0;
        }

    private:
        std::shared_mutex* locks[2] = {nullptr, nullptr};
        unsigned numLocks{0};
        bool isExclusive{false};

    friend class Journal;
};

/**
 * A write journal for a single target file/block device, used for journaled data verification
 * (see "--journaldir"). Tracks which journalBlockSize-sized blocks of the target's
 * [targetOffset, targetOffset+targetSize) range have been written, using 2 bits per block packed
 * into an mmap'ed binary file of targetSize/(4*journalBlockSize) bytes: 0 means uninitialized (or
 * a write to the block never completed), 1/2/3 are rotating content generations, so rewriting a
 * block changes its expected content and stale content becomes detectable.
 *
 * How far the 0 state survives a crash depends on "--journalsync": without it, for as long as the
 * page cache does, i.e. across a process crash or a target that went away and came back; with it,
 * also across a power failure (see JournalBlockIO::claimForWrite() ).
 *
 * Concurrency is provided by striped std::shared_mutex instances, which gives real, blocking
 * mutual exclusion between readers and writers of the same block, with unlimited concurrent
 * readers of the same block. There is no spinning and no backoff anywhere: a thread that holds no
 * guard of its own simply blocks on the mutex. The single exception is a thread that already
 * holds a guard for an I/O of its own still in flight - it must not block for a second one, so it
 * takes the locks only if they are free and otherwise gets ACTION_DEFER and re-attempts the I/O
 * after one of its own I/Os completed. That deferral exists purely to make circular waits
 * impossible, not to cope with contention (see JournalBlockIO::begin() ).
 *
 * Blocks are grouped into "lock regions" before being mapped to a stripe, and a region holds
 * 2^lockRegionShift blocks, sized in initRunOptions() to cover at least one maximum-size I/O.
 * "Maximum size" is the largest block size of the run's "-b"/"--block" mix, so a region always
 * spans roughly one max-size I/O worth of target range (>= that size and < twice it), whatever
 * journalBlockSize happens to be - the latter only decides how many journal blocks that is. Hence
 * any single I/O, however many journal blocks it spans, only ever needs one or two locks (see
 * JournalRangeGuard), and hence the region size is a per-run value: a later run may resume the
 * same journal with a different "-b" mix.
 *
 * The stripe array itself is a single process-wide one of NUM_STRIPES entries (~3.5 MiB),
 * allocated on first use and shared by every journal of the run. Which stripe a region lands on
 * depends on the journal's own stripeSalt as well as on the region index, so a) the same offset
 * in two different targets maps to different stripes, i.e. targets never serialize against each
 * other at equal offsets, and b) no target is confined to a subset of the array - each journal's
 * regions are scattered over all of it. Coarser regions and stripe collisions between unrelated
 * regions only cost occasional false contention, never correctness.
 */
class Journal
{
    public:
        static std::unique_ptr<Journal> createNew(const std::string& journalDirPath,
            const std::string& targetIdentifier, uint64_t journalBlockSize,
            uint64_t targetOffset, uint64_t targetSize);

        static std::unique_ptr<Journal> openExisting(const std::string& binaryPath,
            const std::string& sidecarPath, uint64_t journalBlockSize, uint64_t targetOffset,
            uint64_t targetSize, uint64_t journalSeed);

        ~Journal();

        Journal(const Journal&) = delete;
        Journal& operator=(const Journal&) = delete;

        void initRunOptions(uint64_t maxIOSize, bool journalSyncEnabled);

        /**
         * @mayBlock true to wait until the locks are available, false to give up instead of
         *  waiting (see JournalBlockIO::begin() for why this matters).
         * @return false only for mayBlock==false when the locks were not immediately available.
         */
        bool acquireRangeExclusive(uint64_t firstBlockIdx, size_t numBlocks, bool mayBlock,
            JournalRangeGuard& outGuard);
        bool acquireRangeShared(uint64_t firstBlockIdx, size_t numBlocks, bool mayBlock,
            JournalRangeGuard& outGuard);

        // Cell access. Caller must hold a guard covering journalBlockIndex.
        uint8_t loadBlockGen(uint64_t journalBlockIndex) const;
        void markBlockBusy(uint64_t journalBlockIndex); // atomically clear cell to 0
        void commitBlockGen(uint64_t journalBlockIndex, uint8_t newGen); // atomically set to 1..3

        // msync of just the pages holding [firstBlockIdx, firstBlockIdx+numBlocks); no-op unless
        // "--journalsync" was given.
        void syncRange(uint64_t firstBlockIdx, size_t numBlocks) const;

        uint64_t getJournalBlockSize() const { return journalBlockSize; }
        uint64_t getJournalSeed() const { return journalSeed; }

        double getVerifiablePercent() const;

        uint64_t offsetToBlockIndex(uint64_t absoluteOffset) const
            { return (absoluteOffset - targetOffset) / journalBlockSize; }

        // never returns 0, so a committed generation is always distinguishable from "uninit/busy"
        static uint8_t nextGen(uint8_t prevGen) { return (prevGen == 0) ? 1 : ( (prevGen % 3) + 1); }

        static uint64_t computeExpectedTileValue(uint64_t blockAlignedOffset, uint64_t journalSeed,
            uint8_t generation);

    private:
        /* number of stripe locks, shared by all journals of this process, so the memory cost of
            locking is O(1) regardless of target sizes and number of targets */
        static constexpr size_t NUM_STRIPES = 65536;
        static constexpr unsigned NUM_STRIPES_SHIFT = 16; // log2(NUM_STRIPES)

        Journal() = default;

        std::string binaryPath;
        std::string sidecarPath;
        int fd{-1};
        uint8_t* mapBase{nullptr};
        size_t mapLength{0};
        uint64_t journalBlockSize{0};
        uint64_t targetOffset{0};
        uint64_t targetSize{0};
        uint64_t numJournalBlocks{0};
        uint64_t journalSeed{0};
        bool journalSyncEnabled{false};
        unsigned lockRegionShift{0}; // journal blocks per lock region, as log2
        uint64_t stripeSalt{0}; // per-journal, so different journals don't collide identically

        void mmapBinaryFile();
        bool acquireRange(uint64_t firstBlockIdx, size_t numBlocks, bool isExclusive,
            bool mayBlock, JournalRangeGuard& outGuard);

        static std::shared_mutex* getStripeLocks();

        size_t blockIndexToStripe(uint64_t journalBlockIndex) const
        {
            const uint64_t regionIdx = (journalBlockIndex >> lockRegionShift) ^ stripeSalt;

            // Fibonacci hashing, taking the high bits (the well-mixed ones)
            return (size_t)( (regionIdx * 0x9E3779B97F4A7C15ULL) >> (64 - NUM_STRIPES_SHIFT) );
        }

        static void cellLocation(uint64_t blockIndex, size_t& outByteIdx, unsigned& outShift,
            uint8_t& outMask)
        {
            outByteIdx = blockIndex / 4;
            outShift = (unsigned)(blockIndex % 4) * 2;
            outMask = (uint8_t)(0x3 << outShift);
        }

        static std::string sanitizeTargetToFileNameBase(const std::string& targetIdentifier);
};

/**
 * The journal side of a single I/O: decides whether the I/O becomes a read, a write, or is
 * skipped entirely, holds the journal lock guard for as long as the I/O is in flight, and carries
 * the per-journal-block content generations that LocalWorker's journal buffer fill/verify
 * functions need.
 *
 * Reused across I/Os (one instance per in-flight I/O slot), so its generations vector stops
 * reallocating once it has grown to the largest block size of the run and the hot path is then
 * allocation-free.
 */
class JournalBlockIO
{
    public:
        enum Intent
        {
            INTENT_WRITE, // plain write
            INTENT_READ_SKIP_UNINIT, /* pure read phase: uninitialized blocks are skipped and
                                        accepted as verified-ok, since nothing was ever written */
            INTENT_READ_OR_INIT, /* rwmix read: an uninitialized block is turned into a write to
                                    initialize it, so later reads can verify it */
        };

        enum Action
        {
            ACTION_READ,
            ACTION_WRITE,
            ACTION_SKIP,
            ACTION_DEFER, /* only for mayBlock==false: the journal locks for this range are
                             currently taken, so this I/O must not be started yet */
        };

        /**
         * Decide the action for this I/O and acquire the matching journal lock guard, which stays
         * held until commit() or release(). For ACTION_SKIP and ACTION_DEFER nothing stays held.
         *
         * @mayBlock whether waiting for the journal locks is allowed. This must be false whenever
         *  the calling thread already holds a journal guard for another I/O of its own - which is
         *  the case for the async I/O engines, where a thread has up to "iodepth" I/Os in flight,
         *  each holding its guard until it completes. Blocking while already holding a guard
         *  would both self-deadlock (a thread locking the same lock region twice) and allow
         *  circular waits between threads. With mayBlock==false the locks are only taken if they
         *  are free, and the caller retries this I/O after one of its in-flight I/Os completed
         *  and released its guard, so progress is always guaranteed without any lock ordering.
         */
        Action begin(Journal* journal, uint64_t absoluteOffset, size_t ioLen, Intent intent,
            bool mayBlock);

        /**
         * Finish a successful I/O: for a write, commit the new generations (and msync if
         * "--journalsync"); then release the guard. For a read this only releases the guard.
         */
        void commit();

        /**
         * Give up without committing, e.g. after a failed I/O. Journal cells of a failed write
         * are deliberately left at 0 ("uninitialized"), because a failed or partial write cannot
         * be assumed to have left the previous content intact.
         */
        void release() { guard.release(); }

        bool isWrite() const { return (action == ACTION_WRITE); }
        Journal* getJournal() const { return journal; }
        const std::vector<uint8_t>& getGens() const { return gens; }

    private:
        Journal* journal{nullptr};
        uint64_t baseBlockIdx{0};
        size_t numBlocks{0};
        Action action{ACTION_SKIP};
        std::vector<uint8_t> gens; // per journal block of this I/O; 0 means "never written"
        JournalRangeGuard guard;

        bool claimForWrite(bool mayBlock);
};

#endif /* TOOLKITS_JOURNAL_H_ */
