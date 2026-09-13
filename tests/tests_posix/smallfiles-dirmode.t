#!/bin/bash
#
# Lots of small files in directory mode.
#
# Creates 400 small files spread over per-thread subdirs, reads and stats them
# and deletes everything again. Verifies the resulting tree on the file system,
# the entry and byte counters in the json result file, the individual operations
# in the operations log and the format of the live statistics csv file.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1

NUM_THREADS=4
NUM_DIRS=4      # per thread
NUM_FILES=25    # per thread and dir
FILE_SIZE=4096

# 4 threads x 4 dirs x 25 files
EXPECTED_FILES=$((NUM_THREADS * NUM_DIRS * NUM_FILES))
EXPECTED_BYTES=$((EXPECTED_FILES * FILE_SIZE))
# the "d" subdirs are the counted entries of the mkdirs phase...
EXPECTED_DIR_ENTRIES=$((NUM_THREADS * NUM_DIRS))
# ...while the per-thread "r" parent dirs exist on the file system in addition
EXPECTED_DIRS=$((EXPECTED_DIR_ENTRIES + NUM_THREADS))

test_init
tap_plan 26

DATA_DIR="$TEST_DIR/data"
OPSLOG="$TEST_DIR/write.opslog"
LIVECSV="$TEST_DIR/write.livecsv"

mkdir -p "$DATA_DIR"

################## Create dirs and write files ##################

run_elbencho write \
    -d -w \
    -t $NUM_THREADS -n $NUM_DIRS -N $NUM_FILES -s $FILE_SIZE -b $FILE_SIZE \
    --verify 1 --blockvarpct 0 \
    --opslog "$OPSLOG" --livecsv "$LIVECSV" --liveint 200 \
    "$DATA_DIR"
assert_ok $? "write phase of $EXPECTED_FILES small files succeeds"

assert_eq "$(count_files "$DATA_DIR")" "$EXPECTED_FILES" \
    "$EXPECTED_FILES files exist on the file system"
assert_eq "$(count_dirs "$DATA_DIR")" "$EXPECTED_DIRS" \
    "$EXPECTED_DIRS dirs exist on the file system"
assert_eq "$(unique_file_sizes "$DATA_DIR")" "$FILE_SIZE" \
    "all created files have a size of $FILE_SIZE bytes"

assert_eq "$(json_value "$ELB_JSON" MKDIRS last_done entries)" "$EXPECTED_DIR_ENTRIES" \
    "mkdirs phase reports $EXPECTED_DIR_ENTRIES created dirs"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done entries)" "$EXPECTED_FILES" \
    "write phase reports $EXPECTED_FILES written files"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$EXPECTED_BYTES" \
    "write phase reports $EXPECTED_BYTES written bytes"
assert_gt "$(json_value "$ELB_JSON" WRITE last_done 'bytes/s')" 0 \
    "write phase reports a non-zero throughput"
assert_eq "$(json_config "$ELB_JSON" WRITE file_size)" "$FILE_SIZE" \
    "write phase result confirms the given file size"

assert_eq "$(opslog_count "$OPSLOG" pwrite)" "$EXPECTED_FILES" \
    "operations log contains $EXPECTED_FILES completed pwrite operations"
assert_eq "$(opslog_count "$OPSLOG" openat)" "$EXPECTED_FILES" \
    "operations log contains $EXPECTED_FILES completed openat operations"
assert_eq "$(opslog_count "$OPSLOG" close)" "$EXPECTED_FILES" \
    "operations log contains $EXPECTED_FILES completed close operations"
assert_eq "$(opslog_count "$OPSLOG" mkdirat)" "$EXPECTED_DIRS" \
    "operations log contains $EXPECTED_DIRS completed mkdirat operations"
assert_eq "$(opslog_error_count "$OPSLOG")" "0" \
    "operations log contains no failed operation"

assert_eq "$(livecsv_header "$LIVECSV")" "$LIVECSV_HEADER" \
    "live statistics csv file has the expected header line"
assert_eq "$(livecsv_bad_rows "$LIVECSV")" "0" \
    "all rows of the live statistics csv file have the expected number of fields"

################## Read files and query their attributes ##################

# A different block size than in the write phase, so that the data integrity
# check also covers reading across block boundaries.
run_elbencho read \
    -r --stat \
    -t $NUM_THREADS -n $NUM_DIRS -N $NUM_FILES -s $FILE_SIZE -b 2048 \
    --verify 1 \
    "$DATA_DIR"
assert_ok $? "read phase with data integrity check and stat phase succeed"

assert_eq "$(json_value "$ELB_JSON" READ last_done entries)" "$EXPECTED_FILES" \
    "read phase reports $EXPECTED_FILES read files"
assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$EXPECTED_BYTES" \
    "read phase reports $EXPECTED_BYTES read bytes"
assert_gt "$(json_value "$ELB_JSON" READ last_done iops)" 0 \
    "read phase reports non-zero IOPS"
assert_eq "$(json_value "$ELB_JSON" STAT last_done entries)" "$EXPECTED_FILES" \
    "stat phase reports $EXPECTED_FILES stat'ed files"

################## Delete files and dirs ##################

run_elbencho delete \
    -F -D --nodelerr \
    -t $NUM_THREADS -n $NUM_DIRS -N $NUM_FILES \
    "$DATA_DIR"
assert_ok $? "delete phase of files and dirs succeeds"

assert_eq "$(json_value "$ELB_JSON" RMFILES last_done entries)" "$EXPECTED_FILES" \
    "rmfiles phase reports $EXPECTED_FILES deleted files"
assert_eq "$(json_value "$ELB_JSON" RMDIRS last_done entries)" "$EXPECTED_DIR_ENTRIES" \
    "rmdirs phase reports $EXPECTED_DIR_ENTRIES deleted dirs"

assert_eq "$(count_files "$DATA_DIR")" "0" \
    "no files are left after the delete phase"
assert_eq "$(count_dirs "$DATA_DIR")" "0" \
    "no dirs are left after the delete phase"
