#!/bin/bash
#
# Concurrency stress for journaled data verification: no deadlocks, no hangs.
#
# Random journaled IO means several threads can target the same journal block at
# the same time, so the journal locks a range of blocks per IO. Readers and
# writers of the same block are mutually exclusive, only concurrent readers are
# allowed, and a thread that already holds a lock must never block for another
# one - it defers the IO instead. Getting that wrong deadlocks rather than
# producing wrong results, which is why this script exists.
#
# Contention is maximised by making the target *small* relative to the block
# size rather than by running long: a lock region covers at least one maximum
# size IO, so a large block size on a small target leaves very few regions for
# many threads with a deep queue to fight over.
#
# Every case does a bounded amount of work and therefore has to terminate on its
# own. A deadlock or a livelock shows up as run_elbencho hitting its timeout,
# which it reports as a TIMEOUT diagnostic naming the case.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/journal.sh" || exit 1

require_build_feature libaio

NUM_THREADS=8
IO_DEPTH=16

# 8 MiB in 1 MiB blocks is 8 lock regions for 8 threads with 16 requests each.
TINY_SIZE=$((8 * 1024 * 1024))
BIG_BLOCK=$((1024 * 1024))

# 64 MiB in 1 MiB blocks, so the threads spread out a little more.
LARGE_SIZE=$((64 * 1024 * 1024))

SMALL_BLOCK=4096
MIXED_BLOCKS="4K:1,1M:1"
TIMELIMIT=5

test_init
tap_plan 22

DATA_DIR="$TEST_DIR/data"
JOURNAL_DIR="$TEST_DIR/journal"
mkdir -p "$DATA_DIR" "$JOURNAL_DIR"

TINY_TARGET="$DATA_DIR/tinyfile"
LARGE_TARGET="$DATA_DIR/largefile"

################## Worst case lock region collisions ##################

# One lock region per 1 MiB block and only 8 of them, so almost every one of the
# 128 requests in flight wants a region that another request already holds.
run_elbencho tinyrand \
    -w -t $NUM_THREADS -s $TINY_SIZE -b $BIG_BLOCK --iodepth $IO_DEPTH --rand \
    --journaldir "$JOURNAL_DIR" \
    "$TINY_TARGET"
assert_ok $? "a random journaled write with $NUM_THREADS threads on $((TINY_SIZE / 1024 / 1024)) MiB completes"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$TINY_SIZE" \
    "it wrote exactly $TINY_SIZE bytes, so no request was lost or repeated"

run_elbencho tinyverify \
    -r -t $NUM_THREADS -s $TINY_SIZE -b $BIG_BLOCK --iodepth $IO_DEPTH \
    --journaldir "$JOURNAL_DIR" \
    "$TINY_TARGET"
assert_ok $? "the heavily contended journal still verifies"

run_elbencho largerand \
    -w -t $NUM_THREADS -s $LARGE_SIZE -b $BIG_BLOCK --iodepth $IO_DEPTH --rand \
    --journaldir "$JOURNAL_DIR" \
    "$LARGE_TARGET"
assert_ok $? "the same with $((LARGE_SIZE / 1024 / 1024)) MiB completes"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$LARGE_SIZE" \
    "it wrote exactly $LARGE_SIZE bytes"

################## Mixed block sizes, i.e. the deferral path ##################

# With a block size mix the small IOs land inside a lock region that a large IO
# of the same thread is likely to be holding, so most submissions have to be
# deferred until one of the thread's own requests completes. An explicit
# "--randalgo" is given because the full coverage generator does not support a
# block size mix, so this is the repeating generator with partial coverage -
# only bounds can be asserted for the byte count.
run_elbencho mixedblocks \
    -w -t $NUM_THREADS -s $LARGE_SIZE -b "$MIXED_BLOCKS" --iodepth $IO_DEPTH \
    --rand --randalgo balanced_single \
    --journaldir "$JOURNAL_DIR" \
    "$LARGE_TARGET"
assert_ok $? "a mixed block size random write, i.e. the deferral path, completes"
assert_gt "$(json_value "$ELB_JSON" WRITE last_done bytes)" 0 \
    "the deferral path actually transferred data"
assert_le "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$LARGE_SIZE" \
    "and it did not transfer more than the requested amount"

