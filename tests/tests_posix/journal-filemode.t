#!/bin/bash
#
# Journaled data verification in file mode, in both IO engines.
#
# "--journaldir" keeps one journal per target, tracking which blocks have been
# written so their content can be verified later - after a crash, or during a
# read/write mix phase. Unlike "--verify" it needs no salt, because the expected
# content is derived from the journal's own seed, the absolute offset and a
# generation counter that advances on every rewrite of a block.
#
# This script covers the basic write/verify cycle and the on-disk shape of a
# journal. The engine specific part matters because the journal hooks into the
# synchronous and the asynchronous engine in different places, so both are run
# and the operations log is what proves which engine was actually used.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/journal.sh" || exit 1

NUM_THREADS=2
FILE_SIZE=$((8 * 1024 * 1024))
BLOCK_SIZE=$((64 * 1024))
BIG_BLOCK_SIZE=$((256 * 1024))
IO_DEPTH=8
JOURNAL_BLOCK=4096

# Keep the size an exact multiple of block size times threads, otherwise the
# random full coverage generator below leaves the tail blocks undrawn.
EXPECTED_BLOCKS=$((FILE_SIZE / BLOCK_SIZE))

test_init
tap_plan 41

DATA_DIR="$TEST_DIR/data"
TARGET="$DATA_DIR/file1"
JOURNAL_DIR="$TEST_DIR/journal"

mkdir -p "$DATA_DIR" "$JOURNAL_DIR"

################## Synchronous write, and the journal it creates ##################

OPSLOG="$TEST_DIR/syncwrite.opslog"

run_elbencho syncwrite \
    -w -t $NUM_THREADS -s $FILE_SIZE -b $BLOCK_SIZE --iodepth 1 \
    --journaldir "$JOURNAL_DIR" \
    --opslog "$OPSLOG" \
    "$TARGET"
assert_ok $? "synchronous journaled write phase succeeds"

assert_match "$(cat "$ELB_OUT")" 'Created new journal for target' \
    "the run reports that it created a new journal"
assert_eq "$(journal_sidecar_count "$JOURNAL_DIR")" "1" \
    "exactly one journal sidecar exists for the one target"
assert_eq "$(journal_file_count "$JOURNAL_DIR")" "2" \
    "the journal consists of a sidecar and a binary file"

assert_eq "$(journal_field "$JOURNAL_DIR" "$TARGET" target)" "$TARGET" \
    "the sidecar names the benchmark path as its target"
assert_eq "$(journal_field "$JOURNAL_DIR" "$TARGET" journalBlockSize)" "$JOURNAL_BLOCK" \
    "the sidecar records the default journal block size of $JOURNAL_BLOCK"
assert_eq "$(journal_field "$JOURNAL_DIR" "$TARGET" targetOffset)" "0" \
    "the sidecar records a target offset of 0"
assert_eq "$(journal_field "$JOURNAL_DIR" "$TARGET" targetSize)" "$FILE_SIZE" \
    "the sidecar records the given size as its target size"

JOURNAL_BIN="$(journal_binary_for "$JOURNAL_DIR" "$TARGET")"

assert_eq "$(file_size "$JOURNAL_BIN")" \
    "$(journal_expected_binary_size $FILE_SIZE $JOURNAL_BLOCK)" \
    "the binary journal has the size of two bits per journal block"
# The journal is preallocated with posix_fallocate on creation, so that an
# mmap'ed write can never hit a full file system mid-run. A journal that was
# only ftruncate'd would still be sparse and report zero allocated blocks.
assert_gt "$(allocated_kb "$JOURNAL_BIN")" 0 \
    "the binary journal has disk space allocated, i.e. it is not sparse"

assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$FILE_SIZE" \
    "the write phase reports $FILE_SIZE written bytes"
assert_eq "$(opslog_count "$OPSLOG" pwrite)" "$EXPECTED_BLOCKS" \
    "the operations log has $EXPECTED_BLOCKS synchronous pwrite operations"
assert_eq "$(opslog_count "$OPSLOG" aiowrite)" "0" \
    "the operations log has no asynchronous write operation"
assert_eq "$(opslog_error_count "$OPSLOG")" "0" \
    "no operation was logged as failed"

