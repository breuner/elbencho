// SPDX-FileCopyrightText: 2026 Sven Breuner and elbencho contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <iomanip>
#include <random>
#include <sstream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include "Common.h"
#include "ProgException.h"
#include "toolkits/Journal.h"

namespace bpt = boost::property_tree;

/**
 * The stripe locks, shared by all journals of this process. Allocated on first use, so builds/runs
 * without journaling don't pay for them.
 */
std::shared_mutex* Journal::getStripeLocks()
{
    // (function-local static init is thread-safe; in practice this runs single-threaded during
    // JournalStore::init(), before any worker thread exists.)
    static std::vector<std::shared_mutex> stripeLocks(NUM_STRIPES);

    return stripeLocks.data();
}

/**
 * Turn a target path/identifier into a filesystem-safe base name, with a hash suffix to guarantee
 * uniqueness even when two different identifiers sanitize similarly (e.g. "/dev/sdb" vs.
 * "/mnt/dev_sdb").
 */
std::string Journal::sanitizeTargetToFileNameBase(const std::string& targetIdentifier)
{
    std::string result;
    result.reserve(targetIdentifier.size() );

    for(char c : targetIdentifier)
        result += (std::isalnum( (unsigned char)c) || (c == '-') || (c == '.') ) ? c : '_';

    // keep filenames reasonably short; uniqueness is still guaranteed by the hash suffix below
    if(result.size() > 150)
        result = result.substr(result.size() - 150);

    uint32_t hashVal = (uint32_t)std::hash<std::string>{}(targetIdentifier);

    std::ostringstream hexStream;
    hexStream << std::hex << std::setw(8) << std::setfill('0') << hashVal;

    return result + "_" + hexStream.str();
}

