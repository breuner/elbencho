#!/bin/bash
#
# Time limit in collective MPI-IO mode under mpirun.
#
# Every thread of a collective group has to make the same number of calls, so
# a thread that stops at the time limit completes its planned call count with
# empty collective calls instead of leaving its group waiting. The write is
# rate limited so that it cannot finish within the time limit, and the test
# only passes if the run ends by itself: a deadlock would hit the command
# timeout. The collective run uses one thread per rank (four I/O ranks), see
# mpirun-filemode.t for why; the independent variant is covered for comparison.
# With "--infloop" the group decides together after every repeat whether to
# start another one, so a small file is repeated until the time limit and the
# run has to end with more bytes than one pass over the file.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$(dirname "$(readlink -f "$0")")/../lib/mpiio.sh" || exit 1

require_mpirun

NUM_RANKS=3     # independent: coordinator plus two I/O ranks with two threads
NUM_THREADS=2
COLL_RANKS=5    # collective: coordinator plus four I/O ranks with one thread
COLL_THREADS=1
BLOCK_SIZE=1048576
FILE_SIZE=$((2 * 1024 * 1048576))
LIMIT_PER_THREAD="25m"  # 4 threads x 25 MiB/s makes the 2 GiB take about 20 seconds
TIME_LIMIT=2
LOOP_FILE_SIZE=$((32 * 1048576))  # infinite loop: repeated many times within the time limit

test_init
tap_plan 10

FILE="$TEST_DIR/file"

################## Collective write cut by the time limit ##################

run_mpi_elbencho coll $COLL_RANKS \
    --mpiio --mpicoll -w -t $COLL_THREADS -b $BLOCK_SIZE -s $FILE_SIZE \
    --limitwrite $LIMIT_PER_THREAD --timelimit $TIME_LIMIT \
    "$FILE"
assert_ok $? "collective write phase ends with the time limit instead of deadlocking"

BYTES=$(json_value "$ELB_JSON" WRITE last_done bytes)
assert_gt "$BYTES" 0 "some data was written before the time limit"
assert_lt "$BYTES" "$FILE_SIZE" "less than the full file was written"
assert_eq "$(mpiio_procs_left)" 0 "no elbencho process is left after the collective run"

################## Independent write cut by the time limit ##################

run_mpi_elbencho indep $NUM_RANKS \
    --mpiio -w -t $NUM_THREADS -b $BLOCK_SIZE -s $FILE_SIZE \
    --limitwrite $LIMIT_PER_THREAD --timelimit $TIME_LIMIT \
    "$FILE"
assert_ok $? "independent write phase ends with the time limit"

BYTES=$(json_value "$ELB_JSON" WRITE last_done bytes)
assert_lt "$BYTES" "$FILE_SIZE" "less than the full file was written independently"
assert_eq "$(mpiio_procs_left)" 0 "no elbencho process is left after the independent run"

################## Collective infinite loop cut by the time limit ##################

run_mpi_elbencho coll-infloop $COLL_RANKS \
    --mpiio --mpicoll --strided --infloop -w -t $COLL_THREADS -b $BLOCK_SIZE -s $LOOP_FILE_SIZE \
    --timelimit $TIME_LIMIT \
    "$FILE"
assert_ok $? "collective infinite loop ends with the time limit instead of deadlocking"

BYTES=$(json_value "$ELB_JSON" WRITE last_done bytes)
assert_gt "$BYTES" "$LOOP_FILE_SIZE" "the file was written more than once"
assert_eq "$(mpiio_procs_left)" 0 "no elbencho process is left after the infinite loop run"
