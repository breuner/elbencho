#!/bin/bash
#
# Containment of journaled IO within "--offset" and "--size".
#
# A journal maps an absolute target offset to a journal block index relative to
# the offset it was created for, so an access outside that window would not just
# touch the wrong data, it would touch the wrong journal cell. This checks the
# window two independent ways: the offsets elbencho logged, and the contents of
# the target file itself, which is prefilled with 0xFF so that written and
# untouched regions can be told apart byte-exactly.
#
# The window also becomes a property of the journal: the range a journal was
# created for is recorded in its sidecar, and later runs have to stay inside it.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/journal.sh" || exit 1

FILE_SIZE_MIB=32
OFFSET_MIB=8
LENGTH_MIB=16
END_MIB=$((OFFSET_MIB + LENGTH_MIB))

OFFSET=$((OFFSET_MIB * 1024 * 1024))
LENGTH=$((LENGTH_MIB * 1024 * 1024))
END=$((OFFSET + LENGTH))

BLOCK_SIZE=$((64 * 1024))
BIG_BLOCK_SIZE=$((256 * 1024))
NUM_THREADS=2
IO_DEPTH=8
JOURNAL_BLOCK=4096

EXPECTED_BLOCKS=$((LENGTH / BLOCK_SIZE))

test_init
tap_plan 32

DATA_DIR="$TEST_DIR/data"
mkdir -p "$DATA_DIR"

# assert_window_contained OPSLOG DESC_SUFFIX
# The three range properties of a full, sequential pass over the window. They
# are exact rather than bounds, because every block of the window is written
# exactly once.
assert_window_contained()
{
    local opslog="$1"
    local what="$2"

    assert_eq "$(opslog_min_offset "$opslog")" "$OFFSET" \
        "the lowest accessed offset is exactly the given offset ($what)"
    assert_eq "$(opslog_max_end "$opslog")" "$END" \
        "the highest accessed offset plus length is exactly offset plus size ($what)"
    assert_eq "$(opslog_bytes_sum "$opslog")" "$LENGTH" \
        "the logged block sizes sum up to exactly the given size ($what)"
}

################## Synchronous write of a window ##################

TARGET="$DATA_DIR/rangefile"
JOURNAL_DIR="$TEST_DIR/journal-range"
mkdir -p "$JOURNAL_DIR"

fill_file_ff "$TARGET" $FILE_SIZE_MIB
assert_eq "$(file_size "$TARGET")" "$((FILE_SIZE_MIB * 1024 * 1024))" \
    "the target is prefilled with 0xFF over its full size"

OPSLOG="$TEST_DIR/syncwrite.opslog"

run_elbencho syncwrite \
    -w -t $NUM_THREADS -b $BLOCK_SIZE --offset $OFFSET -s $LENGTH --iodepth 1 \
    --journaldir "$JOURNAL_DIR" \
    --opslog "$OPSLOG" \
    "$TARGET"
assert_ok $? "a journaled write of a limited range succeeds"

assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$LENGTH" \
    "the write phase reports exactly $LENGTH written bytes"
assert_eq "$(opslog_count "$OPSLOG" pwrite)" "$EXPECTED_BLOCKS" \
    "the operations log has exactly $EXPECTED_BLOCKS write operations"
assert_eq "$(opslog_error_count "$OPSLOG")" "0" \
    "no operation was logged as failed"

assert_window_contained "$OPSLOG" "synchronous write"

# The journal was created for the requested window, not for the whole file, so
# its own offset to block index arithmetic is anchored at the offset.
assert_eq "$(journal_field "$JOURNAL_DIR" "$TARGET" targetOffset)" "$OFFSET" \
    "the sidecar records the given offset as its target offset"
assert_eq "$(journal_field "$JOURNAL_DIR" "$TARGET" targetSize)" "$LENGTH" \
    "the sidecar records the given size as its target size"
assert_eq "$(file_size "$(journal_binary_for "$JOURNAL_DIR" "$TARGET")")" \
    "$(journal_expected_binary_size $LENGTH $JOURNAL_BLOCK)" \
    "the binary journal covers the window only, not the whole file"

################## The file itself confirms the window ##################

assert_eq "$(region_is_not_ff "$TARGET" 0 $OFFSET_MIB)" "0" \
    "the $OFFSET_MIB MiB below the given offset are untouched"
assert_eq "$(region_is_not_ff "$TARGET" $END_MIB $((FILE_SIZE_MIB - END_MIB)))" "0" \
    "everything above offset plus size is untouched"
assert_gt "$(region_is_not_ff "$TARGET" $OFFSET_MIB $LENGTH_MIB)" 0 \
    "the region within offset and size was written"

################## Reading the window back ##################

OPSLOG="$TEST_DIR/syncread.opslog"

run_elbencho syncread \
    -r -t $NUM_THREADS -b $BIG_BLOCK_SIZE --offset $OFFSET -s $LENGTH --iodepth 1 \
    --journaldir "$JOURNAL_DIR" \
    --opslog "$OPSLOG" \
    "$TARGET"
assert_ok $? "reading the window back with a larger block size verifies"

assert_eq "$(opslog_min_offset "$OPSLOG")" "$OFFSET" \
    "the read phase also starts exactly at the given offset"
