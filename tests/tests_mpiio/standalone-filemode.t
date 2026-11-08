#!/bin/bash
#
# MPI-IO mode on a shared file without mpirun (a single I/O rank).
#
# Covers independent sequential, random and strided access, collective I/O with
# a single rank (where the threads of the rank form the collective groups and
# the file size gets reduced to a multiple of block size times thread count),
# data verification through the write-then-read wrapper, and the argument
# checks of the mode. The operations log shows which MPI-IO calls were made.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1

require_build_feature mpiio

NUM_THREADS=4
BLOCK_SIZE=1048576
FILE_SIZE=$((64 * 1048576))
NUM_BLOCKS=$((FILE_SIZE / BLOCK_SIZE))

test_init
tap_plan 30

FILE="$TEST_DIR/file"

################## Independent sequential write and read ##################

run_elbencho seq \
    --mpiio -w -r -t $NUM_THREADS -b $BLOCK_SIZE -s $FILE_SIZE \
    --opslog "$TEST_DIR/seq.opslog" \
    "$FILE"
assert_ok $? "independent sequential write and read through MPI-IO succeed"

assert_eq "$(file_size "$FILE")" "$FILE_SIZE" "the shared file has the expected size"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$FILE_SIZE" \
    "write phase reports the full file size"
assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$FILE_SIZE" \
    "read phase reports the full file size"
assert_gt "$(json_value "$ELB_JSON" WRITE last_done 'bytes/s')" 0 \
    "write phase reports a non-zero throughput"

assert_eq "$(opslog_count "$TEST_DIR/seq.opslog" MPI_File_open)" "$NUM_THREADS" \
    "each thread opened the shared file once for the run"
assert_eq "$(opslog_count "$TEST_DIR/seq.opslog" MPI_File_write_at)" "$NUM_BLOCKS" \
    "one MPI_File_write_at call per block was logged"
assert_eq "$(opslog_count "$TEST_DIR/seq.opslog" MPI_File_read_at)" "$NUM_BLOCKS" \
    "one MPI_File_read_at call per block was logged"
assert_eq "$(opslog_max_distinct_ranks "$TEST_DIR/seq.opslog" MPI_File_write_at)" \
    "$NUM_THREADS" "all threads wrote to the shared file"
assert_eq "$(opslog_error_count "$TEST_DIR/seq.opslog")" 0 "no MPI-IO call failed"

################## Independent random and strided access ##################

run_elbencho rand \
    --mpiio -w -r --rand -t $NUM_THREADS -b $BLOCK_SIZE -s $FILE_SIZE \
    "$FILE"
assert_ok $? "independent random write and read succeed"

assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$FILE_SIZE" \
    "random write phase reports the full file size"

run_elbencho strided \
    --mpiio -w -r --strided -t $NUM_THREADS -b $BLOCK_SIZE -s $FILE_SIZE \
    --opslog "$TEST_DIR/strided.opslog" \
    "$FILE"
assert_ok $? "independent strided write and read succeed"

assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$FILE_SIZE" \
    "strided read phase reports the full file size"

# Strided means interleaved blocks: thread 1 starts with the second block.
assert_eq "$(jq -s --argjson rank 1 \
    '[ .[] | select(.op_name == "MPI_File_write_at" and .worker_rank == $rank
        and .is_finished == false) | .offset ] | min' "$TEST_DIR/strided.opslog")" \
    "$BLOCK_SIZE" "strided access interleaves the blocks of the threads"

################## Collective I/O with a single rank ##################

# 31 MiB is not a multiple of 3 threads x 1 MiB, so the size gets reduced to 30 MiB.
COLL_THREADS=3
COLL_SIZE=$((31 * 1048576))
COLL_SIZE_REDUCED=$((30 * 1048576))

run_elbencho coll \
    --mpiio --mpicoll --strided -w -r -t $COLL_THREADS -b $BLOCK_SIZE -s $COLL_SIZE \
    --opslog "$TEST_DIR/coll.opslog" \
    "$FILE"
assert_ok $? "collective strided write and read succeed"

assert_match "$(cat "$ELB_OUT")" "Reducing file size" \
    "the file size reduction for equal block counts is reported"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$COLL_SIZE_REDUCED" \
    "collective write phase reports the reduced file size"
assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$COLL_SIZE_REDUCED" \
    "collective read phase reports the reduced file size"
assert_eq "$(opslog_count "$TEST_DIR/coll.opslog" MPI_File_write_at_all)" \
    "$((COLL_SIZE_REDUCED / BLOCK_SIZE))" \
    "one MPI_File_write_at_all call per block was logged"
assert_eq "$(opslog_count "$TEST_DIR/coll.opslog" MPI_File_read_at_all)" \
    "$((COLL_SIZE_REDUCED / BLOCK_SIZE))" \
    "one MPI_File_read_at_all call per block was logged"

################## Data verification ##################

run_elbencho verifywrite \
    --mpiio -w -t 2 -b $BLOCK_SIZE -s $((8 * 1048576)) --verify 7 \
    "$FILE"
assert_ok $? "write with inline verification succeeds"

run_elbencho verifyread \
    --mpiio -r -t 2 -b $BLOCK_SIZE -s $((8 * 1048576)) --verify 7 \
    "$FILE"
assert_ok $? "read with the same verification salt succeeds"

run_elbencho verifybad \
    --mpiio -r -t 2 -b $BLOCK_SIZE -s $((8 * 1048576)) --verify 8 \
    "$FILE"
assert_nok $? "read with a different verification salt fails"

################## Argument checks ##################

run_elbencho iodepth --mpiio -w -t 1 -b $BLOCK_SIZE -s $((4 * 1048576)) --iodepth 4 "$FILE"
assert_nok $? "\"--iodepth\" is rejected in MPI-IO mode"
assert_match "$(cat "$ELB_OUT")" "iodepth" "the iodepth rejection names the option"

run_elbencho collmix \
    --mpiio --mpicoll --rwmixpct 50 -w -t 1 -b $BLOCK_SIZE -s $((4 * 1048576)) "$FILE"
assert_nok $? "a read/write mix is rejected with collective I/O"

run_elbencho collmulti \
    --mpiio --mpicoll -w -t 1 -b $BLOCK_SIZE -s $((4 * 1048576)) "$FILE" "$FILE.second"
assert_nok $? "more than one file is rejected with collective I/O"
assert_match "$(cat "$ELB_OUT")" "single shared file" \
    "the multiple files rejection explains the single file requirement"

run_elbencho collwithoutmpiio --mpicoll -w -t 1 -b $BLOCK_SIZE -s $((4 * 1048576)) "$FILE"
assert_nok $? "\"--mpicoll\" without \"--mpiio\" is rejected"
