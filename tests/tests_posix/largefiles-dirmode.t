#!/bin/bash
#
# Large files in directory mode.
#
# Writes 4 large files into a shared dir ("-n 0"), reads them back with a
# different block size and a data integrity check, then deletes them. Verifies
# the file sizes on the file system as well as the entry and byte counters of
# the json result file.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1

NUM_THREADS=4
FILE_SIZE=$((32 * 1024 * 1024))
WRITE_BLOCK_SIZE=$((1024 * 1024))
READ_BLOCK_SIZE=$((4 * 1024 * 1024))

EXPECTED_FILES=$NUM_THREADS
EXPECTED_BYTES=$((EXPECTED_FILES * FILE_SIZE))

test_init
tap_plan 19

DATA_DIR="$TEST_DIR/data"
LIVECSV="$TEST_DIR/write.livecsv"

mkdir -p "$DATA_DIR"

################## Write files ##################

# "-n 0" means no subdirs, so all threads write their files into the given dir.
run_elbencho write \
    -w \
    -t $NUM_THREADS -n 0 -N 1 -s $FILE_SIZE -b $WRITE_BLOCK_SIZE \
    --verify 1 --blockvarpct 0 \
    --livecsv "$LIVECSV" --liveint 200 \
    "$DATA_DIR"
assert_ok $? "write phase of $EXPECTED_FILES large files succeeds"

assert_eq "$(count_files "$DATA_DIR")" "$EXPECTED_FILES" \
    "$EXPECTED_FILES files exist on the file system"
assert_eq "$(count_dirs "$DATA_DIR")" "0" \
    "no subdirs are created when \"-n 0\" is given"
assert_eq "$(unique_file_sizes "$DATA_DIR")" "$FILE_SIZE" \
    "all created files have a size of $FILE_SIZE bytes"

assert_eq "$(json_value "$ELB_JSON" WRITE last_done entries)" "$EXPECTED_FILES" \
    "write phase reports $EXPECTED_FILES written files"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$EXPECTED_BYTES" \
    "write phase reports $EXPECTED_BYTES written bytes"
assert_gt "$(json_value "$ELB_JSON" WRITE last_done 'bytes/s')" 0 \
    "write phase reports a non-zero throughput"
assert_gt "$(json_value "$ELB_JSON" WRITE last_done iops)" 0 \
    "write phase reports non-zero IOPS"

assert_eq "$(json_config "$ELB_JSON" WRITE path_type)" "dir" \
    "write phase result reports a dir as benchmark path type"
assert_eq "$(json_config "$ELB_JSON" WRITE block_size)" "$WRITE_BLOCK_SIZE" \
    "write phase result confirms the given block size"

assert_eq "$(livecsv_header "$LIVECSV")" "$LIVECSV_HEADER" \
    "live statistics csv file has the expected header line"
assert_eq "$(livecsv_bad_rows "$LIVECSV")" "0" \
    "all rows of the live statistics csv file have the expected number of fields"

################## Read files back ##################

run_elbencho read \
    -r \
    -t $NUM_THREADS -n 0 -N 1 -s $FILE_SIZE -b $READ_BLOCK_SIZE \
    --verify 1 \
    "$DATA_DIR"
assert_ok $? "read phase with data integrity check and a larger block size succeeds"

assert_eq "$(json_value "$ELB_JSON" READ last_done entries)" "$EXPECTED_FILES" \
    "read phase reports $EXPECTED_FILES read files"
assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$EXPECTED_BYTES" \
    "read phase reports $EXPECTED_BYTES read bytes"
assert_gt "$(json_value "$ELB_JSON" READ last_done 'bytes/s')" 0 \
    "read phase reports a non-zero throughput"

################## Delete files ##################

run_elbencho delete \
    -F --nodelerr \
    -t $NUM_THREADS -n 0 -N 1 \
    "$DATA_DIR"
assert_ok $? "delete phase of files succeeds"

assert_eq "$(json_value "$ELB_JSON" RMFILES last_done entries)" "$EXPECTED_FILES" \
    "rmfiles phase reports $EXPECTED_FILES deleted files"
assert_eq "$(count_files "$DATA_DIR")" "0" \
    "no files are left after the delete phase"