std::unique_ptr<Journal> Journal::createNew(const std::string& journalDirPath,
    const std::string& targetIdentifier, uint64_t journalBlockSize, uint64_t targetOffset,
    uint64_t targetSize)
{
    std::unique_ptr<Journal> journal(new Journal() );

    const std::string baseName = sanitizeTargetToFileNameBase(targetIdentifier);
    const std::string binaryFileName = baseName + ".journal";

    journal->binaryPath = journalDirPath + "/" + binaryFileName;
    journal->sidecarPath = journalDirPath + "/" + baseName + ".journal.json";
    journal->journalBlockSize = journalBlockSize;
    journal->targetOffset = targetOffset;
    journal->targetSize = targetSize;
    journal->numJournalBlocks = (targetSize + journalBlockSize - 1) / journalBlockSize;
    journal->mapLength = (journal->numJournalBlocks * 2 + 7) / 8;

    std::random_device randDev;
    journal->journalSeed = ( (uint64_t)randDev() << 32) | (uint64_t)randDev();

    // write the sidecar first, durably, before touching the binary journal at all
    bpt::ptree tree;
    tree.put("target", targetIdentifier);
    tree.put("journalBlockSize", journal->journalBlockSize);
    tree.put("targetOffset", journal->targetOffset);
    tree.put("targetSize", journal->targetSize);
    tree.put("journalSeed", journal->journalSeed);
    tree.put("binaryFileName", binaryFileName);

    std::ostringstream jsonStream;
    bpt::write_json(jsonStream, tree);
    const std::string jsonStr = jsonStream.str();

    int sidecarFD = open(journal->sidecarPath.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if(sidecarFD == -1)
        throw ProgException("Unable to create journal sidecar file. "
            "Path: " + journal->sidecarPath + "; "
            "SysErr: " + strerror(errno) );

    ssize_t writeRes = write(sidecarFD, jsonStr.c_str(), jsonStr.size() );
    if(writeRes != (ssize_t)jsonStr.size() )
    {
        close(sidecarFD);
        throw ProgException("Unable to write journal sidecar file. "
            "Path: " + journal->sidecarPath );
    }

    if(fsync(sidecarFD) == -1)
    {
        close(sidecarFD);
        throw ProgException("Unable to fsync journal sidecar file. "
            "Path: " + journal->sidecarPath + "; "
            "SysErr: " + strerror(errno) );
    }

    close(sidecarFD);

    // create, preallocate and mmap the binary journal
    journal->fd = open(journal->binaryPath.c_str(), O_RDWR | O_CREAT | O_EXCL, 0644);
    if(journal->fd == -1)
        throw ProgException("Unable to create journal binary file. "
            "Path: " + journal->binaryPath + "; "
            "SysErr: " + strerror(errno) );

    if(ftruncate(journal->fd, journal->mapLength) == -1)
        throw ProgException("Unable to set size of journal binary file. "
            "Path: " + journal->binaryPath + "; "
            "Size: " + std::to_string(journal->mapLength) + "; "
            "SysErr: " + strerror(errno) );

#if !defined(__APPLE__)
    // (no posix_fallocate on macOS, so we skip preallocation there.)

    /* preallocate real disk blocks now, so later mmap'ed writes can never SIGBUS due to the
        journal directory's file system running out of space in the middle of a run */
    int preallocRes = posix_fallocate(journal->fd, 0, journal->mapLength);
    if(preallocRes != 0)
        throw ProgException("Unable to preallocate journal binary file disk space. "
            "Path: " + journal->binaryPath + "; "
            "Size: " + std::to_string(journal->mapLength) + "; "
            "SysErr: " + strerror(preallocRes) ); // (posix_fallocate does not set errno)
#endif // !apple

    journal->mmapBinaryFile();

    return journal;
}

std::unique_ptr<Journal> Journal::openExisting(const std::string& binaryPath,
    const std::string& sidecarPath, uint64_t journalBlockSize, uint64_t targetOffset,
    uint64_t targetSize, uint64_t journalSeed)
{
    std::unique_ptr<Journal> journal(new Journal() );

    journal->binaryPath = binaryPath;
    journal->sidecarPath = sidecarPath;
    journal->journalBlockSize = journalBlockSize;
    journal->targetOffset = targetOffset;
    journal->targetSize = targetSize;
    journal->journalSeed = journalSeed;
    journal->numJournalBlocks = (targetSize + journalBlockSize - 1) / journalBlockSize;
    journal->mapLength = (journal->numJournalBlocks * 2 + 7) / 8;

    journal->fd = open(binaryPath.c_str(), O_RDWR);
    if(journal->fd == -1)
        throw ProgException("Unable to open existing journal binary file. "
            "Path: " + binaryPath + "; "
            "SysErr: " + strerror(errno) );

    struct stat statBuf;
    if(fstat(journal->fd, &statBuf) == -1)
        throw ProgException("Unable to stat existing journal binary file. "
            "Path: " + binaryPath + "; "
            "SysErr: " + strerror(errno) );

    if( (uint64_t)statBuf.st_size != journal->mapLength)
        throw ProgException("Existing journal binary file size does not match the size derived "
            "from its sidecar. "
            "Path: " + binaryPath + "; "
            "Expected size: " + std::to_string(journal->mapLength) + "; "
            "Actual size: " + std::to_string(statBuf.st_size) );

    journal->mmapBinaryFile();

    return journal;
}

Journal::~Journal()
{
    if(mapBase)
    {
        if(journalSyncEnabled)
            msync(mapBase, mapLength, MS_SYNC);

        munmap(mapBase, mapLength);
    }

    if(fd != -1)
        close(fd);
}

void Journal::mmapBinaryFile()
{
    void* mapRes = mmap(NULL, mapLength, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    if(mapRes == MAP_FAILED)
        throw ProgException("Unable to mmap journal binary file. "
            "Path: " + binaryPath + "; "
            "Size: " + std::to_string(mapLength) + "; "
            "SysErr: " + strerror(errno) );

    mapBase = (uint8_t*)mapRes;
}

/**
 * Apply the options that depend on the current run (rather than on the journal's stored metadata)
 * and size the lock regions accordingly.
 *
 * Lock regions are sized to hold at least one max-size I/O, which is what guarantees that a
 * single I/O never covers more than two regions and thus never needs more than two locks. (This
 * is also why the region size is a per-run value: a later run may resume the same journal with a
 * different "--block" size mix.)
 *
 * @maxIOSize largest possible single I/O in this run, i.e. progArgs::blockSizeMix::getMaxSize().
 */
void Journal::initRunOptions(uint64_t maxIOSize, bool journalSyncEnabled)
{
    this->journalSyncEnabled = journalSyncEnabled;

    const uint64_t maxBlocksPerIO = (maxIOSize + journalBlockSize - 1) / journalBlockSize;

    // round up to a power of two, so the region index is a cheap shift in the hot path
    lockRegionShift = 0;
    while( ( (uint64_t)1 << lockRegionShift) < maxBlocksPerIO)
        lockRegionShift++;

    // mix the journal's own seed in, so two journals don't collide in exactly the same pattern
    stripeSalt = journalSeed;

    getStripeLocks(); // force init while we are still single-threaded
}

/**
 * Acquire the journal locks covering [firstBlockIdx, firstBlockIdx+numBlocks).
 *
 * At most two distinct stripes are involved (see initRunOptions() ), and they are always locked in
 * ascending stripe order. Together with the rule that a thread only ever waits for locks while
 * holding none of its own (see JournalBlockIO::begin() ), that makes circular wait - the one
 * precondition for deadlock - structurally impossible.
 *
 * @return false only for mayBlock==false when the locks were not immediately available; nothing
 *  stays locked in that case.
 */
bool Journal::acquireRange(uint64_t firstBlockIdx, size_t numBlocks, bool isExclusive,
    bool mayBlock, JournalRangeGuard& outGuard)
{
    std::shared_mutex* stripeLocks = getStripeLocks();

    const size_t firstStripe = blockIndexToStripe(firstBlockIdx);
    const size_t lastStripe = blockIndexToStripe(firstBlockIdx + numBlocks - 1);

    outGuard.isExclusive = isExclusive;

    // lock ascending; dedup, because locking the same mutex twice would deadlock on itself
    outGuard.locks[0] = &stripeLocks[std::min(firstStripe, lastStripe)];
    outGuard.locks[1] = (firstStripe != lastStripe) ?
        &stripeLocks[std::max(firstStripe, lastStripe)] : nullptr;

    const unsigned numLocksWanted = outGuard.locks[1] ? 2 : 1;

    outGuard.numLocks = 0; // counts what we actually hold, so release() stays correct on failure

    for(unsigned i = 0; i < numLocksWanted; i++)
    {
        if(mayBlock)
        {
            if(isExclusive)
                outGuard.locks[i]->lock();
            else
                outGuard.locks[i]->lock_shared();
        }
        else
        {
            const bool gotLock = isExclusive ?
                outGuard.locks[i]->try_lock() : outGuard.locks[i]->try_lock_shared();

            IF_UNLIKELY(!gotLock)
            {
                outGuard.release(); // give up whatever we got so far, so we never hold-and-wait

                return false;
            }
        }

        outGuard.numLocks = i + 1;
    }

    return true;
}

bool Journal::acquireRangeExclusive(uint64_t firstBlockIdx, size_t numBlocks, bool mayBlock,
    JournalRangeGuard& outGuard)
{
    return acquireRange(firstBlockIdx, numBlocks, true, mayBlock, outGuard);
}

bool Journal::acquireRangeShared(uint64_t firstBlockIdx, size_t numBlocks, bool mayBlock,
    JournalRangeGuard& outGuard)
{
    return acquireRange(firstBlockIdx, numBlocks, false, mayBlock, outGuard);
}

uint8_t Journal::loadBlockGen(uint64_t journalBlockIndex) const
{
    size_t byteIdx; unsigned shift; uint8_t mask;
    cellLocation(journalBlockIndex, byteIdx, shift, mask);

    /* (atomics directly on the mmap'ed bytes: four blocks share an on-disk byte, and with a lock
        region of fewer than four blocks - i.e. maxIOSize <= 2*journalBlockSize - they can belong
        to different regions and thus be updated concurrently.) */
    auto* atomicBytePtr = reinterpret_cast<const std::atomic<uint8_t>*>(mapBase + byteIdx);
    uint8_t byteVal = atomicBytePtr->load(std::memory_order_acquire);

    return (byteVal & mask) >> shift;
}

void Journal::markBlockBusy(uint64_t journalBlockIndex)
{
    size_t byteIdx; unsigned shift; uint8_t mask;
    cellLocation(journalBlockIndex, byteIdx, shift, mask);

    auto* atomicBytePtr = reinterpret_cast<std::atomic<uint8_t>*>(mapBase + byteIdx);

    // masked, so a concurrent update of one of the 3 blocks sharing this byte can't be lost
    atomicBytePtr->fetch_and( (uint8_t)~mask, std::memory_order_acq_rel);
}

void Journal::commitBlockGen(uint64_t journalBlockIndex, uint8_t newGen)
{
    size_t byteIdx; unsigned shift; uint8_t mask;
    cellLocation(journalBlockIndex, byteIdx, shift, mask);

    auto* atomicBytePtr = reinterpret_cast<std::atomic<uint8_t>*>(mapBase + byteIdx);

    // cell is already 0 from markBlockBusy(), so a plain OR of the new generation is enough
    atomicBytePtr->fetch_or( (uint8_t)( (newGen << shift) & mask), std::memory_order_acq_rel);
}

/**
 * msync only the pages actually holding this I/O's journal cells - never the whole journal file,
 * which would defeat the point of the option: it is targetSize/(4*journalBlockSize) bytes, so
 * still tens of MiB for a multi-TiB target at the default journal block size.
 */
void Journal::syncRange(uint64_t firstBlockIdx, size_t numBlocks) const
{
    if(!journalSyncEnabled)
        return;

    /* (explicit uint64_t for the min, because size_t and uint64_t are not the same type on all
        platforms - e.g. on macOS - so deduction from a mixed pair would fail. Narrowing to size_t
        afterwards is safe, because the value is clamped to mapLength, which is a size_t.) */
    const size_t startByteIdx = firstBlockIdx / 4;
    const size_t endByteIdx = (size_t)std::min<uint64_t>(
        (firstBlockIdx + numBlocks + 3) / 4, mapLength);

    const size_t pageSize = sysconf(_SC_PAGESIZE);
    const size_t pageAlignedStart = (startByteIdx / pageSize) * pageSize; // msync needs this

    int syncRes = msync(mapBase + pageAlignedStart, endByteIdx - pageAlignedStart, MS_SYNC);

    if(syncRes == -1)
        throw ProgException("Unable to msync journal. "
            "Path: " + binaryPath + "; "
            "SysErr: " + strerror(errno) );
}

/**
 * Percentage of this journal's blocks that currently hold a verifiable content generation, i.e.
 * whose cell is not 0. Meant for a one-time report when a journal is opened, not for the I/O
 * path: this scans the whole mapping, which is targetSize/(4*journalBlockSize) bytes.
 *
 * Takes no locks, because this only runs single-threaded from JournalStore::init(), before any
 * worker thread exists.
 */
double Journal::getVerifiablePercent() const
{
    if(!numJournalBlocks)
        return 0; // can't happen (a zero size is rejected), but don't divide by it either

    uint64_t numVerifiable = 0;
    size_t byteIdx = 0;

    /* Each byte holds four 2bit cells, so "(w | (w >> 1)) & 0x55.." leaves the low bit of every
        non-zero cell set and popcount then counts the cells. Cells never straddle a byte, so this
        works the same on a whole word as on a single byte, regardless of byte order.
        (The up-to-three spare cells in the last byte need no masking: nothing ever addresses them
        and the binary journal is created zero-filled, so they are always 0 and count as nothing.)
    */
    for( /* byteIdx */ ; (byteIdx + sizeof(uint64_t) ) <= mapLength; byteIdx += sizeof(uint64_t) )
    {
        uint64_t wordVal;
        memcpy(&wordVal, mapBase + byteIdx, sizeof(wordVal) ); // (memcpy for strict aliasing)

        numVerifiable += (uint64_t)__builtin_popcountll(
            (wordVal | (wordVal >> 1) ) & 0x5555555555555555ULL);
    }

    for( /* byteIdx */ ; byteIdx < mapLength; byteIdx++)
    {
        const uint8_t byteVal = mapBase[byteIdx];

        numVerifiable += (uint64_t)__builtin_popcount(
            (unsigned)( (byteVal | (byteVal >> 1) ) & 0x55) );
    }

    return 100.0 * (double)numVerifiable / (double)numJournalBlocks;
}

uint64_t Journal::computeExpectedTileValue(uint64_t blockAlignedOffset, uint64_t journalSeed,
    uint8_t generation)
{
    uint64_t x = blockAlignedOffset ^ (journalSeed * 0x9E3779B97F4A7C15ULL) ^
        ( (uint64_t)generation * 0xBF58476D1CE4E5B9ULL);

    x ^= x >> 33; x *= 0xFF51AFD7ED558CCDULL;
    x ^= x >> 33; x *= 0xC4CEB9FE1A85EC53ULL;
    x ^= x >> 33;

    return x;
}


JournalBlockIO::Action JournalBlockIO::begin(Journal* journal, uint64_t absoluteOffset,
    size_t ioLen, Intent intent, bool mayBlock)
{
    const uint64_t journalBlockSize = journal->getJournalBlockSize();

    this->journal = journal;
    this->baseBlockIdx = journal->offsetToBlockIndex(absoluteOffset);
    this->numBlocks = (ioLen + journalBlockSize - 1) / journalBlockSize;

    gens.resize(numBlocks); // (no-op/no realloc once the vector has grown to its max size)

    if(intent == INTENT_WRITE)
        return claimForWrite(mayBlock) ? action : ACTION_DEFER;

    // a read: take the shared guard first, so the generations we snapshot cannot change under us
    if(!journal->acquireRangeShared(baseBlockIdx, numBlocks, mayBlock, guard) )
        return ACTION_DEFER;

    bool anyUninit = false;
    bool allUninit = true;

    for(size_t i = 0; i < numBlocks; i++)
    {
        gens[i] = journal->loadBlockGen(baseBlockIdx + i);

        if(gens[i] == 0)
            anyUninit = true;
        else
            allUninit = false;
    }

    if(!anyUninit)
    { // fully initialized, so this stays a plain read
        action = ACTION_READ;
        return action;
    }

    if(intent == INTENT_READ_SKIP_UNINIT)
    {
        if(allUninit)
        { // nothing was ever written here, so skip the read entirely and accept it as verified-ok
            guard.release();
            action = ACTION_SKIP;
            return action;
        }

        /* partially initialized: still read it, and verify only the initialized blocks (see
            LocalWorker::postReadJournalVerifyBuf() ) */
        action = ACTION_READ;
        return action;
    }

    // INTENT_READ_OR_INIT with uninitialized block(s): turn this read into an initializing write
    guard.release(); // upgrade shared -> exclusive

    return claimForWrite(mayBlock) ? action : ACTION_DEFER;
}

/**
 * Take the exclusive guard and stamp the covered blocks as "being written": read each block's
 * current generation, rotate it to get the generation to write, and only then clear the cells to
 * 0. A crash between here and commit() therefore leaves them at 0, i.e. correctly resuming as
 * "uninitialized" rather than claiming content that was never fully written.
 *
 * Without "--journalsync" that holds as long as the page cache survives, i.e. for a process crash
 * or a target that went away and came back. With "--journalsync" the cleared cells are made
 * durable here, before the caller issues the data write, so it also holds across a power failure.
 *
 * @return false if the guard was not available and mayBlock was false.
 */
bool JournalBlockIO::claimForWrite(bool mayBlock)
{
    if(!journal->acquireRangeExclusive(baseBlockIdx, numBlocks, mayBlock, guard) )
        return false;

    bool anyWasVerifiable = false; // whether any covered cell held a generation before this I/O

    for(size_t i = 0; i < numBlocks; i++)
    {
        // note: load the old generation *before* clearing the cell, so rotation really advances
        const uint8_t oldGen = journal->loadBlockGen(baseBlockIdx + i);

        gens[i] = Journal::nextGen(oldGen);

        anyWasVerifiable |= (oldGen != 0);

        journal->markBlockBusy(baseBlockIdx + i);
    }

    /* Get "being written" to disk before the caller issues the data write, so that a power
        failure can't leave the journal claiming this block's previous generation while the target
        block already holds half of the new content. No-op without "--journalsync".
        Skipped when no covered cell held a generation anyway: the on-disk cells are then already
        0, so there is nothing that a power failure could wrongly claim - which also means that a
        first pass over a fresh journal pays nothing for this at all. */
    if(anyWasVerifiable)
        journal->syncRange(baseBlockIdx, numBlocks);

    action = ACTION_WRITE;

    return true;
}

void JournalBlockIO::commit()
{
    if(action == ACTION_WRITE)
    {
        for(size_t i = 0; i < numBlocks; i++)
            journal->commitBlockGen(baseBlockIdx + i, gens[i] );

        journal->syncRange(baseBlockIdx, numBlocks);
    }

    guard.release();
}
