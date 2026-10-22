#!/bin/bash
#
# Netbench interfaces selected by IP subnet ("--netdevs 127.0.0.0/8").
#
# A subnet in CIDR notation stands for every interface that has an address in
# it. On the loopback subnet that is the "lo" interface with 127.0.0.1, so the
# server's endpoints have to end up on that address. A service refuses to start
# when no interface matches any of its "--netdevs" entries; an entry that
# matches nothing while others do only gives a warning, because the interfaces
# are resolved again at every run (a cable may get plugged in later).

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/netbench.sh" || exit 1

NUM_THREADS=2
FILE_SIZE=$((8 * 1024 * 1024))
SEND_SIZE=$((1024 * 1024))
RECV_SIZE=4096
SUBNET="127.0.0.0/8"
NO_SUCH_SUBNET="10.255.255.0/24"

test_init
tap_plan 11

netbench_start_service --netdevs "$SUBNET" && netbench_start_service
if [ $? -ne 0 ]; then
    tap_bail "Unable to start 2 elbencho service instances."
fi

tap_ok "a service with \"--netdevs $SUBNET\" and a plain service are listening"

SERVERS="$(netbench_hosts 0 1)"
CLIENTS="$(netbench_hosts 1 1)"
SERVER_LOG="${NETBENCH_LOGS[0]}"
CLIENT_LOG="${NETBENCH_LOGS[1]}"

SERVER_MARK="$(netbench_log_mark "$SERVER_LOG")"
CLIENT_MARK="$(netbench_log_mark "$CLIENT_LOG")"

run_elbencho subnet \
    --netbench \
    --servers "$SERVERS" --clients "$CLIENTS" \
    -s $FILE_SIZE --sendsize $SEND_SIZE --recvsize $RECV_SIZE -t $NUM_THREADS
assert_ok $? "netbench run against the subnet-selected server interface succeeds"

EXPECTED="$(netbench_expected_send_bytes 1 $NUM_THREADS $FILE_SIZE $SEND_SIZE $RECV_SIZE)"
EXPECTED_RECV="$(netbench_expected_recv_bytes 1 $NUM_THREADS $FILE_SIZE $SEND_SIZE $RECV_SIZE)"

assert_eq "$(json_value "$ELB_JSON" "$NET_PHASE" last_done bytes)" "$EXPECTED" \
    "the run sent exactly $EXPECTED bytes"
assert_eq "$(netbench_log_count_since "$SERVER_LOG" "$SERVER_MARK" 'endpoint ready')" \
    "$NUM_THREADS" "the subnet expanded to one interface, so the server has $NUM_THREADS endpoints"

# the endpoint addresses that the client connected to, without their ports
ENDPOINT_IPS=""
for addr in $(netbench_log_endpoints_since "$CLIENT_LOG" "$CLIENT_MARK"); do
    ENDPOINT_IPS="$ENDPOINT_IPS $(netbench_ip_part "$addr")"
done

assert_eq "$(echo $ENDPOINT_IPS | tr ' ' '\n' | sort -u | tr '\n' ' ' | sed -e 's/ $//')" \
    "127.0.0.1" "the server endpoints are on the loopback address of the subnet"

################## A missing subnet among existing ones is a warning ##################

netbench_start_service --netdevs "$SUBNET,$NO_SUCH_SUBNET"
assert_ok $? "a service with \"--netdevs $SUBNET,$NO_SUCH_SUBNET\" starts"

WARN_LOG="${NETBENCH_LOGS[2]}"

assert_match "$(cat "$WARN_LOG")" \
    "WARNING: No network interface currently matches \"--netdevs\" entry \"$NO_SUCH_SUBNET\"" \
    "the service warns about the subnet without interfaces"

run_elbencho warnsubnet \
    --netbench \
    --servers "$(netbench_hosts 2 1)" --clients "$CLIENTS" \
    -s $FILE_SIZE --sendsize $SEND_SIZE --recvsize $RECV_SIZE -t $NUM_THREADS
assert_ok $? "netbench run against the server with a missing subnet succeeds"

################## A list without any existing interface is rejected ##################

NOSUBNET_LOG="$TEST_DIR/nosubnet.log"
NOSUBNET_PORT="$(find_free_port)"

"$ELBENCHO_TEST_BIN" --service --foreground --port "$NOSUBNET_PORT" --netdevs "$NO_SUCH_SUBNET" \
    > "$NOSUBNET_LOG" 2>&1
RC=$?
trace_cmd "service start with a subnet that no interface has" \
    "$ELBENCHO_TEST_BIN" --service --foreground --port "$NOSUBNET_PORT" --netdevs "$NO_SUCH_SUBNET"
trace_add_log "$NOSUBNET_LOG"

assert_nok $RC "a service with \"--netdevs $NO_SUCH_SUBNET\" refuses to start"
assert_match "$(cat "$NOSUBNET_LOG")" "None of the network interfaces given with" \
    "the missing interface is reported as the reason"

################## Shut the services down ##################

netbench_quit_services
assert_ok $? "all service instances terminate on \"--quit\""
