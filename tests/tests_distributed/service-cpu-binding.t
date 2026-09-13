#!/bin/bash
#
# CPU core and NUMA zone binding of a service instance.
#
# Neither the bound cores nor the bound NUMA zones appear in any result file, so
# the effective CPU affinity of the service process is what shows whether the
# option had an effect.
#
# The options are given in the abbreviated form "--core" and "--zone" that the
# examples in tools/test-examples.sh use. Those work because the argument parser
# accepts unambiguous prefixes of "--cores" and "--zones", so this is also the
# regression check for that abbreviation staying unambiguous.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1

require_build_feature corebind

# Pinning to the first core this test is allowed to use instead of to core 0,
# so that the check also works when the test runner itself is restricted to a
# set of cores that does not contain core 0.
BIND_CPU="$(first_allowed_cpu)"

NUM_NUMA_NODES=$(ls -d /sys/devices/system/node/node* 2>/dev/null | wc -l)
BAD_ZONE=$((NUM_NUMA_NODES + 100))
BAD_CPU=99999

SERVICE_PORT=""
SERVICE_PID=""

# start_bound_service ARGS...
# Starts a service in the foreground with the given binding arguments, so that
# the test keeps a pid whose CPU affinity can be inspected.
start_bound_service()
{
    local tries=0

    while [ $tries -lt 5 ]; do
        tries=$((tries+1))

        SERVICE_PORT=$(find_free_port)
        if [ -z "$SERVICE_PORT" ]; then
            return 1
        fi

        "$ELBENCHO_TEST_BIN" --service --foreground --port "$SERVICE_PORT" "$@" \
            > "$TEST_DIR/service.log" 2>&1 &

        SERVICE_PID=$!
        register_pid "$SERVICE_PID"

        trace_cmd "service start on port $SERVICE_PORT (attempt $tries)" \
            "$ELBENCHO_TEST_BIN" --service --foreground --port "$SERVICE_PORT" "$@"
        trace_add_log "$TEST_DIR/service.log"

        if wait_for_port 127.0.0.1 "$SERVICE_PORT" 10; then
            return 0
        fi

        tap_diag "Service did not come up on port $SERVICE_PORT, retrying..."
        kill -KILL "$SERVICE_PID" >/dev/null 2>&1
        wait "$SERVICE_PID" 2>/dev/null
    done

    return 1
}

stop_bound_service()
{
    run_elbencho "$1" --hosts "localhost:$SERVICE_PORT" --quit >/dev/null 2>&1
    wait_for_port_gone 127.0.0.1 "$SERVICE_PORT" 10
}

test_init
tap_plan 12

DATA_DIR="$TEST_DIR/data"

mkdir -p "$DATA_DIR"

################## Service bound to a single CPU core ##################

start_bound_service --core "$BIND_CPU"
if [ $? -ne 0 ]; then
    tap_diag_file "$TEST_DIR/service.log"
    tap_bail "Unable to start a service instance bound to CPU core $BIND_CPU."
fi

tap_ok "service bound to CPU core $BIND_CPU is listening on port $SERVICE_PORT"

assert_eq "$(cpus_allowed_list "$SERVICE_PID")" "$BIND_CPU" \
    "the service process is allowed to run on CPU core $BIND_CPU only"

run_elbencho corebench \
    --hosts "localhost:$SERVICE_PORT" \
    -d -w -F -D --nodelerr \
    -t 2 -n 1 -N 2 -s 4096 -b 4096 \
    "$DATA_DIR"
assert_ok $? "a benchmark through the core bound service succeeds"

stop_bound_service corequit
assert_ok $? "the core bound service terminates on --quit"

################## Service bound to a NUMA zone ##################

if has_build_feature libnuma; then

    start_bound_service --zone 0
    if [ $? -ne 0 ]; then
        tap_diag_file "$TEST_DIR/service.log"
        tap_bail "Unable to start a service instance bound to NUMA zone 0."
    fi

    tap_ok "service bound to NUMA zone 0 is listening on port $SERVICE_PORT"

    # Only a weak check on purpose: on a host with a single NUMA zone the
    # resulting affinity mask is the same as without any binding.
    assert_ne "$(cpus_allowed_list "$SERVICE_PID")" "" \
        "the NUMA zone bound service process has a CPU affinity mask"

    run_elbencho zonebench \
        --hosts "localhost:$SERVICE_PORT" \
        -d -w -F -D --nodelerr \
        -t 2 -n 1 -N 2 -s 4096 -b 4096 \
        "$DATA_DIR"
    assert_ok $? "a benchmark through the NUMA zone bound service succeeds"

    stop_bound_service zonequit
    assert_ok $? "the NUMA zone bound service terminates on --quit"

else
    tap_skip "service bound to NUMA zone 0 is listening"
    tap_skip "the NUMA zone bound service process has a CPU affinity mask"
    tap_skip "a benchmark through the NUMA zone bound service succeeds"
    tap_skip "the NUMA zone bound service terminates on --quit"
fi

################## Values outside of the available range ##################

# The bindings are applied during argument checking, so no service is needed to
# see them fail.
run_elbencho badcore -w -t 1 -s 4096 -b 4096 --cores $BAD_CPU "$TEST_DIR/badcore"
assert_nok $? "a run with CPU core $BAD_CPU does not start"

assert_match "$(cat "$ELB_OUT")" 'Applying CPU core set failed' \
    "the unavailable CPU core is reported as an error"

if has_build_feature libnuma; then

    run_elbencho badzone -w -t 1 -s 4096 -b 4096 --zones $BAD_ZONE "$TEST_DIR/badzone"
    assert_nok $? "a run with NUMA zone $BAD_ZONE does not start"

    assert_match "$(cat "$ELB_OUT")" "Parsing of NUMA zone string failed: $BAD_ZONE" \
        "the unavailable NUMA zone is reported as an error"

else
    tap_skip "a run with an unavailable NUMA zone does not start"
    tap_skip "the unavailable NUMA zone is reported as an error"
fi
