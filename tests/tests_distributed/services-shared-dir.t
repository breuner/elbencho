#!/bin/bash
#
# Two service instances working on the same shared directory.
#
# The two services are given to the coordinator as a port range in brackets,
# which is the second thing this test covers. Because the benchmark path is
# shared between the services, the coordinator assigns each service an offset
# for its worker thread ranks, so the services must not collide in the shared
# dir even though they create their trees at the same time. Threads, dirs and
# files are per service, so the expected totals are multiplied by the number of
# services.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1

NUM_SERVICES=2
NUM_THREADS=4   # per service
NUM_DIRS=2      # per thread
NUM_FILES=3     # per thread and dir
FILE_SIZE=4096

EXPECTED_DIR_ENTRIES=$((NUM_SERVICES * NUM_THREADS * NUM_DIRS))
EXPECTED_FILES=$((EXPECTED_DIR_ENTRIES * NUM_FILES))
EXPECTED_BYTES=$((EXPECTED_FILES * FILE_SIZE))

# Each worker thread also gets its own rank dir as the parent of its numbered
# dirs, and those are not part of the reported dirs counter.
EXPECTED_DIRS=$((EXPECTED_DIR_ENTRIES + NUM_SERVICES * NUM_THREADS))

PORT1=""
PORT2=""
HOSTS=""
SERVICE_PIDS=""

# Start both services on two consecutive free ports, so that they can be given
# to the coordinator as a port range. Retries the whole sequence, because a port
# can be taken between the free port check and the actual bind.
start_services()
{
    local tries=0
    local pair
    local port
    local pid
    local allup

    while [ $tries -lt 5 ]; do
        tries=$((tries+1))

        pair=$(find_free_port_pair)
        if [ -z "$pair" ]; then
            return 1
        fi

        PORT1=${pair% *}
        PORT2=${pair#* }
        SERVICE_PIDS=""

        for port in "$PORT1" "$PORT2"; do
            "$ELBENCHO_TEST_BIN" --service --foreground --port "$port" \
                > "$TEST_DIR/service-$port.log" 2>&1 &

            pid=$!
            SERVICE_PIDS="$SERVICE_PIDS $pid"
            register_pid "$pid"

            trace_cmd "service start on port $port (attempt $tries)" \
                "$ELBENCHO_TEST_BIN" --service --foreground --port "$port"
            trace_add_log "$TEST_DIR/service-$port.log"
        done

        allup=1
        for port in "$PORT1" "$PORT2"; do
            if ! wait_for_port 127.0.0.1 "$port" 10; then
                allup=0
            fi
        done

        if [ $allup -eq 1 ]; then
            HOSTS="localhost:[$PORT1-$PORT2]"
            return 0
        fi

        tap_diag "Services did not come up on ports $PORT1 and $PORT2, retrying..."
        for pid in $SERVICE_PIDS; do
            kill -KILL "$pid" >/dev/null 2>&1
            wait "$pid" 2>/dev/null
        done
    done

    return 1
}

test_init
tap_plan 27

DATA_DIR="$TEST_DIR/data"

mkdir -p "$DATA_DIR"

################## Start both services ##################

start_services
if [ $? -ne 0 ]; then
    tap_bail "Unable to start two elbencho service instances on consecutive ports."
fi

tap_ok "first service instance is listening on port $PORT1"
tap_ok "second service instance is listening on port $PORT2"

################## Write and read through both services ##################

run_elbencho bench \
    --hosts "$HOSTS" \
    -d -w -r \
    -t $NUM_THREADS -n $NUM_DIRS -N $NUM_FILES -s $FILE_SIZE -b $FILE_SIZE \
    --verify 1 --blockvarpct 0 \
    "$DATA_DIR"
assert_ok $? "a benchmark through both services succeeds"

assert_eq "$(json_config "$ELB_JSON" WRITE hosts)" "$NUM_SERVICES" \
    "the port range in the host list expanded to $NUM_SERVICES hosts"
assert_eq "$(json_config "$ELB_JSON" WRITE threads)" "$NUM_THREADS" \
    "write phase result reports the $NUM_THREADS threads per service"
assert_eq "$(json_config "$ELB_JSON" WRITE dirs)" "$NUM_DIRS" \
    "write phase result reports the $NUM_DIRS dirs per thread"
assert_eq "$(json_config "$ELB_JSON" WRITE files)" "$NUM_FILES" \
    "write phase result reports the $NUM_FILES files per dir"
assert_eq "$(json_config "$ELB_JSON" WRITE shared_service_paths)" "true" \
    "the benchmark path is reported as shared between the services"

assert_eq "$(json_value "$ELB_JSON" MKDIRS last_done entries)" "$EXPECTED_DIR_ENTRIES" \
    "mkdirs phase reports $EXPECTED_DIR_ENTRIES created dirs for both services"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done entries)" "$EXPECTED_FILES" \
    "write phase reports $EXPECTED_FILES written files for both services"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$EXPECTED_BYTES" \
    "write phase reports $EXPECTED_BYTES written bytes"
