#!/bin/bash
#
# Netbench mode with a fixed data port on the server.
#
# By default the data endpoints use ports chosen by the OS, which the clients
# learn through the service HTTP API. "--netport" on a server service fixes
# the base port instead: endpoint number N listens on base port plus N, i.e. one
# port per server thread here. A second run against the same services shows
# that the ports are released and taken again.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/netbench.sh" || exit 1

NUM_THREADS=2
FILE_SIZE=$((4 * 1024 * 1024))
SEND_SIZE=$((1024 * 1024))
RECV_SIZE=4096

test_init
tap_plan 7

# two consecutive free ports for the two server threads
read -r BASE_PORT SECOND_PORT <<< "$(find_free_port_pair)"
if [ -z "$SECOND_PORT" ]; then
    tap_bail "No free port pair found."
fi

netbench_start_service --netport "$BASE_PORT" && netbench_start_service
if [ $? -ne 0 ]; then
    tap_bail "Unable to start 2 elbencho service instances."
fi

tap_ok "2 elbencho service instances are listening"

SERVERS="$(netbench_hosts 0 1)"
CLIENTS="$(netbench_hosts 1 1)"
CLIENT_LOG="${NETBENCH_LOGS[1]}"
EXPECTED_PORTS="$BASE_PORT $SECOND_PORT"
EXPECTED="$(netbench_expected_send_bytes 1 $NUM_THREADS $FILE_SIZE $SEND_SIZE $RECV_SIZE)"
EXPECTED_RECV="$(netbench_expected_recv_bytes 1 $NUM_THREADS $FILE_SIZE $SEND_SIZE $RECV_SIZE)"

CLIENT_MARK="$(netbench_log_mark "$CLIENT_LOG")"

run_elbencho fixedport \
    --netbench \
    --servers "$SERVERS" --clients "$CLIENTS" \
    -s $FILE_SIZE --sendsize $SEND_SIZE --recvsize $RECV_SIZE -t $NUM_THREADS
assert_ok $? "netbench run against a server with a fixed base port succeeds"

assert_eq "$(json_value "$ELB_JSON" "$NET_PHASE" last_done bytes)" "$EXPECTED" \
    "the run sent exactly $EXPECTED bytes"
# ("localhost" may resolve to 127.0.0.1 or ::1, so only the ports are compared)
assert_eq "$(netbench_log_endpoint_ports_since "$CLIENT_LOG" "$CLIENT_MARK")" \
    "$EXPECTED_PORTS" \
    "the client connected to the data endpoints on the fixed ports"

################## Second run on the same ports ##################

CLIENT_MARK="$(netbench_log_mark "$CLIENT_LOG")"

run_elbencho fixedportagain \
    --netbench \
    --servers "$SERVERS" --clients "$CLIENTS" \
    -s $FILE_SIZE --sendsize $SEND_SIZE --recvsize $RECV_SIZE -t $NUM_THREADS
assert_ok $? "a second netbench run against the same server succeeds"

assert_eq "$(netbench_log_endpoint_ports_since "$CLIENT_LOG" "$CLIENT_MARK")" \
    "$EXPECTED_PORTS" \
    "the second run used the same fixed ports again"

netbench_quit_services
assert_ok $? "all service instances terminate on \"--quit\""
