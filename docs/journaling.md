# **Journaled Data Verification**

Journaled data verification lets elbencho check that a target still contains what elbencho wrote into it, even when the run that wrote it is long over — after a crash, after a power failure, or continuously while a read/write mix is in progress.

It is enabled by pointing `--journaldir` at a directory:

```bash
elbencho -w -t 8 -b 1M --iodepth 8 --journaldir /mnt/journal /dev/nvme0n1
```

elbencho then keeps a small **write journal** next to the benchmark: one journal per target, recording which blocks of that target have been written and what they are supposed to contain. Any later run that is given the same `--journaldir` picks the journal up again and verifies every block the journal knows about.

## **Quick Start**

Write a target with journaling enabled, then read it back and verify it:

```bash
# write, creating the journal
elbencho -w -t 8 -b 1M --journaldir /mnt/journal /dev/nvme0n1

# read back and verify against the journal
elbencho -r -t 8 -b 1M --journaldir /mnt/journal /dev/nvme0n1
```

The read phase reports what it found:

```
NOTE: Resumed existing journal for target. Target: /dev/nvme0n1; Verifiable: 100.0%
```

A verification failure names the exact offset and the two values that disagree, and the run exits non-zero:

```
ERROR: Journal data verification failed. Offset: 1048576; Expected value: 146; Actual value: 0
```

Reading with a different block size than the write used is fine, because the expected content is anchored to the journal's own block size and not to whatever block size happened to write it:

```bash
elbencho -r -t 8 -b 64K --journaldir /mnt/journal /dev/nvme0n1
```

The journal directory has to exist; elbencho does not create it. It should live on a **different** file system than the target, so that the journal does not compete with the benchmark for the very device under test.

## **How This Differs From `--verify`**

`--verify <salt>` writes a pattern that is a pure function of the salt and the block's offset. That makes it self-describing — any reader with the same salt knows what every offset should contain — but it also means it has no idea *whether* a block was ever written, or *when*. Consequences:

* A block that was never written fails verification, so `--verify` can only check a target that was written completely, from start to finish, in a phase that ran to the end.
* Rewriting a block with the same salt produces the same bytes, so content left over from an earlier run is indistinguishable from content written by this one.
* `--verify` cannot be combined with `--rwmixpct`, because a mixed phase reads blocks whose write may not have happened yet.

A journal fixes all three, because the per-block state is recorded rather than assumed:

| | `--verify` | `--journaldir` |
| :--- | :--- | :--- |
| Partially written target | fails | uninitialized blocks are skipped and accepted |
| Survives a crash / power loss | no notion of it | that is the point of it |
| Detects stale content from an earlier run | no | yes, via content generations |
| Read/write mix phases (`--rwmixpct`, `--rwmixthr`) | rejected | supported, reads are verified inline |
| Extra state to keep | none | one journal per target |
| Salt to remember between runs | yes | no |

The two are mutually exclusive — pick one. Note also that journaling implicitly disables block variance, exactly as `--verify` does, so `--blockvarpct 0` does not have to be given.

In a read/write mix, a read that lands on a block the journal has never seen written is turned into a write that initializes it, so the mix can start on an untouched target and still verify everything it reads:

```bash
elbencho -w -t 8 -b 1M --iodepth 8 --rwmixpct 30 --journaldir /mnt/journal /dev/nvme0n1
```

The first such run therefore mostly writes; subsequent runs on the same journal really do read and verify.

## **What Is Stored In The Journal Directory**

Two files per target:

```
<sanitized target>_<hash>.journal        # the binary journal, 2 bits per block
<sanitized target>_<hash>.journal.json   # the sidecar describing it
```

The name is derived from the target path with everything that is not a letter, digit, `-` or `.` replaced by `_`, plus a short hash of the full target string so that two different targets can never collide. The names are for humans only — elbencho finds a journal by reading the sidecars, never by guessing file names.

The sidecar is small and is written once, at creation:

```json
{
    "target": "\/dev\/nvme0n1",
    "journalBlockSize": "4096",
    "targetOffset": "0",
    "targetSize": "3200631791616",
    "journalSeed": "270628837756989013",
    "binaryFileName": "_dev_nvme0n1_<hash>.journal"
}
```

`journalSeed` is a random 64-bit value drawn when the journal is created; it is what makes the expected content differ between journals. `targetOffset` and `targetSize` record the range the journal was created for. The sidecar never needs updating afterwards.

## **How Large The Journal Gets**

The journal tracks whole blocks of `--journalblock` bytes (default 4096) and spends **2 bits per block**, i.e. 4 blocks per byte. So:

```
journal size = target size / (4 * journalblock)
```

