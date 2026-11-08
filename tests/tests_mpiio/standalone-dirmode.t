#!/bin/bash
#
# MPI-IO mode with one file per thread (directory mode) without mpirun.
#
# Each file is opened on MPI_COMM_SELF and written with independent MPI-IO
# calls, the dir, stat and delete phases stay POSIX. Covers "--trunctosize" and
# "--trunc" through MPI_File_set_size, "--statinline" and the rejection of
# collective I/O in this mode.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1

require_build_feature mpiio

NUM_THREADS=2
NUM_DIRS=2      # per thread
NUM_FILES=3     # per thread and dir
FILE_SIZE=1048576
BLOCK_SIZE=262144

EXPECTED_DIR_ENTRIES=$((NUM_THREADS * NUM_DIRS))
EXPECTED_DIRS=$((EXPECTED_DIR_ENTRIES + NUM_THREADS))
EXPECTED_FILES=$((EXPECTED_DIR_ENTRIES * NUM_FILES))
EXPECTED_BYTES=$((EXPECTED_FILES * FILE_SIZE))
EXPECTED_WRITES=$((EXPECTED_FILES * FILE_SIZE / BLOCK_SIZE))

test_init
tap_plan 19

DATA_DIR="$TEST_DIR/data"
mkdir -p "$DATA_DIR"

################## Create dirs and write files ##################

run_elbencho write \
    --mpiio -d -w -t $NUM_THREADS -n $NUM_DIRS -N $NUM_FILES -s $FILE_SIZE -b $BLOCK_SIZE \
    --trunctosize --statinline \
    --opslog "$TEST_DIR/write.opslog" \
    "$DATA_DIR"
assert_ok $? "mkdirs and write phases through MPI-IO succeed"

assert_eq "$(json_value "$ELB_JSON" MKDIRS last_done entries)" "$EXPECTED_DIR_ENTRIES" \
    "mkdirs phase reports $EXPECTED_DIR_ENTRIES created dirs"
# ("--statinline" marks the phase as "WRITE+s" in the result files.)
assert_eq "$(json_value "$ELB_JSON" "WRITE+s" last_done entries)" "$EXPECTED_FILES" \
    "write phase reports $EXPECTED_FILES written files"
assert_eq "$(json_value "$ELB_JSON" "WRITE+s" last_done bytes)" "$EXPECTED_BYTES" \
    "write phase reports $EXPECTED_BYTES written bytes"

assert_eq "$(count_files "$DATA_DIR")" "$EXPECTED_FILES" \
    "$EXPECTED_FILES files exist on the file system"
assert_eq "$(count_dirs "$DATA_DIR")" "$EXPECTED_DIRS" \
    "$EXPECTED_DIRS dirs exist on the file system"
assert_eq "$(unique_file_sizes "$DATA_DIR")" "$FILE_SIZE" \
    "all files have the expected size"

assert_eq "$(opslog_count "$TEST_DIR/write.opslog" MPI_File_open)" "$EXPECTED_FILES" \
    "each file was opened through MPI_File_open"
assert_eq "$(opslog_count "$TEST_DIR/write.opslog" MPI_File_write_at)" "$EXPECTED_WRITES" \
    "one MPI_File_write_at call per block was logged"
assert_eq "$(opslog_count "$TEST_DIR/write.opslog" stat)" "$EXPECTED_FILES" \
    "the inline stat was done for each file"
assert_eq "$(opslog_error_count "$TEST_DIR/write.opslog")" 0 "no operation failed"

################## Read files ##################

run_elbencho read \
    --mpiio -r -t $NUM_THREADS -n $NUM_DIRS -N $NUM_FILES -s $FILE_SIZE -b $BLOCK_SIZE \
    "$DATA_DIR"
assert_ok $? "read phase through MPI-IO succeeds"

assert_eq "$(json_value "$ELB_JSON" READ last_done entries)" "$EXPECTED_FILES" \
    "read phase reports $EXPECTED_FILES read files"
assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$EXPECTED_BYTES" \
    "read phase reports $EXPECTED_BYTES read bytes"

################## Rewrite with truncation ##################

run_elbencho rewrite \
    --mpiio -w --trunc -t $NUM_THREADS -n $NUM_DIRS -N $NUM_FILES \
    -s $((FILE_SIZE / 2)) -b $BLOCK_SIZE \
    "$DATA_DIR"
assert_ok $? "rewrite with \"--trunc\" succeeds"

assert_eq "$(unique_file_sizes "$DATA_DIR")" "$((FILE_SIZE / 2))" \
    "\"--trunc\" truncated the existing files before the smaller rewrite"

################## Delete ##################

run_elbencho delete \
    --mpiio -F -D -t $NUM_THREADS -n $NUM_DIRS -N $NUM_FILES \
    "$DATA_DIR"
assert_ok $? "delete phases succeed"

assert_eq "$(count_files "$DATA_DIR")" "0" "no files are left after the delete phase"

################## Collective I/O needs a shared file ##################

run_elbencho collreject \
    --mpiio --mpicoll -w -t 1 -n 1 -N 1 -s $FILE_SIZE -b $BLOCK_SIZE \
    "$DATA_DIR"
assert_nok $? "collective I/O is rejected in directory mode"
