#!/bin/bash
#
# Error handling of MPI-IO mode under mpirun.
#
# Whatever fails, every rank has to end: a path that the service ranks cannot
# open (reported through the coordinator), an argument that the coordinator
# rank rejects after the service ranks already started, and "--hosts" given
# under mpirun, where the ranks are assigned automatically.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$(dirname "$(readlink -f "$0")")/../lib/mpiio.sh" || exit 1

require_mpirun

NUM_RANKS=3

test_init
tap_plan 9

FILE="$TEST_DIR/file"

################## Path that the service ranks cannot open ##################

run_mpi_elbencho badpath $NUM_RANKS \
    --mpiio -w -t 1 -b 1m -s 4m \
    "$TEST_DIR/nonexistent-dir/file"
assert_nok $? "a path that the service ranks cannot open fails the run"
assert_match "$(cat "$ELB_OUT")" "Unable to open benchmark path" \
    "the services' open error is reported by the coordinator"
assert_eq "$(mpiio_procs_left)" 0 "no elbencho process is left after the failed run"

################## Argument rejected by the coordinator rank ##################

run_mpi_elbencho collmix $NUM_RANKS \
    --mpiio --mpicoll --rwmixpct 50 -w -t 1 -b 1m -s 4m \
    "$FILE"
assert_nok $? "an invalid option combination fails the run"
assert_match "$(cat "$ELB_OUT")" "read/write mix" "the rejection message is shown"
assert_eq "$(mpiio_procs_left)" 0 "the service ranks ended with the coordinator rank"

################## "--hosts" under mpirun ##################

run_mpi_elbencho hosts $NUM_RANKS \
    --mpiio --hosts localhost -w -t 1 -b 1m -s 4m \
    "$FILE"
assert_nok $? "\"--hosts\" is rejected under mpirun"
assert_match "$(cat "$ELB_OUT")" "cannot be given under mpirun" \
    "the rejection explains that the ranks are assigned automatically"
assert_eq "$(mpiio_procs_left)" 0 "no elbencho process is left after the rejection"
