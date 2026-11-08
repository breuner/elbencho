#!/bin/bash
#
# Helper library for the MPI-IO mode tests.
#
# Source this after tests/lib/testlib.sh. require_mpirun() has to be called
# before test_init(), because it may skip the whole test file.
#
# Under mpirun, rank 0 is the coordinator and writes the result files, all other
# ranks are elbencho service instances that do the I/O. Every rank gets the same
# command line, so an "--opslog" path is written by the service ranks (where the
# I/O happens), which on a single host means that all of them append to the same
# file.

MPIRUN=""
MPIRUN_FLAGS=""

# Skip the whole test file unless elbencho was built with MPI support and an
# mpirun is available. Open MPI refuses to start more ranks than there are cores
# without "--oversubscribe", which MPICH's hydra does not know.
require_mpirun()
{
    require_build_feature mpiio

    if command -v mpirun >/dev/null 2>&1; then
        MPIRUN="mpirun"
    elif command -v mpiexec >/dev/null 2>&1; then
        MPIRUN="mpiexec"
    else
        tap_skip_all "no mpirun/mpiexec found in PATH"
    fi

    if "$MPIRUN" --version 2>&1 | grep -q "Open MPI"; then
        MPIRUN_FLAGS="--oversubscribe"
    fi
}

# run_mpi_elbencho TAG NUM_RANKS [ELBENCHO ARGS...]
#
# Like run_elbencho, but through mpirun with the given number of ranks on this
# host: one coordinator rank plus NUM_RANKS-1 I/O ranks. The result files are
# written by the coordinator rank and exported like run_elbencho does.
run_mpi_elbencho()
{
    local tag="$1"
    local np="$2"
    local filename="$(tag_to_filename "$1")"
    shift 2

    ELB_JSON="$TEST_DIR/$filename.json"
    ELB_CSV="$TEST_DIR/$filename.csv"
    ELB_RES="$TEST_DIR/$filename.txt"
    ELB_OUT="$TEST_DIR/$filename.out"

    local started="$(trace_now_ms)"

    trace_cmd "mpirun elbencho \"$tag\"" \
        "$MPIRUN" $MPIRUN_FLAGS -np "$np" "$ELBENCHO_TEST_BIN" \
        --nolive --no0usecerr \
        --resfile "$ELB_RES" --csvfile "$ELB_CSV" --jsonfile "$ELB_JSON" \
        "$@"

    timeout --kill-after=5s --signal=TERM "$ELBENCHO_TEST_CMD_TIMEOUT" \
        "$MPIRUN" $MPIRUN_FLAGS -np "$np" "$ELBENCHO_TEST_BIN" \
        --nolive --no0usecerr \
        --resfile "$ELB_RES" --csvfile "$ELB_CSV" --jsonfile "$ELB_JSON" \
        "$@" > "$ELB_OUT" 2>&1

    local rc=$?

    trace_result "mpirun elbencho \"$tag\"" "$rc" \
        "$(( $(trace_now_ms) - started ))" "$ELB_OUT"

    if [ $rc -eq 124 ] || [ $rc -eq 137 ]; then
        tap_diag "TIMEOUT: mpirun elbencho \"$tag\" exceeded ${ELBENCHO_TEST_CMD_TIMEOUT}s and was killed."
    fi

    if [ $rc -ne 0 ]; then
        tap_diag "Output of failed mpirun elbencho \"$tag\":"
        tap_diag_file "$ELB_OUT"
    fi

    return $rc
}

# mpiio_procs_left -> number of elbencho processes that still have this test's
# dir on their command line. All ranks of a finished mpirun have to be gone.
mpiio_procs_left()
{
    ps -eo comm,args | awk '$1 ~ /^elbencho/' | grep -cF "$TEST_DIR"
}

# opslog_rank_min_offset OPSLOGFILE OP_NAME WORKER_RANK
# Lowest offset that the given worker rank accessed with the given operation,
# e.g. to verify an interleaved ("--strided") block layout.
opslog_rank_min_offset()
{
    jq -s --arg op "$2" --argjson rank "$3" \
        '[ .[] | select(.op_name == $op and .worker_rank == $rank and .is_finished == false)
           | .offset ] | min // 0' "$1" 2>/dev/null
}
