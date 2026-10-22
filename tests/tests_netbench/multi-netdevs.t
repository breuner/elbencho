#!/bin/bash
#
# Netbench mode with several interfaces on the server and on the client.
#
# "--netdevs" is a service option: a server opens one data endpoint per thread
# on each listed interface, a client binds its threads round-robin to its
# interfaces. Linux treats the whole 127.0.0.0/8 range as local, so three
# loopback addresses stand in for three server interfaces and two for the
# client's. With six client threads, the round-robin placement has to use every
# server address and every client address, which is verified from the client's
# log of established connections.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/netbench.sh" || exit 1

SERVER_DEVS="127.0.0.1,127.0.0.2,127.0.0.3"
CLIENT_DEVS="127.0.0.1,127.0.0.2"
NUM_THREADS=6
FILE_SIZE=$((4 * 1024 * 1024))
SEND_SIZE=$((1024 * 1024))
RECV_SIZE=4096

[ "$(uname -s)" = "Linux" ] || tap_skip_all "needs the Linux 127.0.0.0/8 loopback range"

test_init
tap_plan 7

netbench_start_service --netdevs "$SERVER_DEVS" && netbench_start_service --netdevs "$CLIENT_DEVS"
if [ $? -ne 0 ]; then
    tap_bail "Unable to start 2 elbencho service instances."
fi

tap_ok "2 elbencho service instances are listening"

SERVERS="$(netbench_hosts 0 1)"
CLIENTS="$(netbench_hosts 1 1)"
SERVER_LOG="${NETBENCH_LOGS[0]}"
CLIENT_LOG="${NETBENCH_LOGS[1]}"
EXPECTED="$(netbench_expected_send_bytes 1 $NUM_THREADS $FILE_SIZE $SEND_SIZE $RECV_SIZE)"
EXPECTED_RECV="$(netbench_expected_recv_bytes 1 $NUM_THREADS $FILE_SIZE $SEND_SIZE $RECV_SIZE)"

SERVER_MARK="$(netbench_log_mark "$SERVER_LOG")"
CLIENT_MARK="$(netbench_log_mark "$CLIENT_LOG")"

run_elbencho multidevs \
    --netbench \
    --servers "$SERVERS" --clients "$CLIENTS" \
    -s $FILE_SIZE --sendsize $SEND_SIZE --recvsize $RECV_SIZE -t $NUM_THREADS
assert_ok $? "netbench run with several interfaces on both sides succeeds"

assert_eq "$(json_value "$ELB_JSON" "$NET_PHASE" last_done bytes)" "$EXPECTED" \
    "the run sent exactly $EXPECTED bytes"
assert_eq "$(netbench_log_count_since "$SERVER_LOG" "$SERVER_MARK" 'Accepted stream from')" \
    "$NUM_THREADS" "the server accepted one stream per client thread"

# the IPs of the server endpoints and of the client endpoints, each sorted
# and unique, as the client's log reports them
server_ips()
{
    local addr
    for addr in $(netbench_log_endpoints_since "$CLIENT_LOG" "$CLIENT_MARK"); do
        netbench_ip_part "$addr"
    done | sort -u | tr '\n' ' ' | sed -e 's/ $//'
}

client_ips()
{
    local addr
    for addr in $(netbench_log_sources_since "$CLIENT_LOG" "$CLIENT_MARK"); do
        netbench_ip_part "$addr"
    done | sort -u | tr '\n' ' ' | sed -e 's/ $//'
}

assert_eq "$(server_ips)" "$(echo "$SERVER_DEVS" | tr ',' ' ')" \
    "the streams used all three server interfaces"
assert_eq "$(client_ips)" "$(echo "$CLIENT_DEVS" | tr ',' ' ')" \
    "the streams used both client interfaces"

netbench_quit_services
assert_ok $? "all service instances terminate on \"--quit\""
