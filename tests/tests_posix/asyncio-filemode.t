#!/bin/bash
#
# Asynchronous IO in file mode via "--iodepth".
#
# An "--iodepth" greater than 1 switches elbencho from pread/pwrite to the libaio
# based engine. The operations log is what proves that this really happened,
# because it logs the operation names of the engine that was used, so a run with
# "--iodepth 1" serves as the synchronous counterpart.
#
# The last case is the combination of direct IO, random offsets, asynchronous IO
# and preallocation, which is how a block device would be benchmarked - covered
# here in file mode, because creating a block device requires root privileges.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1

require_build_feature libaio

NUM_THREADS=2
IO_DEPTH=4
FILE_SIZE=$((4 * 1024 * 1024))
BLOCK_SIZE=$((64 * 1024))

EXPECTED_FILES=2
EXPECTED_BYTES=$((EXPECTED_FILES * FILE_SIZE))
EXPECTED_BLOCKS=$((EXPECTED_BYTES / BLOCK_SIZE))

# Random IO case: 13 files of 1MiB in 4KiB blocks, spread over 4 threads. The
# number of blocks is a multiple of the thread count, so all of them are used.
RAND_THREADS=4
RAND_FILES=13
RAND_FILE_SIZE=$((1024 * 1024))
RAND_BLOCK_SIZE=$((4 * 1024))
RAND_BYTES=$((RAND_FILES * RAND_FILE_SIZE))

test_init
tap_plan 20

DATA_DIR="$TEST_DIR/data"
BENCH_PATHS="$DATA_DIR/file[1-2]"
RAND_PATHS="$DATA_DIR/testfile[1-$RAND_FILES]"

mkdir -p "$DATA_DIR"

################## Asynchronous write ##################

OPSLOG="$TEST_DIR/aiowrite.opslog"

run_elbencho aiowrite \
    -w \
    -t $NUM_THREADS -s $FILE_SIZE -b $BLOCK_SIZE --iodepth $IO_DEPTH \
    --verify 1 --blockvarpct 0 \
    --opslog "$OPSLOG" \
    "$BENCH_PATHS"
assert_ok $? "asynchronous write phase with an io depth of $IO_DEPTH succeeds"

assert_eq "$(json_config "$ELB_JSON" WRITE io_depth)" "$IO_DEPTH" \
    "write phase result confirms the given io depth"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$EXPECTED_BYTES" \
    "write phase reports $EXPECTED_BYTES written bytes"
assert_eq "$(unique_file_sizes "$DATA_DIR")" "$FILE_SIZE" \
    "both files have a size of $FILE_SIZE bytes"

assert_eq "$(opslog_count "$OPSLOG" aiowrite)" "$EXPECTED_BLOCKS" \
    "the operations log has $EXPECTED_BLOCKS completed aiowrite operations"
assert_eq "$(opslog_count "$OPSLOG" pwrite)" "0" \
    "the operations log has no synchronous pwrite operation"
assert_eq "$(opslog_error_count "$OPSLOG")" "0" \
    "no operation was logged as failed"

################## Asynchronous read ##################

OPSLOG="$TEST_DIR/aioread.opslog"

run_elbencho aioread \
    -r \
    -t $NUM_THREADS -s $FILE_SIZE -b $BLOCK_SIZE --iodepth $IO_DEPTH \
    --verify 1 --blockvarpct 0 \
    --opslog "$OPSLOG" \
    "$BENCH_PATHS"
assert_ok $? "asynchronous read phase with data verification succeeds"

assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$EXPECTED_BYTES" \
    "read phase reports $EXPECTED_BYTES read bytes"
assert_eq "$(opslog_count "$OPSLOG" aioread)" "$EXPECTED_BLOCKS" \
    "the operations log has $EXPECTED_BLOCKS completed aioread operations"
assert_eq "$(opslog_count "$OPSLOG" pread)" "0" \
    "the operations log has no synchronous pread operation"

################## Synchronous counterpart ##################

OPSLOG="$TEST_DIR/syncwrite.opslog"

run_elbencho syncwrite \
    -w \
    -t $NUM_THREADS -s $FILE_SIZE -b $BLOCK_SIZE --iodepth 1 \
    --verify 1 --blockvarpct 0 \
    --opslog "$OPSLOG" \
    "$BENCH_PATHS"
assert_ok $? "synchronous write phase with an io depth of 1 succeeds"

assert_eq "$(json_config "$ELB_JSON" WRITE io_depth)" "1" \
    "write phase result confirms the io depth of 1"
assert_eq "$(opslog_count "$OPSLOG" pwrite)" "$EXPECTED_BLOCKS" \
    "the operations log has $EXPECTED_BLOCKS synchronous pwrite operations"
assert_eq "$(opslog_count "$OPSLOG" aiowrite)" "0" \
    "the operations log has no asynchronous aiowrite operation"

################## Direct, random, asynchronous and preallocated ##################

if fs_supports_directio "$TEST_DIR"; then

    run_elbencho directrand \
        -w \
        -t $RAND_THREADS -s $RAND_FILE_SIZE -b $RAND_BLOCK_SIZE \
        --rand --direct --iodepth 2 --preallocfile \
        "$RAND_PATHS"
    assert_ok $? "random direct IO write with an io depth of 2 succeeds"

    assert_eq "$(json_config "$ELB_JSON" WRITE direct_io)" "true" \
        "write phase result confirms direct IO"
    assert_eq "$(json_config "$ELB_JSON" WRITE random_offsets)" "true" \
        "write phase result confirms random offsets"
    assert_eq "$(json_config "$ELB_JSON" WRITE io_depth)" "2" \
        "write phase result confirms the io depth of 2"
    assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$RAND_BYTES" \
        "write phase reports $RAND_BYTES written bytes"

else
    tap_skip "random direct IO write with an io depth of 2 succeeds"
    tap_skip "write phase result confirms direct IO"
    tap_skip "write phase result confirms random offsets"
    tap_skip "write phase result confirms the io depth of 2"
    tap_skip "write phase reports the expected number of written bytes"
fi