################## Synchronous read-back and verification ##################

OPSLOG="$TEST_DIR/syncread.opslog"

run_elbencho syncread \
    -r -t $NUM_THREADS -s $FILE_SIZE -b $BLOCK_SIZE --iodepth 1 \
    --journaldir "$JOURNAL_DIR" \
    --opslog "$OPSLOG" \
    "$TARGET"
assert_ok $? "synchronous journaled read phase verifies the written content"

assert_match "$(cat "$ELB_OUT")" 'Resumed existing journal for target' \
    "the read phase resumed the existing journal instead of creating one"
assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$FILE_SIZE" \
    "the read phase reports $FILE_SIZE read bytes, i.e. nothing was skipped"
assert_eq "$(opslog_count "$OPSLOG" pread)" "$EXPECTED_BLOCKS" \
    "the operations log has $EXPECTED_BLOCKS synchronous pread operations"

# A larger block size on read means every IO spans several journal blocks, each
# with its own expected content, so this is what proves the content is anchored
# to the journal block and not to whatever block size happened to write it.
run_elbencho bigblockread \
    -r -t $NUM_THREADS -s $FILE_SIZE -b $BIG_BLOCK_SIZE --iodepth 1 \
    --journaldir "$JOURNAL_DIR" \
    "$TARGET"
assert_ok $? "reading back with a larger block size verifies as well"
assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$FILE_SIZE" \
    "the read phase with a larger block size also reports $FILE_SIZE read bytes"

################## Durable journal updates ##################

# "--journalsync" makes every journal update durable instead of leaving it in
# the page cache, which is what allows a resume after a power failure rather
# than only after a process crash. The msync ordering itself is not observable
# from the outside, so what is checked here is that it changes nothing
# functionally - and that it implicitly enables direct IO, which it needs
# because a committed generation is only meaningful if the target write it
# describes is durable once it completes.
#
# That implicit direct IO is also why these cases need the O_DIRECT probe: on a
# file system that rejects O_DIRECT, "--journalsync" cannot work at all.
if fs_supports_directio "$TEST_DIR"; then

    run_elbencho syncjournalwrite \
        -w -t $NUM_THREADS -s $FILE_SIZE -b $BLOCK_SIZE --iodepth 1 \
        --journaldir "$JOURNAL_DIR" --journalsync \
        "$TARGET"
    assert_ok $? "a write phase with \"--journalsync\" succeeds"

    assert_eq "$(json_config "$ELB_JSON" WRITE direct_io)" "true" \
        "\"--journalsync\" implicitly enabled direct IO"

    run_elbencho syncjournalread \
        -r -t $NUM_THREADS -s $FILE_SIZE -b $BLOCK_SIZE --iodepth 1 \
        --journaldir "$JOURNAL_DIR" \
        "$TARGET"
    assert_ok $? "the content written with \"--journalsync\" verifies"

else
    tap_skip "a write phase with \"--journalsync\" succeeds"
    tap_skip "\"--journalsync\" implicitly enabled direct IO"
    tap_skip "the content written with \"--journalsync\" verifies"
fi

# The implicit direct IO is scoped to "--journalsync": a plain journaled run
# must not silently change what the benchmark measures.
run_elbencho nodirectio \
    -w -t $NUM_THREADS -s $FILE_SIZE -b $BLOCK_SIZE --iodepth 1 \
    --journaldir "$JOURNAL_DIR" \
    "$TARGET"
assert_ok $? "a journaled write without \"--journalsync\" succeeds"
assert_eq "$(json_config "$ELB_JSON" WRITE direct_io)" "false" \
    "journaling alone does not enable direct IO"

################## Random offsets ##################

# A random aligned write phase without an explicit "--randalgo" and without a
# block size mix uses the full coverage generator, which visits every block
# exactly once. So this is the random counterpart with predictable results, and
# it leaves every journal block initialized for the sequential read after it.
run_elbencho randwrite \
    -w -t $NUM_THREADS -s $FILE_SIZE -b $BLOCK_SIZE --iodepth 1 --rand \
    --journaldir "$JOURNAL_DIR" \
    "$TARGET"