assert_eq "$(opslog_max_end "$OPSLOG")" "$END" \
    "the read phase also ends exactly at offset plus size"

################## Random IO stays inside the window ##################

# Random offsets are drawn from the window, so only bounds can be asserted
# here - but a wrong window would show up immediately.
OPSLOG="$TEST_DIR/randwrite.opslog"

run_elbencho randwrite \
    -w -t $NUM_THREADS -b $BLOCK_SIZE --offset $OFFSET -s $LENGTH --iodepth 1 --rand \
    --journaldir "$JOURNAL_DIR" \
    --opslog "$OPSLOG" \
    "$TARGET"
assert_ok $? "a journaled random write inside the window succeeds"

assert_ge "$(opslog_min_offset "$OPSLOG")" "$OFFSET" \
    "no random access went below the given offset"
assert_le "$(opslog_max_end "$OPSLOG")" "$END" \
    "no random access went beyond offset plus size"

################## The asynchronous engine, on its own target ##################

if has_build_feature libaio; then

    ASYNC_TARGET="$DATA_DIR/asyncrangefile"
    ASYNC_JOURNAL="$TEST_DIR/journal-asyncrange"
    mkdir -p "$ASYNC_JOURNAL"

    fill_file_ff "$ASYNC_TARGET" $FILE_SIZE_MIB

    OPSLOG="$TEST_DIR/asyncwrite.opslog"

    run_elbencho asyncwrite \
        -w -t $NUM_THREADS -b $BLOCK_SIZE --offset $OFFSET -s $LENGTH --iodepth $IO_DEPTH \
        --journaldir "$ASYNC_JOURNAL" \
        --opslog "$OPSLOG" \
        "$ASYNC_TARGET"
    assert_ok $? "an asynchronous journaled write of the window succeeds"

    assert_window_contained "$OPSLOG" "asynchronous write"

    assert_eq "$(region_is_not_ff "$ASYNC_TARGET" 0 $OFFSET_MIB)" "0" \
        "the asynchronous engine also left everything below the offset untouched"
    assert_eq "$(region_is_not_ff "$ASYNC_TARGET" $END_MIB $((FILE_SIZE_MIB - END_MIB)))" "0" \
        "the asynchronous engine also left everything above the window untouched"

else
    tap_skip "an asynchronous journaled write of the window succeeds"
    tap_skip "the lowest accessed offset is exactly the given offset (asynchronous write)"
    tap_skip "the highest accessed offset plus length is exactly offset plus size (async write)"
    tap_skip "the logged block sizes sum up to exactly the given size (asynchronous write)"
    tap_skip "the asynchronous engine also left everything below the offset untouched"
    tap_skip "the asynchronous engine also left everything above the window untouched"
fi

################## A journal's window is binding for later runs ##################

# Each of these must be rejected before any IO happens.
check_rejected()
{
    local tag="$1"
    local expected="$2"
    shift 2

    run_elbencho "$tag" "$@" > /dev/null 2>&1

    if [ $? -eq 0 ]; then
        tap_fail "$tag is rejected"
        tap_diag "  the run unexpectedly succeeded"
        return
    fi

    assert_match "$(cat "$ELB_OUT")" "$expected" "$tag is rejected as expected"
}

RANGE_RE='range exceeds the range that the existing journal'

check_rejected "a range wider than the journal's" "$RANGE_RE" \
    -r -t 1 -b $BLOCK_SIZE --offset 0 -s $((FILE_SIZE_MIB * 1024 * 1024)) \
    --journaldir "$JOURNAL_DIR" "$TARGET"

check_rejected "a range starting below the journal's offset" "$RANGE_RE" \
    -r -t 1 -b $BLOCK_SIZE --offset $((OFFSET - 4 * 1024 * 1024)) -s $LENGTH \
    --journaldir "$JOURNAL_DIR" "$TARGET"

check_rejected "a range ending above the journal's window" "$RANGE_RE" \
    -r -t 1 -b $BLOCK_SIZE --offset $OFFSET -s $((LENGTH + 4 * 1024 * 1024)) \
    --journaldir "$JOURNAL_DIR" "$TARGET"

# A narrower window is contained in the journal's range and therefore fine,
# which is how a run can verify just a part of what it wrote earlier.
NARROW_OFFSET=$((OFFSET + 4 * 1024 * 1024))
NARROW_LENGTH=$((4 * 1024 * 1024))
OPSLOG="$TEST_DIR/narrowread.opslog"

run_elbencho narrowread \
    -r -t 1 -b $BLOCK_SIZE --offset $NARROW_OFFSET -s $NARROW_LENGTH --iodepth 1 \
    --journaldir "$JOURNAL_DIR" \
    --opslog "$OPSLOG" \
    "$TARGET"
assert_ok $? "a range narrower than the journal's is accepted"

assert_eq "$(opslog_min_offset "$OPSLOG")" "$NARROW_OFFSET" \
    "the narrower run starts at its own offset"
assert_eq "$(opslog_max_end "$OPSLOG")" "$((NARROW_OFFSET + NARROW_LENGTH))" \
    "the narrower run ends at its own offset plus size"
