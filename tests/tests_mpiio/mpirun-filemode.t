#!/bin/bash
#
# MPI-IO mode on a shared file under mpirun with two I/O ranks on this host.
#
# Rank 0 is the coordinator, the other ranks are the service instances that do
# the I/O: two ranks with two threads each for independent I/O, four ranks with
# one thread each for collective I/O, because MPI libraries whose MPI-IO layer
# serializes the calls of a process (MPICH's ROMIO) deadlock on collective calls
# from several threads of one rank (see docs/mpiio.md; the threaded collective
# case with groups of one participant is in standalone-filemode.t). Covers
# independent and collective I/O with sequential, strided and random access,
# the file size reduction for equal block counts, an MPI-IO hint, and that no
# rank is left behind when mpirun returns.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$(dirname "$(readlink -f "$0")")/../lib/mpiio.sh" || exit 1

require_mpirun

NUM_RANKS=3     # independent I/O: coordinator plus two I/O ranks
NUM_HOSTS=2     # the I/O ranks
NUM_THREADS=2   # per I/O rank
DATASET_THREADS=$((NUM_HOSTS * NUM_THREADS))
COLL_RANKS=5    # collective I/O: coordinator plus four I/O ranks with one thread each
COLL_THREADS=1
BLOCK_SIZE=1048576
FILE_SIZE=$((64 * 1048576))
NUM_BLOCKS=$((FILE_SIZE / BLOCK_SIZE))

test_init
tap_plan 22

FILE="$TEST_DIR/file"

################## Independent sequential write and read ##################

run_mpi_elbencho seq $NUM_RANKS \
    --mpiio -w -r -t $NUM_THREADS -b $BLOCK_SIZE -s $FILE_SIZE \
    --opslog "$TEST_DIR/seq.opslog" \
    "$FILE"
assert_ok $? "independent write and read under mpirun succeed"

assert_eq "$(json_config "$ELB_JSON" WRITE hosts)" "$NUM_HOSTS" \
    "the result reports the two I/O ranks as hosts"
assert_eq "$(file_size "$FILE")" "$FILE_SIZE" "the shared file has the expected size"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$FILE_SIZE" \
    "write phase reports the full file size"
assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$FILE_SIZE" \
    "read phase reports the full file size"

assert_eq "$(opslog_count "$TEST_DIR/seq.opslog" MPI_File_write_at)" "$NUM_BLOCKS" \
    "the I/O ranks logged one MPI_File_write_at call per block"
assert_eq "$(opslog_max_distinct_ranks "$TEST_DIR/seq.opslog" MPI_File_write_at)" \
    "$DATASET_THREADS" "all threads of both I/O ranks wrote to the shared file"
assert_eq "$(mpiio_procs_left)" 0 "no elbencho process is left after mpirun returned"

################## Collective sequential I/O with file size reduction ##################

# 65 MiB is not a multiple of 4 threads x 1 MiB, so the services reduce it to 64 MiB.
run_mpi_elbencho collseq $COLL_RANKS \
    --mpiio --mpicoll -w -r -t $COLL_THREADS -b $BLOCK_SIZE -s $((65 * 1048576)) \
    --opslog "$TEST_DIR/coll.opslog" \
    "$FILE"
assert_ok $? "collective sequential write and read succeed"

assert_match "$(cat "$ELB_OUT")" "adapted file size" \
    "the coordinator reports the file size adaptation of the services"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$FILE_SIZE" \
    "collective write phase reports the reduced file size"
assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$FILE_SIZE" \
    "collective read phase reports the reduced file size"
assert_eq "$(opslog_count "$TEST_DIR/coll.opslog" MPI_File_write_at_all)" "$NUM_BLOCKS" \
    "one MPI_File_write_at_all call per block was logged"
assert_eq "$(opslog_count "$TEST_DIR/coll.opslog" MPI_File_read_at_all)" "$NUM_BLOCKS" \
    "one MPI_File_read_at_all call per block was logged"

################## Collective strided and random I/O ##################

run_mpi_elbencho collstrided $COLL_RANKS \
    --mpiio --mpicoll --strided -w -r -t $COLL_THREADS -b $BLOCK_SIZE -s $FILE_SIZE \
    --opslog "$TEST_DIR/strided.opslog" \
    "$FILE"
assert_ok $? "collective strided write and read succeed"

assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$FILE_SIZE" \
    "collective strided read phase reports the full file size"
assert_eq "$(opslog_rank_min_offset "$TEST_DIR/strided.opslog" MPI_File_write_at_all 1)" \
    "$BLOCK_SIZE" "strided access interleaves the blocks across the ranks"
assert_eq "$(opslog_count "$TEST_DIR/strided.opslog" MPI_File_write_at_all)" "$NUM_BLOCKS" \
    "strided collective write logged one call per block"

run_mpi_elbencho collrand $COLL_RANKS \
    --mpiio --mpicoll --rand -w -r -t $COLL_THREADS -b $BLOCK_SIZE -s $FILE_SIZE \
    "$FILE"
assert_ok $? "collective random write and read succeed"

assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$FILE_SIZE" \
    "collective random write phase reports the full file size"

################## MPI-IO hints ##################

run_mpi_elbencho hints $NUM_RANKS \
    --mpiio -w -t $NUM_THREADS -b $BLOCK_SIZE -s $((16 * 1048576)) \
    --mpihints "romio_cb_write=enable,romio_ds_write=disable" \
    "$FILE"
assert_ok $? "a run with MPI-IO hints succeeds"

assert_eq "$(mpiio_procs_left)" 0 "no elbencho process is left at the end"
