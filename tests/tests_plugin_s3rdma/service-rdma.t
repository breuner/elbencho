#!/bin/bash
#
# S3-over-RDMA via an s3rdma plugin in distributed mode.
#
# The coordinator activates a plugin on the service instances per run, so the
# service is started without it. This starts a private S3-over-RDMA test server
# and a local elbencho service, uploads and downloads objects through the service
# with the plugin active, downloads them again without the plugin against the
# same service (which has to move the data in the HTTP body again), and uploads
# them once more with the plugin. The server log tells the transports apart: it
# counts the RDMA transfers, which have to stop and resume with the plugin.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/minio.sh" || exit 1
source "$ELBENCHO_TEST_LIB/plugin_s3rdma.sh" || exit 1

NUM_THREADS=2
NUM_OBJECTS=3   # per thread
OBJ_SIZE=$((4 * 1024 * 1024))

# threads and objects are per service, and there is one service in this test
EXPECTED_OBJECTS=$((NUM_THREADS * NUM_OBJECTS))
EXPECTED_BYTES=$((EXPECTED_OBJECTS * OBJ_SIZE))

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

# rdma_ops_check PUT|GET EXPECTED DESC
#
# Checks the number of RDMA transfers in the log of the private server, or skips
# the check when the tests run against an external server.
rdma_ops_check()
{
    if s3rdma_server_is_private; then
        assert_eq "$(s3rdma_server_rdma_ops "$1")" "$2" "$3"
    else
        tap_skip "$3"
    fi
}

require_s3rdma

test_init
tap_plan 19

BUCKET="$(bucket_name)"

# The S3 server comes first, so that the service inherits the environment of the
# cuObject client library and finds the server when the plugin initializes.
start_s3rdma_server
if [ $? -ne 0 ]; then
    tap_bail "Unable to start an S3-over-RDMA test server instance."
fi

start_service
if [ $? -ne 0 ]; then
    tap_diag "Service log:"
    tap_diag_file "$TEST_DIR/service.log"
    tap_bail "Unable to start an elbencho service instance."
fi

tap_ok "elbencho service instance is listening on port $SERVICE_PORT"

################## Upload and download over RDMA through the service ##################

run_elbencho write \
    --hosts "localhost:$SERVICE_PORT" \
    "${S3_OPTS[@]}" \
    -d -w \
    -t $NUM_THREADS -n 0 -N $NUM_OBJECTS -s $OBJ_SIZE -b $OBJ_SIZE \
    --verify 1 --blockvarpct 0 \
    "s3://$BUCKET"
assert_ok $? "bucket creation and upload of $EXPECTED_OBJECTS objects over RDMA through the service succeed"

assert_eq "$(json_config "$ELB_JSON" WRITE hosts)" "1" \
    "write phase result reports a single service host"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done entries)" "$EXPECTED_OBJECTS" \
    "write phase reports $EXPECTED_OBJECTS uploaded objects"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$EXPECTED_BYTES" \
    "write phase reports $EXPECTED_BYTES uploaded bytes"
assert_eq "$(s3_object_count "$BUCKET")" "$EXPECTED_OBJECTS" \
    "the object listing contains $EXPECTED_OBJECTS objects"
rdma_ops_check PUT "$EXPECTED_OBJECTS" \
    "the server log shows $EXPECTED_OBJECTS RDMA transfers for the uploads"

run_elbencho read \
    --hosts "localhost:$SERVICE_PORT" \
    "${S3_OPTS[@]}" \
    -r \
    -t $NUM_THREADS -n 0 -N $NUM_OBJECTS -s $OBJ_SIZE -b $OBJ_SIZE \
    --verify 1 \
    "s3://$BUCKET"
assert_ok $? "download over RDMA through the service with data integrity check succeeds"

assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$EXPECTED_BYTES" \
    "read phase reports $EXPECTED_BYTES downloaded bytes"
rdma_ops_check GET "$EXPECTED_OBJECTS" \
    "the server log shows $EXPECTED_OBJECTS RDMA transfers for the downloads"

################## The same service without the plugin ##################

run_elbencho plainread \
    --hosts "localhost:$SERVICE_PORT" \
    "${S3_OPTS_NOPLUGIN[@]}" \
    -r \
    -t $NUM_THREADS -n 0 -N $NUM_OBJECTS -s $OBJ_SIZE -b $OBJ_SIZE \
    --verify 1 \
    "s3://$BUCKET"
assert_ok $? "download without the plugin through the same service succeeds"

assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$EXPECTED_BYTES" \
    "read phase without the plugin reports $EXPECTED_BYTES downloaded bytes"
rdma_ops_check GET "$EXPECTED_OBJECTS" \
    "the server log shows no additional RDMA transfer, i.e. the service deactivated the plugin"

################## The plugin is activated again ##################

run_elbencho rewrite \
    --hosts "localhost:$SERVICE_PORT" \
    "${S3_OPTS[@]}" \
    -w \
    -t $NUM_THREADS -n 0 -N $NUM_OBJECTS -s $OBJ_SIZE -b $OBJ_SIZE \
    --verify 1 --blockvarpct 0 \
    "s3://$BUCKET"
assert_ok $? "a second upload with the plugin through the same service succeeds"

rdma_ops_check PUT "$((2 * EXPECTED_OBJECTS))" \
    "the server log shows $((2 * EXPECTED_OBJECTS)) RDMA transfers for the uploads now"

################## Delete the objects and the bucket ##################

run_elbencho delete \
    --hosts "localhost:$SERVICE_PORT" \
    "${S3_OPTS[@]}" \
    -F -D --nodelerr \
    -t $NUM_THREADS -n 0 -N $NUM_OBJECTS \
    "s3://$BUCKET"
assert_ok $? "delete phase of the objects and the bucket through the service succeeds"

assert_eq "$(json_value "$ELB_JSON" RMOBJECTS last_done entries)" "$EXPECTED_OBJECTS" \
    "rmobjects phase reports $EXPECTED_OBJECTS deleted objects"

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
