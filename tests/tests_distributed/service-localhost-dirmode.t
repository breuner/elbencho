#!/bin/bash
#
# Distributed mode against a local elbencho service instance.
#
# Starts an elbencho service on a random free TCP port and runs a small
# directory mode benchmark through it, i.e. the coordinator talks to the service
# over its http interface and the service does the actual IO. Verifies the
# resulting tree, the counters reported back to the coordinator and that the
# service terminates on "--quit".

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1

NUM_THREADS=2
NUM_DIRS=2      # per thread
NUM_FILES=5     # per thread and dir
FILE_SIZE=16384

# In distributed mode threads, dirs and files are per service, and there is one
# service in this test.
EXPECTED_FILES=$((NUM_THREADS * NUM_DIRS * NUM_FILES))
EXPECTED_BYTES=$((EXPECTED_FILES * FILE_SIZE))
EXPECTED_DIR_ENTRIES=$((NUM_THREADS * NUM_DIRS))
EXPECTED_DIRS=$((EXPECTED_DIR_ENTRIES + NUM_THREADS))

SERVICE_PORT=""
SERVICE_PID=""

# Start the service on a random free port. The port could have been taken
# between the free port check and the actual bind, so this retries a few times.
# Output must go to a file, because prove reads the test's stdout until EOF.
start_service()
{
    local tries=0

    while [ $tries -lt 5 ]; do
        tries=$((tries+1))

        SERVICE_PORT=$(find_free_port)
        if [ -z "$SERVICE_PORT" ]; then
            return 1
        fi

        # "--foreground" prevents the service from daemonizing, so that the test
        # keeps a pid to clean up.
        "$ELBENCHO_TEST_BIN" --service --foreground --port "$SERVICE_PORT" \
            > "$TEST_DIR/service.log" 2>&1 &

        SERVICE_PID=$!
        register_pid "$SERVICE_PID"

        trace_cmd "service start on port $SERVICE_PORT (attempt $tries)" \
            "$ELBENCHO_TEST_BIN" --service --foreground --port "$SERVICE_PORT"
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

test_init
tap_plan 16

DATA_DIR="$TEST_DIR/data"

mkdir -p "$DATA_DIR"

################## Start the service ##################

start_service
if [ $? -ne 0 ]; then
    tap_diag "Service log:"
    tap_diag_file "$TEST_DIR/service.log"
    tap_bail "Unable to start an elbencho service instance."
fi

tap_ok "elbencho service instance is listening on port $SERVICE_PORT"

################## Run a benchmark through the service ##################

run_elbencho bench \
    --hosts "localhost:$SERVICE_PORT" \
    -d -w -r \
    -t $NUM_THREADS -n $NUM_DIRS -N $NUM_FILES -s $FILE_SIZE -b $FILE_SIZE \
    --verify 1 --blockvarpct 0 \
    "$DATA_DIR"
assert_ok $? "distributed write and read phases through the service succeed"

assert_eq "$(json_config "$ELB_JSON" WRITE hosts)" "1" \
    "write phase result reports a single service host"

assert_eq "$(json_value "$ELB_JSON" MKDIRS last_done entries)" "$EXPECTED_DIR_ENTRIES" \
    "mkdirs phase reports $EXPECTED_DIR_ENTRIES created dirs"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done entries)" "$EXPECTED_FILES" \
    "write phase reports $EXPECTED_FILES written files"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$EXPECTED_BYTES" \
    "write phase reports $EXPECTED_BYTES written bytes"
assert_eq "$(json_value "$ELB_JSON" READ last_done entries)" "$EXPECTED_FILES" \
    "read phase reports $EXPECTED_FILES read files"
assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$EXPECTED_BYTES" \
    "read phase reports $EXPECTED_BYTES read bytes"
assert_gt "$(json_value "$ELB_JSON" WRITE last_done 'bytes/s')" 0 \
    "write phase reports a non-zero throughput"

assert_eq "$(count_files "$DATA_DIR")" "$EXPECTED_FILES" \
    "$EXPECTED_FILES files exist on the file system"
assert_eq "$(count_dirs "$DATA_DIR")" "$EXPECTED_DIRS" \
    "$EXPECTED_DIRS dirs exist on the file system"

################## Clean up the dataset through the service ##################

run_elbencho delete \
    --hosts "localhost:$SERVICE_PORT" \
    -F -D --nodelerr \
    -t $NUM_THREADS -n $NUM_DIRS -N $NUM_FILES \
    "$DATA_DIR"
assert_ok $? "distributed delete phase through the service succeeds"

assert_eq "$(count_files "$DATA_DIR")" "0" \
    "no files are left after the delete phase"
assert_eq "$(count_dirs "$DATA_DIR")" "0" \
    "no dirs are left after the delete phase"

################## Terminate the service ##################

run_elbencho quit \
    --hosts "localhost:$SERVICE_PORT" \
    --quit
assert_ok $? "\"--quit\" is accepted by the service"

if wait_for_port_gone 127.0.0.1 "$SERVICE_PORT" 10; then
    tap_ok "service stopped listening on port $SERVICE_PORT after \"--quit\""
else
    tap_fail "service stopped listening on port $SERVICE_PORT after \"--quit\""
fi