assert_ok $? "a journaled random write with full block coverage succeeds"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$FILE_SIZE" \
    "the random write phase reports $FILE_SIZE written bytes"

run_elbencho randverify \
    -r -t $NUM_THREADS -s $FILE_SIZE -b $BLOCK_SIZE --iodepth 1 \
    --journaldir "$JOURNAL_DIR" \
    "$TARGET"
assert_ok $? "reading the randomly written content back verifies"
assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$FILE_SIZE" \
    "nothing was skipped, so the random write really covered every block"

################## Asynchronous engine ##################

if has_build_feature libaio; then

    OPSLOG="$TEST_DIR/asyncwrite.opslog"

    run_elbencho asyncwrite \
        -w -t $NUM_THREADS -s $FILE_SIZE -b $BLOCK_SIZE --iodepth $IO_DEPTH \
        --journaldir "$JOURNAL_DIR" \
        --opslog "$OPSLOG" \
        "$TARGET"
    assert_ok $? "asynchronous journaled write phase succeeds"

    assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$FILE_SIZE" \
        "the asynchronous write phase reports $FILE_SIZE written bytes"
    assert_eq "$(opslog_count "$OPSLOG" aiowrite)" "$EXPECTED_BLOCKS" \
        "the operations log has $EXPECTED_BLOCKS asynchronous aiowrite operations"
    assert_eq "$(opslog_count "$OPSLOG" pwrite)" "0" \
        "the operations log has no synchronous write operation"
    assert_eq "$(opslog_error_count "$OPSLOG")" "0" \
        "no asynchronous operation was logged as failed"

    OPSLOG="$TEST_DIR/asyncread.opslog"

    run_elbencho asyncread \
        -r -t $NUM_THREADS -s $FILE_SIZE -b $BLOCK_SIZE --iodepth $IO_DEPTH \
        --journaldir "$JOURNAL_DIR" \
        --opslog "$OPSLOG" \
        "$TARGET"
    assert_ok $? "asynchronous journaled read phase verifies the written content"

    assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$FILE_SIZE" \
        "the asynchronous read phase reports $FILE_SIZE read bytes"
    assert_eq "$(opslog_count "$OPSLOG" aioread)" "$EXPECTED_BLOCKS" \
        "the operations log has $EXPECTED_BLOCKS asynchronous aioread operations"

    # The journal is engine independent, so content written by one engine has to
    # verify in the other. The asynchronous read above already covered one
    # direction, this is the other one.
    run_elbencho crossenginewrite \
        -w -t $NUM_THREADS -s $FILE_SIZE -b $BLOCK_SIZE --iodepth $IO_DEPTH \
        --journaldir "$JOURNAL_DIR" \
        "$TARGET"
    assert_ok $? "asynchronous write for the cross engine check succeeds"

    run_elbencho crossengineread \
        -r -t $NUM_THREADS -s $FILE_SIZE -b $BLOCK_SIZE --iodepth 1 \
        --journaldir "$JOURNAL_DIR" \
        "$TARGET"
    assert_ok $? "content written asynchronously verifies in the synchronous engine"

else
    tap_skip "asynchronous journaled write phase succeeds"
    tap_skip "the asynchronous write phase reports the expected written bytes"
    tap_skip "the operations log has the expected asynchronous aiowrite operations"
    tap_skip "the operations log has no synchronous write operation"
    tap_skip "no asynchronous operation was logged as failed"
    tap_skip "asynchronous journaled read phase verifies the written content"
    tap_skip "the asynchronous read phase reports the expected read bytes"
    tap_skip "the operations log has the expected asynchronous aioread operations"
    tap_skip "asynchronous write for the cross engine check succeeds"
    tap_skip "content written asynchronously verifies in the synchronous engine"
fi

################## The journal dir is still a single journal ##################

# Every run above used the same target under the same path string, so all of
# them had to resume the same journal. A second journal here would mean the
# target was not recognized, which would silently disable verification.
assert_eq "$(journal_sidecar_count "$JOURNAL_DIR")" "1" \
    "all runs shared the one journal, no duplicate was created"
assert_eq "$(journal_file_count "$JOURNAL_DIR")" "2" \
    "the journal dir still holds exactly one sidecar and one binary"
