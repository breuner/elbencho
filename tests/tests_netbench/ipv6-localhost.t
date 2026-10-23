#!/bin/bash
#
# Netbench mode over IPv6.
#
# The services are addressed as "[::1]:PORT", so the servers open their data
# endpoints on the IPv6 loopback address and the clients bind to it as well.
# Skipped on hosts without an IPv6 loopback address.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/netbench.sh" || exit 1

NUM_THREADS=2
FILE_SIZE=$((8 * 1024 * 1024))
SEND_SIZE=$((1024 * 1024))
RECV_SIZE=4096

grep -q "^00000000000000000000000000000001" /proc/net/if_inet6 2>/dev/null || \
    tap_skip_all "this host has no IPv6 loopback address"
is_cygwin && tap_skip_all "the elbencho service listens on IPv4 only on Windows"

test_init
tap_plan 7

netbench_start_services 2
if [ $? -ne 0 ]; then
    tap_bail "Unable to start 2 elbencho service instances."
fi

tap_ok "2 elbencho service instances are listening"

CLIENT_LOG="${NETBENCH_LOGS[1]}"
CLIENT_MARK="$(netbench_log_mark "$CLIENT_LOG")"

run_elbencho ipv6 \
    --netbench \
    --servers "[::1]:${NETBENCH_PORTS[0]}" --clients "[::1]:${NETBENCH_PORTS[1]}" \
    -s $FILE_SIZE --sendsize $SEND_SIZE --recvsize $RECV_SIZE -t $NUM_THREADS
assert_ok $? "netbench run over IPv6 succeeds"

EXPECTED="$(netbench_expected_send_bytes 1 $NUM_THREADS $FILE_SIZE $SEND_SIZE $RECV_SIZE)"
EXPECTED_RECV="$(netbench_expected_recv_bytes 1 $NUM_THREADS $FILE_SIZE $SEND_SIZE $RECV_SIZE)"

assert_eq "$(json_value "$ELB_JSON" "$NET_PHASE" last_done bytes)" "$EXPECTED" \
    "the run sent exactly $EXPECTED bytes"
assert_eq "$(json_value_rwmix "$ELB_JSON" "$NET_PHASE" last_done bytes)" "$EXPECTED_RECV" \
    "the receive side reports the received bytes"

# Both ends of the streams have to be IPv6 addresses, which the client log
# shows as "[::1]:PORT".
assert_match "$(netbench_log_endpoints_since "$CLIENT_LOG" "$CLIENT_MARK")" \
    '^\[::1\]:[0-9]+( \[::1\]:[0-9]+)*$' \
    "the server data endpoints are IPv6 loopback addresses"
assert_match "$(netbench_log_sources_since "$CLIENT_LOG" "$CLIENT_MARK")" \
    '^\[::1\]:[0-9]+( \[::1\]:[0-9]+)*$' \
    "the client endpoints are IPv6 loopback addresses"

netbench_quit_services
assert_ok $? "all service instances terminate on \"--quit\""