################## Readers and writers contending for the same blocks ##################

run_elbencho mixpct \
    -w -t $NUM_THREADS -s $LARGE_SIZE -b $SMALL_BLOCK --iodepth $IO_DEPTH --rand \
    --rwmixpct 50 \
    --journaldir "$JOURNAL_DIR" \
    "$LARGE_TARGET"
assert_ok $? "a 50% read mix under contention completes"

run_elbencho mixthr \
    -w -t $NUM_THREADS --rwmixthr 4 -s $LARGE_SIZE -b $SMALL_BLOCK --iodepth $IO_DEPTH --rand \
    --journaldir "$JOURNAL_DIR" \
    "$LARGE_TARGET"
assert_ok $? "four dedicated reader threads against four writers complete"

################## Durable journal updates under contention ##################

# Every journal update is msync'ed here, twice per write - once to make "being
# written" durable before the data write is issued and once for the committed
# generation - so this serializes far more than the default and is kept small.
# "--journalsync" implicitly enables direct IO, hence the O_DIRECT probe.
if fs_supports_directio "$TEST_DIR"; then

    run_elbencho syncjournal \
        -w -t $NUM_THREADS -s $TINY_SIZE -b $BIG_BLOCK --iodepth $IO_DEPTH --rand \
        --journaldir "$JOURNAL_DIR" --journalsync \
        "$TINY_TARGET"
    assert_ok $? "\"--journalsync\" under contention completes"

else
    tap_skip "\"--journalsync\" under contention completes"
fi

################## Sustained churn with a time limit ##################

# "--infloop" keeps the threads restarting their workload, so this runs until
# the time limit cuts it off. A time limit is not an error, so the run has to
# end by itself with exit code 0 - if the journal deadlocked, the threads could
# not react to the interruption and the command timeout would fire instead.
run_elbencho infloop \
    -w -t $NUM_THREADS --rwmixthr 4 -s $TINY_SIZE -b $SMALL_BLOCK \
    --iodepth $IO_DEPTH --rand --infloop --timelimit $TIMELIMIT \
    --journaldir "$JOURNAL_DIR" \
    "$TINY_TARGET"
assert_ok $? "a sustained read/write mix ends by itself at the time limit"
assert_match "$(cat "$ELB_OUT")" 'Terminating due to phase time limit' \
    "the run really ended because of the time limit"

################## IO depth sweep ##################

# The deadlock that this script is about only appears from an IO depth of 2
# upwards, because a single request in flight never holds a second lock.
for iodepth in 1 2 4 16 32; do
    run_elbencho "depth$iodepth" \
        -w -t $NUM_THREADS -s $TINY_SIZE -b $BIG_BLOCK --iodepth $iodepth --rand \
        --journaldir "$JOURNAL_DIR" \
        "$TINY_TARGET"
    assert_ok $? "a contended journaled write with IO depth $iodepth completes"
done

################## Nothing reported a lock problem ##################

# A thread that blocks for a journal lock it already holds gets EDEADLK from the
# kernel, which surfaces under this name. The exit codes above would catch it
# too, but naming it makes a regression obvious in the output.
assert_eq "$(grep -l 'Resource deadlock avoided' "$TEST_DIR"/*.out 2>/dev/null | wc -l)" "0" \
    "no run reported a resource deadlock"
assert_eq "$(grep -l 'Journal data verification failed' "$TEST_DIR"/*.out 2>/dev/null | wc -l)" "0" \
    "no run reported a verification failure"

################## The journal survived all of it consistently ##################

# Whatever is marked as written has to still hold what the journal says it
# holds. A journal update that was torn by concurrent access would show up here
# as a verification failure rather than as a hang.
run_elbencho finalverify \
    -r -t $NUM_THREADS -s $TINY_SIZE -b $SMALL_BLOCK --iodepth 1 \
    --journaldir "$JOURNAL_DIR" \
    "$TINY_TARGET"
assert_ok $? "the small target verifies after all the concurrent journal updates"

run_elbencho finalverifylarge \
    -r -t $NUM_THREADS -s $LARGE_SIZE -b $SMALL_BLOCK --iodepth 1 \
    --journaldir "$JOURNAL_DIR" \
    "$LARGE_TARGET"
assert_ok $? "the large target verifies after all the concurrent journal updates"
