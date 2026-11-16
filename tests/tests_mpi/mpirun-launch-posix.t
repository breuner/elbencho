#!/bin/bash
#
# POSIX benchmark under an MPI launcher ("--mpilaunch") with two I/O ranks on
# this host.
#
# "--mpilaunch" only maps the MPI ranks to roles: rank 0 is the coordinator,
# the other ranks are the service instances that do the I/O in whatever
# benchmark mode the options select, here POSIX directory mode. Covers the
# role mapping without MPI-IO, that the result counts the I/O ranks as hosts,
# the rejection of "--netbench" under the launcher (the ranks cannot be split
# into servers and clients), a standalone run with the option but without
# mpirun, and that no rank is left behind.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$(dirname "$(readlink -f "$0")")/../lib/mpi.sh" || exit 1

require_mpirun

NUM_RANKS=3     # coordinator plus two I/O ranks
NUM_HOSTS=2     # the I/O ranks
NUM_THREADS=2   # per I/O rank
NUM_FILES=4     # per thread
FILE_SIZE=1048576
BLOCK_SIZE=1048576
TOTAL_FILES=$((NUM_HOSTS * NUM_THREADS * NUM_FILES))

test_init
tap_plan 9

################## POSIX directory mode under the launcher ##################

mkdir -p "$TEST_DIR/data"

run_mpi_elbencho posixdir $NUM_RANKS \
    --mpilaunch -d -w -r -F -D -t $NUM_THREADS -n 1 -N $NUM_FILES -s $FILE_SIZE -b $BLOCK_SIZE \
    "$TEST_DIR/data"
assert_ok $? "POSIX directory mode under mpirun with --mpilaunch succeeds"

assert_eq "$(json_config "$ELB_JSON" WRITE hosts)" "$NUM_HOSTS" \
    "the result reports the two I/O ranks as hosts"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done entries)" "$TOTAL_FILES" \
    "write phase reports the files of all threads of both I/O ranks"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$((TOTAL_FILES * FILE_SIZE))" \
    "write phase reports the bytes of all files"
assert_eq "$(json_value "$ELB_JSON" READ last_done entries)" "$TOTAL_FILES" \
    "read phase reports the files of all threads of both I/O ranks"
assert_eq "$(mpi_procs_left)" 0 "no elbencho process is left after mpirun returned"

################## Netbench is rejected under the launcher ##################

run_mpi_elbencho netbench $NUM_RANKS \
    --mpilaunch --netbench -t 1 -s $FILE_SIZE
assert_nok $? "netbench under the launcher is rejected"
assert_eq "$(mpi_procs_left)" 0 "the service ranks ended with the rejected coordinator rank"

################## Standalone run with the option but without mpirun ##################

run_elbencho standalone \
    --mpilaunch -w -t $NUM_THREADS -s $((4 * FILE_SIZE)) -b $BLOCK_SIZE \
    "$TEST_DIR/file"
assert_ok $? "--mpilaunch without mpirun is a standalone run"
