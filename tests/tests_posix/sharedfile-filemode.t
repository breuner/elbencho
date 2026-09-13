#!/bin/bash
#
# Large shared files in file mode.
#
# The benchmark paths are two files instead of a directory, so all worker
# threads share the given files and each of them works on a range of the file.
# Covers the shared file descriptor default, "--nofdsharing", a sequential read
# with data integrity check, a random read and, if the file system supports it,
# direct IO.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1

NUM_THREADS=8
FILE_SIZE=$((16 * 1024 * 1024))
BLOCK_SIZE=$((1024 * 1024))

EXPECTED_FILES=2
EXPECTED_BYTES=$((EXPECTED_FILES * FILE_SIZE))

test_init
tap_plan 20

DATA_DIR="$TEST_DIR/data"
OPSLOG="$TEST_DIR/write.opslog"

mkdir -p "$DATA_DIR"

# Quoted, so that elbencho itself expands the number range in the brackets.
BENCH_PATHS="$DATA_DIR/file[1-2]"

################## Write shared files ##################

run_elbencho write \
    -w \
    -t $NUM_THREADS -s $FILE_SIZE -b $BLOCK_SIZE \
    --verify 1 --blockvarpct 0 \
    --opslog "$OPSLOG" \
    "$BENCH_PATHS"
assert_ok $? "write phase of $EXPECTED_FILES shared files succeeds"

assert_eq "$(count_files "$DATA_DIR")" "$EXPECTED_FILES" \
    "the number range in the given path created $EXPECTED_FILES files"
assert_eq "$(file_size "$DATA_DIR/file1")" "$FILE_SIZE" \
    "file1 has a size of $FILE_SIZE bytes"
assert_eq "$(file_size "$DATA_DIR/file2")" "$FILE_SIZE" \
    "file2 has a size of $FILE_SIZE bytes"

assert_eq "$(json_config "$ELB_JSON" WRITE path_type)" "file" \
    "write phase result reports a file as benchmark path type"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$EXPECTED_BYTES" \
    "write phase reports $EXPECTED_BYTES written bytes"
assert_gt "$(json_value "$ELB_JSON" WRITE last_done 'bytes/s')" 0 \
    "write phase reports a non-zero throughput"

assert_eq "$(opslog_error_count "$OPSLOG")" "0" \
    "operations log contains no failed operation"
# With fewer files than threads, several threads have to share each file.
assert_ge "$(opslog_max_distinct_ranks "$OPSLOG" pwrite)" "2" \
    "operations log confirms that a file was written by multiple threads"

################## Write again without sharing the file descriptor ##################

run_elbencho nofdsharing \
    -w \
    -t $NUM_THREADS -s $FILE_SIZE -b $BLOCK_SIZE \
    --verify 1 --blockvarpct 0 \
    --nofdsharing \
    "$BENCH_PATHS"
assert_ok $? "write phase with \"--nofdsharing\" succeeds"

assert_eq "$(unique_file_sizes "$DATA_DIR")" "$FILE_SIZE" \
    "file sizes are unchanged after the \"--nofdsharing\" write phase"

################## Read shared files back ##################

run_elbencho read \
    -r \
    -t $NUM_THREADS -s $FILE_SIZE -b $BLOCK_SIZE \
    --verify 1 \
    "$BENCH_PATHS"
assert_ok $? "sequential read phase with data integrity check succeeds"

assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$EXPECTED_BYTES" \
    "read phase reports $EXPECTED_BYTES read bytes"
assert_gt "$(json_value "$ELB_JSON" READ last_done 'bytes/s')" 0 \
    "read phase reports a non-zero throughput"

################## Random read ##################

run_elbencho randread \
    -r --rand \
    -t $NUM_THREADS -s $FILE_SIZE -b 4096 --randamount $((4 * 1024 * 1024)) \
    "$BENCH_PATHS"
assert_ok $? "random read phase succeeds"

assert_gt "$(json_value "$ELB_JSON" READ last_done bytes)" 0 \
    "random read phase reports a non-zero amount of read bytes"
assert_gt "$(json_value "$ELB_JSON" READ last_done iops)" 0 \
    "random read phase reports non-zero IOPS"

################## Direct IO read ##################

# Not all file systems support O_DIRECT, so this is skipped where unavailable.
if fs_supports_directio "$TEST_DIR"; then
    run_elbencho directread \
        -r --direct \
        -t $NUM_THREADS -s $FILE_SIZE -b $BLOCK_SIZE \
        --verify 1 \
        "$BENCH_PATHS"
    assert_ok $? "direct IO read phase with data integrity check succeeds"
else
    tap_skip "direct IO read phase (file system does not support O_DIRECT)"
fi

################## Delete files ##################

run_elbencho delete \
    -F --nodelerr \
    -t $NUM_THREADS \
    "$BENCH_PATHS"
assert_ok $? "delete phase of the shared files succeeds"

assert_eq "$(count_files "$DATA_DIR")" "0" \
    "no files are left after the delete phase"