| `--journalblock` | journal per target byte | 1 TiB target | 100 TiB target | 1 PiB target |
| :--- | :--- | :--- | :--- | :--- |
| 4K (default) | 1 : 16384 | 64 MiB | 6.25 GiB | 64 GiB |
| 64K | 1 : 262144 | 4 MiB | 400 MiB | 4 GiB |
| 1M | 1 : 4194304 | 256 KiB | 25 MiB | 256 MiB |

The default keeps several hundreds of TiB of targets within a couple dozen GiB of journal, which is what makes it practical to put `--journaldir` on a `tmpfs` for maximum journal speed. A larger `--journalblock` shrinks the journal proportionally at the cost of coarser tracking: every I/O block size and `--offset` must be an exact multiple of it, and `--size` gets rounded down to a multiple of it.

The binary journal is preallocated with `posix_fallocate()` when it is created, so it can never run out of space halfway through a run.

## **Resuming A Journal**

Any run given a `--journaldir` scans it, matches the sidecars against the targets of this run, resumes what it finds and creates what it does not. Resuming is the normal case and needs no extra option:

```bash
elbencho -w -t 8 -b 1M --journaldir /mnt/journal /dev/nvme0n1   # creates
elbencho -r -t 8 -b 1M --journaldir /mnt/journal /dev/nvme0n1   # resumes
```

A resumed journal may be used with a **different block size** or even a different block size mix than the run that wrote it:

```bash
elbencho -w -t 8 -b 4K:1,1M:1 --journaldir /mnt/journal /dev/nvme0n1
elbencho -r -t 8 -b 256K --journaldir /mnt/journal /dev/nvme0n1
```

It may also be used with **more or fewer targets** than before. Targets that are not part of this run are simply left alone, and targets that have no journal yet get one:

```bash
# two targets, one journal each
elbencho -w -t 8 -b 1M --journaldir /mnt/journal /dev/nvme0n1 /dev/nvme1n1

# only one of them, the other journal stays untouched
elbencho -r -t 8 -b 1M --journaldir /mnt/journal /dev/nvme0n1
```

Two things have to stay the same: `--journalblock`, and the `--offset`/`--size` range must lie **within** the range the journal was originally created for. A narrower range is fine, a wider one is refused.

### **Resuming With A Different Target Path**

A journal is matched to its target by the exact path string, so a device that comes back under a different name — or a file that was moved — needs its sidecar's `target` field updated. That is what the sidecar is for, and it is the only field that has to change:

```bash
# /dev/nvme0n1 came back as /dev/nvme2n1 after a reboot
cd /mnt/journal
SIDECAR=$(grep -l '"target": "\\/dev\\/nvme0n1"' *.journal.json)

jq '.target = "/dev/nvme2n1"' "$SIDECAR" > "$SIDECAR.new" && mv "$SIDECAR.new" "$SIDECAR"

elbencho -r -t 8 -b 1M --journaldir /mnt/journal /dev/nvme2n1
```

Without `jq`, a plain `sed` on that one line does the same job:

```bash
sed -i 's|"target": ".*"|"target": "\\/dev\\/nvme2n1"|' "$SIDECAR"
```

The binary journal file does not have to be renamed — its name comes from the sidecar's `binaryFileName` field, not from the target. Note that the same target under a different *spelling* of the same path (say `./nvme0n1` versus `/dev/nvme0n1`, or a symlink) counts as a different target and gets a second, empty journal, which is not what you want: keep the path identical across runs that are meant to resume.

A resumed journal reports how much of it is usable, which after an interrupted run is the interesting part:

```
NOTE: Resumed existing journal for target. Target: /dev/nvme0n1; Verifiable: 61.7%
```

That is the share of journal blocks holding verifiable content. The remaining blocks were never written, or their write did not complete, and a read phase skips them instead of reading them.

## **When `--journalsync` Is Relevant**

By default, journal updates go into the page cache and are written back by the kernel whenever it sees fit. That is enough for the journal to stay correct across:

* a process crash (elbencho segfaulting, being killed, or hitting an I/O error),
* an interrupted run (ctrl+c, `--timelimit`),
* a target that disappears and comes back, e.g. a device or path that was temporarily removed.

In all of those, the kernel and its page cache survive, so the journal does too.

It is **not** enough for a host power failure or a kernel crash, where the page cache is lost along with everything in it. `--journalsync` is for exactly that case:

```bash
elbencho -w -t 8 -b 1M --iodepth 8 --journalsync --journaldir /mnt/journal /dev/nvme0n1
```

With it, each block's "being written" state is made durable *before* its data write is issued, and its new content state durably after the write completed. A block whose write was in flight when the power went therefore resumes as "never written" and is skipped, rather than being reported as corruption.

Two things to keep in mind:

* `--journalsync` implicitly enables `--direct`, because a recorded content state only means something if the target write it describes is durable once it completes. A buffered write still sitting in the page cache when the power goes would otherwise look exactly like corruption on the next run. This also means `--journalsync` cannot be used on a file system that rejects `O_DIRECT`.
* The journal itself has to survive the power failure. A `tmpfs` journal directory is excellent for speed and for the read/write-mix use case, but it is gone after a power cut — so for power-failure verification put `--journaldir` on a persistent file system (still a different one than the target).

`--journalsync` costs up to two `msync()` calls per write, so expect a significant throughput impact. The first of the two is skipped on a journal's first pass over a range, because a block that had no content state yet has nothing that a power failure could misreport — so an initial write pass costs one sync per write and only later rewrites cost two.

## **Requirements And Restrictions**

* Targets must be **files or block devices**, including SPDK NVMe-oF namespaces. Directory mode (`-d`) is not supported, because there is no stable per-target journal in that case.
* Local runs only: `--journaldir` cannot be combined with `--hosts` or `--service`.
* Mutually exclusive with `--verify`.
* Every block size of `-b`, plus `--offset`, must be an exact multiple of `--journalblock`. A `--size` that is not a multiple of it is **rounded down** to the next one, and the run says so:
  ```
  NOTE: File size has to be a multiple of the write journal's block size. Reducing file size. Old: 10485760; New: 10485618; Journal block size: 1111
  ```
  A `--size` smaller than one journal block is refused, since there would be nothing left to track.
* Random I/O must be block-aligned to the journal's block size, so `--norandalign` cannot be used with it.
* All journals of one run share one `--journalblock`.

---

## **Technical Details**

This section describes implementation details of elbencho's journaling feature.

### **What Gets Written To The Target**

For each journal block, elbencho computes one 64-bit value from three inputs — the block's absolute offset in the target, the journal's random seed, and the block's current content generation — mixed through a murmur3-style avalanche function. That value is then repeated ("tiled") across the whole journal block.

Anchoring on the absolute offset makes a misdirected write detectable: content that is valid but landed at the wrong offset does not match what the journal expects there. Including the seed keeps two journals for the same target from agreeing by accident. Including the generation is what makes stale content detectable, as below.

Verification recomputes the value per journal block and compares it, which is why a read phase may use any block size that is a multiple of `--journalblock`: it simply spans several journal blocks per I/O, each with its own expected value.

### **Journal Block States And Their Rotation**

Each journal block gets 2 bits, so four states, packed 4 blocks per byte:

* **0** — uninitialized: never written, or a write to it did not complete. Nothing is known about this block's content, so a read phase skips it without reading it and accepts it as OK, while a read/write mix converts the read into a write that initializes it.
* **1, 2, 3** — content generations.

A write to a block reads its current generation and rotates it: `0 → 1`, `1 → 2`, `2 → 3`, `3 → 1`. Note that 0 is never re-entered by rotation, so a committed generation is always distinguishable from "uninitialized".

Because the generation feeds into the expected content, rewriting a block changes the bytes that belong in it. That is what catches **stale content**: a block still holding the perfectly well-formed content of an earlier generation — a write that the storage stack silently lost, or an old copy restored underneath elbencho — does not match the generation the journal recorded, and is reported as a verification failure. A salt-based `--verify` cannot see this, since the same salt and offset always yield the same bytes.

The sequence for one write is therefore:

1. Take the journal lock for the block range.
2. Read the old generations, compute the new ones, and set the cells to 0 ("being written").
3. With `--journalsync`: `msync()` those cells, so the 0 is durable before step 4.
4. Fill the I/O buffer with the new generations' content and issue the write.
5. On successful completion, write the new generations into the cells, and with `--journalsync` `msync()` them.
6. Release the lock.

A crash anywhere between steps 2 and 5 leaves the cells at 0, which resumes as "never written" — conservative, and never a false alarm. If the write **fails**, the cells are deliberately left at 0 as well, because a failed or partial write cannot be assumed to have left the previous content intact.

### **Power-Loss Correctness, With And Without `--journalsync`**

The ordering in steps 3 and 5 is the whole difference. Without `--journalsync`, steps 2 and 5 only touch the mapped pages, and the kernel writes them back at its own discretion. After a power cut the on-disk journal may therefore still show the block's *previous* generation while the target block already holds part of the new content — which reads as corruption on the next run, even though nothing was actually lost except an in-flight write.

With `--journalsync`, the 0 reaches stable storage before the data write is even issued, so that window does not exist: whatever the power cut interrupts, the journal on disk says "uninitialized" for it. The commit in step 5 is likewise durable before the run considers the block written.

The other half of the ordering belongs to the target rather than to the journal, which is why `--journalsync` turns on `--direct`: if a committed generation reached the disk while the data it describes was still in the page cache, the next run would again see a mismatch. Direct I/O removes that cache from the picture, leaving durability at the device, where power-loss protection is the storage's own business.

