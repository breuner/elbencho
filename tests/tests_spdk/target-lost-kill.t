#!/bin/bash
#
# The NVMe-oF target disappears while elbencho is running.
#
# The target process gets killed hard in the middle of a benchmark, so the
# connections drop without any notice at the protocol level. elbencho has to
# notice this, report an error and terminate - not crash with a backtrace, not
# die from an unhandled exception and not hang forever.
#
# Both IO engines are covered, because the synchronous and the asynchronous path
# wait for completions in completely different ways, and a write phase as well
# as a read phase, because they report the failure through different paths.
#
# The runtime is stretched with "--limitwrite"/"--limitread" rather than with
# "--infloop", so that the run has a defined end even if nothing breaks. A
# "--timelimit" is deliberately not used: exceeding it is a successful exit in
# elbencho, which would make a hang look like a pass.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/spdk.sh" || exit 1

require_spdk

test_init
tap_plan 18

NS_IDX=2
RATE=8m
GRACE_SECS=30
MAX_SECS=20

# Run a scenario: start a fresh target, start elbencho, wait until the target
# really sees IO, then kill the target and check how elbencho reacts.
run_kill_scenario()
{
    local what="$1"
    shift

    start_nvmf_tgt
    if [ $? -ne 0 ]; then
        tap_fail "a target is available for the \"$what\" scenario"
        tap_fail "elbencho terminates by itself after $what"
        tap_fail "elbencho reports a failure exit code after $what"
        tap_fail "elbencho reports an error message after $what"
        tap_fail "elbencho does not generate a backtrace after $what"
        tap_fail "elbencho terminates within ${MAX_SECS}s after $what"
        return
    fi

    tap_ok "a target is available for the \"$what\" scenario"

    spdk_start_background_elbencho "${what// /-}" \
        "${SPDK_OPTS[@]}" "$@" "$(spdk_ns_uuid $NS_IDX)"

    # Make sure the kill lands while IO is really in flight, rather than during
    # startup. This asks the target itself, so it cannot be fooled by elbencho
    # buffering its output.
    if ! spdk_wait_for_target_io "$(spdk_bdev_name $NS_IDX)" 30; then
        tap_diag "elbencho never sent any IO to the target. Output:"
        tap_diag_file "$ELB_BG_OUT"
        spdk_kill_background_elbencho
        stop_nvmf_tgt
        tap_fail "elbencho terminates by itself after $what"
        tap_fail "elbencho reports a failure exit code after $what"
        tap_fail "elbencho reports an error message after $what"
        tap_fail "elbencho does not generate a backtrace after $what"
        tap_fail "elbencho terminates within ${MAX_SECS}s after $what"
        return
    fi

    sleep 1 # let some steady state IO accumulate

    kill_nvmf_tgt

    spdk_wait_for_own_exit $GRACE_SECS
    spdk_assert_clean_termination "$what" $MAX_SECS
}

################## Asynchronous write ##################

run_kill_scenario "a killed target during an asynchronous write" \
    -w -t 1 --iodepth 4 -b 65536 -s "$(spdk_ns_size $NS_IDX)" --limitwrite $RATE

################## Synchronous write ##################

# The synchronous path waits for completions differently from the asynchronous
# one, so it needs its own scenario.
run_kill_scenario "a killed target during a synchronous write" \
    -w -t 1 --iodepth 1 -b 65536 -s "$(spdk_ns_size $NS_IDX)" --limitwrite $RATE

################## Asynchronous read ##################

run_kill_scenario "a killed target during a read" \
    -r -t 1 --iodepth 4 -b 65536 -s "$(spdk_ns_size $NS_IDX)" --limitread $RATE
