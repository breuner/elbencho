#!/bin/bash
#
# Upload dirs of two "--port 0" services on the same host.
#
# A service stores files that the coordinator uploads (the treefile of custom
# tree mode) in a dir named after its port. With "--port 0" the OS picks the
# port, and the dir has to be named after that actual port, not after the
# configured 0, or several such services on one host would share one dir and
# overwrite each other's uploads while a run is being prepared.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1

NUM_SERVICES=2
NUM_THREADS=2

SERVICE_PORTS=()
SERVICE_PIDS=()

start_service()
{
    local idx=${#SERVICE_PORTS[@]}
    local log="$TEST_DIR/service$idx.log"
    local pid
    local port

    "$ELBENCHO_TEST_BIN" --service --foreground --port 0 > "$log" 2>&1 &
    pid=$!
    register_pid "$pid"

    trace_cmd "service $idx start" "$ELBENCHO_TEST_BIN" --service --foreground --port 0
    trace_add_log "$log"

    port=$(wait_for_service "$pid" "$log" 10)
    if [ -z "$port" ]; then
        tap_diag "Service $idx did not come up."
        tap_diag_file "$log"
        return 1
    fi

    SERVICE_PORTS+=("$port")
    SERVICE_PIDS+=("$pid")

    return 0
}

test_init
tap_plan 7

DATA_DIR="$TEST_DIR/data"
TREEFILE="$TEST_DIR/tree.txt"

mkdir -p "$DATA_DIR"

cat > "$TREEFILE" <<TREEFILE_EOF
d sub1
f 4096 sub1/a
f 4096 sub1/b
f 65536 sub1/c
TREEFILE_EOF

# Where the services keep uploaded files (ELBENCHO_VAR_TMP in the sources).
if is_cygwin; then
    UPLOAD_BASE="$TEMP/elbencho_$(id -un)"
else
    UPLOAD_BASE="/var/tmp/elbencho_$(id -un)"
fi

################## Start the services ##################

for i in $(seq 1 $NUM_SERVICES); do
    start_service || tap_bail "Unable to start elbencho service instance $i."
done

HOSTS="localhost:${SERVICE_PORTS[0]},localhost:${SERVICE_PORTS[1]}"

tap_ok "two services are listening on ports ${SERVICE_PORTS[*]}"

################## Custom tree run through both services ##################

run_elbencho write \
    --hosts "$HOSTS" \
    -d -w -t $NUM_THREADS --treefile "$TREEFILE" \
    "$DATA_DIR"
assert_ok $? "custom tree write through both services succeeds"

assert_eq "$(count_files "$DATA_DIR")" "3" "the three files of the tree were created"

for port in "${SERVICE_PORTS[@]}"; do
    if [ -n "$(ls -A "${UPLOAD_BASE}_p$port" 2>/dev/null)" ]; then
        tap_ok "service on port $port keeps its uploaded tree file in its own upload dir"
    else
        tap_fail "service on port $port keeps its uploaded tree file in its own upload dir"
        tap_diag "Expected a non-empty dir: ${UPLOAD_BASE}_p$port"
    fi
done

################## Terminate the services ##################

run_elbencho quit --hosts "$HOSTS" --quit
assert_ok $? "\"--quit\" is accepted by both services"

ALL_GONE=1
for port in "${SERVICE_PORTS[@]}"; do
    wait_for_port_gone 127.0.0.1 "$port" 10 || ALL_GONE=0
done

if [ $ALL_GONE -eq 1 ]; then
    tap_ok "both services stopped listening after \"--quit\""
else
    tap_fail "both services stopped listening after \"--quit\""
fi