Also note the page granularity of the per-I/O syncs: only the pages actually holding the affected cells are synced, not the whole journal — with the default `--journalblock` a 4 KiB page covers 16384 journal blocks, i.e. 64 MiB of target range. The whole mapping is synced once more when the journal is closed at the end of the run, so a run that finished normally leaves nothing unwritten behind.

### **In-Memory Stripe Locks**

Random I/O means two worker threads can target the same block at the same time, so access to a journal block has to be serialized: two concurrent writers would corrupt each other's generations, and a reader running alongside a writer would verify against a state that is mid-change. Multiple concurrent *readers* of the same block are fine.

That is provided by a fixed array of 65536 `std::shared_mutex` instances — about 3.5 MiB, allocated on first use and shared by every journal of the process. The cost is therefore constant no matter how large the targets are or how many of them there are, which a lock-per-block or lock-per-target scheme could not offer.

Blocks are first grouped into **lock regions**: fixed runs of `2^n` blocks, aligned to their own size, so block *i* always belongs to region *i >> n*. Locking is done per region rather than per block, so one lock covers a whole run of neighbouring blocks and an I/O spanning many of them still only takes a handful of locks.

A region is sized to cover at least one maximum-size I/O of the run, i.e. derived from the largest block size of `-b`. The region size is recomputed per run, since a later run may resume the same journal with a different `-b`.

That sizing is what bounds the number of locks per I/O — but the bound is **two**, not one, because regions are aligned while I/Os are not. An I/O covers a contiguous run of journal blocks that is never *longer* than a region, yet it may start anywhere, so it either falls entirely inside one region or overlaps the end of one and the start of the next. It can never reach a third, precisely because it is not longer than a region.

With `--journalblock 4K` and `-b 64K`, a region is 16 journal blocks:

```
regions (16 blocks each, always aligned to 16):
   |........... region 1 ...........|........... region 2 ...........|
   16 17 18 19 ..................31 | 32 33 34 35 ..................47 |

a 64 KiB I/O at a 64 KiB-aligned offset covers blocks 16..31:
   [===============================]                                     -> 1 region,  1 lock

the same 64 KiB I/O one journal block later covers blocks 17..32:
      [===============================]                                  -> 2 regions, 2 locks
```

The diagram shows the tightest case, where the I/O is exactly as long as its region: then any misalignment at all makes it straddle. An I/O *shorter* than its region has slack and only needs the second lock when it happens to span a boundary.

Whether that happens is decided by the I/O's offset, and two common setups make it frequent:

* A **block size mix** sizes the region for the largest size of the mix, while the I/Os using it start at multiples of whichever size was drawn — so a large I/O straddles unless it happens to land on a region boundary, while the small ones almost always fit inside one.
* A block size that is **not a power of two** — say `-b 96K` on a 4 KiB journal block — makes the region round up to 128 KiB while the I/Os advance in steps of 96 KiB. The offsets then cycle through the region at four different alignments, of which two straddle a boundary and two do not.

A uniform, power-of-two block size on the other hand matches its region exactly, and sequential I/O from a region-aligned offset then needs just one lock per I/O.

So the lock guard for one I/O is a fixed two-slot array and never needs a heap allocation in the I/O path.

Region indices are then hashed onto the 65536 stripes, mixing in a per-journal salt. Two effects worth knowing:

* The same offset in two different targets maps to different stripes, so targets never serialize against each other at equal offsets.
* No target is confined to a corner of the array — each journal's regions are scattered across all of it.

Unrelated regions can of course collide on the same stripe. That costs occasional false contention and nothing else. One collision is even convenient: if the two regions of a single I/O happen to land on the same stripe — as do both ends of an I/O that stayed inside one region — then only that one lock is taken, because locking the same mutex twice would deadlock a thread against itself.

There is no spinning and no backoff anywhere. A thread that holds no lock of its own simply blocks. The one exception is a thread that already holds a lock for an I/O of its own still in flight — with `--iodepth` above 1 that is the normal situation — which must not block for a second one, or two threads could wait on each other's locks forever. Such a thread takes the locks only if they are free, and otherwise defers the I/O and retries it after one of its own in-flight I/Os completes and releases its lock. Since locks are also always taken in a fixed order, circular waits are structurally impossible.

### **Journal Access Via mmap**

Journals are accessed through a shared `mmap()` of the binary journal file and updated in place — the 2-bit cells are modified with atomic byte operations directly on the mapping. There is no write-ahead log and no staging area: a journal update is an atomic bit operation on a mapped page, which is what keeps the overhead low enough to leave in the I/O path of a benchmark.

This is also why reading a journal is essentially free and why resuming one costs nothing more than opening and mapping the file. Only two operations ever touch the mapping as a whole: the count behind the `Verifiable:` percentage reported when a journal is resumed, and the final `msync()` when a `--journalsync` journal is closed.
