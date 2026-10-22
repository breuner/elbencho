#!/bin/bash
#
# Netbench mode over RDMA with the libfabric verbs provider ("--netprov rdma").
#
# This needs an RDMA-capable interface (InfiniBand or RoCE) with an IPv4
# address on this host and a locked memory limit of at least 512 MiB. Server
# and client run on that same address, so the traffic
# loops through the adapter. Skipped where no such interface exists, where the
# limit is too low or where libfabric offers no verbs provider.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/netbench.sh" || exit 1

require_build_feature libfabric

NET_PHASE="NET/libfabric"

NUM_THREADS=2
FILE_SIZE=$((16 * 1024 * 1024))
SEND_SIZE=$((1024 * 1024))
RECV_SIZE=4096

RDMA_IP="$(rdma_first_ipv4)"
[ -n "$RDMA_IP" ] || tap_skip_all "no RDMA-capable interface with an IPv4 address found"

# The verbs provider pins about 65 MiB per endpoint at libfabric's defaults
# (mostly sized by FI_UNIVERSE_SIZE), and this test has four endpoints: it
# passes with a 256 MiB limit and fails with 128 MiB.
require_memlock 512

"$ELBENCHO_TEST_BIN" --netbench --netprov list 2>/dev/null | grep -q "^  verbs" || \
    tap_skip_all "libfabric offers no verbs provider on this host"

test_init
tap_plan 9

netbench_start_services 2
if [ $? -ne 0 ]; then
    tap_bail "Unable to start 2 elbencho service instances."
fi

tap_ok "2 elbencho service instances are listening"

SERVERS="$RDMA_IP:${NETBENCH_PORTS[0]}"
CLIENTS="$RDMA_IP:${NETBENCH_PORTS[1]}"
SERVER_LOG="${NETBENCH_LOGS[0]}"
CLIENT_LOG="${NETBENCH_LOGS[1]}"

SERVER_MARK="$(netbench_log_mark "$SERVER_LOG")"
CLIENT_MARK="$(netbench_log_mark "$CLIENT_LOG")"

run_elbencho rdma \
    --netbench --netprov rdma \
    --servers "$SERVERS" --clients "$CLIENTS" \
    -s $FILE_SIZE --sendsize $SEND_SIZE --recvsize $RECV_SIZE -t $NUM_THREADS
assert_ok $? "netbench run over RDMA succeeds"

EXPECTED="$(netbench_expected_send_bytes 1 $NUM_THREADS $FILE_SIZE $SEND_SIZE $RECV_SIZE)"
EXPECTED_RECV="$(netbench_expected_recv_bytes 1 $NUM_THREADS $FILE_SIZE $SEND_SIZE $RECV_SIZE)"

assert_eq "$(json_value "$ELB_JSON" "$NET_PHASE" last_done bytes)" "$EXPECTED" \
    "the RDMA run sent exactly $EXPECTED bytes"
assert_eq "$(json_value_rwmix "$ELB_JSON" "$NET_PHASE" last_done bytes)" "$EXPECTED_RECV" \
    "the RDMA receive side reports the received bytes"
assert_eq "$(netbench_log_count_since "$CLIENT_LOG" "$CLIENT_MARK" 'endpoint: verbs')" \
    "$NUM_THREADS" "every client thread connected to a verbs endpoint"
assert_eq "$(netbench_log_count_since "$SERVER_LOG" "$SERVER_MARK" 'Accepted stream from')" \
    "$NUM_THREADS" "the server accepted one stream per client thread"

################## Receive-only streams over RDMA ##################

run_elbencho rdmarecvonly \
    --netbench --netprov rdma \
    --servers "$SERVERS" --clients "$CLIENTS" \
    -s $FILE_SIZE --sendsize 0 --recvsize $SEND_SIZE -t $NUM_THREADS --iodepth 4
assert_ok $? "receive-only netbench run over RDMA succeeds"

EXPECTED="$(netbench_expected_send_bytes 1 $NUM_THREADS $FILE_SIZE 0 $SEND_SIZE)"
EXPECTED_RECV="$(netbench_expected_recv_bytes 1 $NUM_THREADS $FILE_SIZE 0 $SEND_SIZE)"

assert_eq "$(json_value_rwmix "$ELB_JSON" "$NET_PHASE" last_done bytes)" "$EXPECTED_RECV" \
    "the receive-only RDMA run received exactly $EXPECTED_RECV bytes"

netbench_quit_services
assert_ok $? "all service instances terminate on \"--quit\""