assert_eq "$(json_value "$ELB_JSON" READ last_done entries)" "$EXPECTED_FILES" \
    "read phase reports $EXPECTED_FILES read files"
assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$EXPECTED_BYTES" \
    "read phase reports $EXPECTED_BYTES read bytes"

# This is what shows that the rank offset of the second service really keeps the
# two services apart: without it they would write the same names.
assert_eq "$(count_files "$DATA_DIR")" "$EXPECTED_FILES" \
    "$EXPECTED_FILES files exist on the file system, i.e. the services did not collide"
assert_eq "$(count_dirs "$DATA_DIR")" "$EXPECTED_DIRS" \
    "$EXPECTED_DIRS dirs exist on the file system"

################## Delete through both services ##################

# Without "--nodelerr", so that a name written by neither service would surface.
run_elbencho delete \
    --hosts "$HOSTS" \
    -F -D \
    -t $NUM_THREADS -n $NUM_DIRS -N $NUM_FILES \
    "$DATA_DIR"
assert_ok $? "the delete phase through both services succeeds"

assert_eq "$(json_value "$ELB_JSON" RMFILES last_done entries)" "$EXPECTED_FILES" \
    "rmfiles phase reports $EXPECTED_FILES deleted files"
assert_eq "$(json_value "$ELB_JSON" RMDIRS last_done entries)" "$EXPECTED_DIR_ENTRIES" \
    "rmdirs phase reports $EXPECTED_DIR_ENTRIES deleted dirs"
assert_eq "$(count_files "$DATA_DIR")" "0" \
    "no file is left on the file system"
assert_eq "$(count_dirs "$DATA_DIR")" "0" \
    "no dir is left on the file system"

################## All phases in a single invocation ##################

run_elbencho allphases \
    --hosts "$HOSTS" \
    -d -w -r -F -D \
    -t $NUM_THREADS -n $NUM_DIRS -N $NUM_FILES -s $FILE_SIZE -b $FILE_SIZE \
    --verify 1 --blockvarpct 0 \
    "$DATA_DIR"
assert_ok $? "all phases in a single invocation through both services succeed"

assert_eq "$(json_phases "$ELB_JSON")" "MKDIRS WRITE READ RMFILES RMDIRS" \
    "the single invocation ran all five phases in the expected order"
assert_eq "$(count_files "$DATA_DIR")" "0" \
    "the single invocation left no file behind"

################## A duplicate host is rejected ##################

run_elbencho duplicate --hosts "localhost:$PORT1,localhost:$PORT1" --quit
assert_nok $? "a host list with a duplicate entry is rejected"

assert_match "$(cat "$ELB_OUT")" 'List of hosts contains duplicates' \
    "the duplicate host is reported as an error"

################## Terminate both services ##################

run_elbencho quit --hosts "$HOSTS" --quit
assert_ok $? "--quit through the port range terminates both services"

wait_for_port_gone 127.0.0.1 "$PORT1" 10 && wait_for_port_gone 127.0.0.1 "$PORT2" 10
assert_ok $? "neither service port is listening anymore"
