#!/bin/bash
#
# Netbench mode over RDMA with the ibverbs engine ("--netengine ibverbs").
#
# This needs an RDMA-capable interface (InfiniBand or RoCE) with an IPv4
# address on this host. Server and client run on that same address, so the
# traffic loops through the adapter. The engine pins only its own buffers, so
# a few MiB of locked memory are enough. Skipped where no such interface
# exists.
#
# The same services then run a tcp engine benchmark, which shows that the
# engine can change between runs without a service restart.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/netbench.sh" || exit 1

require_build_feature ibverbs

NET_PHASE="NET/ibverbs"

NUM_THREADS=2
IO_DEPTH=4
FILE_SIZE=$((8 * 1024 * 1024))
SEND_SIZE=$((64 * 1024))
RECV_SIZE=4096

RDMA_IP="$(rdma_first_ipv4)"
[ -n "$RDMA_IP" ] || tap_skip_all "no RDMA-capable interface with an IPv4 address found"

# per endpoint: iodepth x sendsize (server) or iodepth x (sendsize + 2 x recvsize) (client)
require_memlock 4

test_init
tap_plan 18

netbench_start_service --netdevs "$RDMA_IP" && netbench_start_service --netdevs "$RDMA_IP"
if [ $? -ne 0 ]; then
    tap_bail "Unable to start 2 elbencho service instances."
fi

tap_ok "2 elbencho service instances are listening"

SERVERS="$RDMA_IP:${NETBENCH_PORTS[0]}"
CLIENTS="$RDMA_IP:${NETBENCH_PORTS[1]}"
SERVER_LOG="${NETBENCH_LOGS[0]}"
CLIENT_LOG="${NETBENCH_LOGS[1]}"

################## Ping-pong streams over ibverbs ##################

SERVER_MARK="$(netbench_log_mark "$SERVER_LOG")"
CLIENT_MARK="$(netbench_log_mark "$CLIENT_LOG")"

run_elbencho ibverbs \
    --netbench --netengine ibverbs \
    --servers "$SERVERS" --clients "$CLIENTS" \
    -s $FILE_SIZE --sendsize $SEND_SIZE --recvsize $RECV_SIZE -t $NUM_THREADS --iodepth $IO_DEPTH
assert_ok $? "netbench run with the ibverbs engine succeeds"

EXPECTED="$(netbench_expected_send_bytes 1 $NUM_THREADS $FILE_SIZE $SEND_SIZE $RECV_SIZE)"
EXPECTED_RECV="$(netbench_expected_recv_bytes 1 $NUM_THREADS $FILE_SIZE $SEND_SIZE $RECV_SIZE)"

assert_eq "$(json_value "$ELB_JSON" "$NET_PHASE" last_done bytes)" "$EXPECTED" \
    "the ibverbs run sent exactly $EXPECTED bytes"
assert_eq "$(json_value_rwmix "$ELB_JSON" "$NET_PHASE" last_done bytes)" "$EXPECTED_RECV" \
    "the ibverbs receive side reports the received bytes"
assert_eq "$(netbench_log_count_since "$CLIENT_LOG" "$CLIENT_MARK" 'endpoint: ibverbs rc:')" \
    "$NUM_THREADS" "every client thread connected to an ibverbs endpoint"
assert_eq "$(netbench_log_count_since "$SERVER_LOG" "$SERVER_MARK" 'Accepted stream from')" \
    "$NUM_THREADS" "the server accepted one stream per client thread"
assert_eq "$(netbench_log_endpoints_since "$CLIENT_LOG" "$CLIENT_MARK")" "$RDMA_IP" \
    "the server endpoints are on the RDMA address"

################## One-directional streams over ibverbs ##################

run_elbencho ibverbssendonly \
    --netbench --netengine ibverbs \
    --servers "$SERVERS" --clients "$CLIENTS" \
    -s $FILE_SIZE --sendsize $SEND_SIZE --recvsize 0 -t $NUM_THREADS --iodepth $IO_DEPTH
assert_ok $? "send-only netbench run with the ibverbs engine succeeds"

EXPECTED="$(netbench_expected_send_bytes 1 $NUM_THREADS $FILE_SIZE $SEND_SIZE 0)"
EXPECTED_RECV="$(netbench_expected_recv_bytes 1 $NUM_THREADS $FILE_SIZE $SEND_SIZE 0)"

assert_eq "$(json_value "$ELB_JSON" "$NET_PHASE" last_done bytes)" "$EXPECTED" \
    "the send-only ibverbs run sent exactly $EXPECTED bytes"

run_elbencho ibverbsrecvonly \
    --netbench --netengine ibverbs \
    --servers "$SERVERS" --clients "$CLIENTS" \
    -s $FILE_SIZE --sendsize 0 --recvsize $SEND_SIZE -t $NUM_THREADS --iodepth $IO_DEPTH
assert_ok $? "receive-only netbench run with the ibverbs engine succeeds"

EXPECTED="$(netbench_expected_send_bytes 1 $NUM_THREADS $FILE_SIZE 0 $SEND_SIZE)"
EXPECTED_RECV="$(netbench_expected_recv_bytes 1 $NUM_THREADS $FILE_SIZE 0 $SEND_SIZE)"

assert_eq "$(json_value_rwmix "$ELB_JSON" "$NET_PHASE" last_done bytes)" "$EXPECTED_RECV" \
    "the receive-only ibverbs run received exactly $EXPECTED_RECV bytes"

################## Provoked receiver-not-ready retries ##################

# With half as many receive buffers as the client has blocks in flight, the
# server answers some blocks with RNR NAKs and the adapter counts them (where
# its sysfs counters exist); the stream still completes through the retries.
RNR_BEFORE="$(rdma_rnr_nak_count "$RDMA_IP")"

run_elbencho ibverbsrnr \
    --netbench --netengine ibverbs \
    --servers "$SERVERS" --clients "$CLIENTS" \
    -s $FILE_SIZE --sendsize $SEND_SIZE --recvsize 0 -t $NUM_THREADS --iodepth $IO_DEPTH \
    --netrecvpct 50
assert_ok $? "send-only ibverbs run with half as many receive buffers as blocks in flight succeeds"

EXPECTED="$(netbench_expected_send_bytes 1 $NUM_THREADS $FILE_SIZE $SEND_SIZE 0)"

assert_eq "$(json_value "$ELB_JSON" "$NET_PHASE" last_done bytes)" "$EXPECTED" \
    "the ibverbs run with fewer receive buffers sent exactly $EXPECTED bytes"

if [ -n "$RNR_BEFORE" ]; then
    assert_gt "$(rdma_rnr_nak_count "$RDMA_IP")" "$RNR_BEFORE" \
        "the adapter counted receiver-not-ready NAKs during the run"
else
    tap_skip "the RDMA device has no receiver-not-ready NAK counter in sysfs"
fi

################## The same services switch to the tcp engine ##################

NET_PHASE="NET/tcp"
CLIENT_MARK="$(netbench_log_mark "$CLIENT_LOG")"

run_elbencho tcpafter \
    --netbench --netengine tcp \
    --servers "$SERVERS" --clients "$CLIENTS" \
    -s $FILE_SIZE --sendsize $SEND_SIZE --recvsize $RECV_SIZE -t $NUM_THREADS
assert_ok $? "a tcp engine run on the same services succeeds"

EXPECTED="$(netbench_expected_send_bytes 1 $NUM_THREADS $FILE_SIZE $SEND_SIZE $RECV_SIZE)"
EXPECTED_RECV="$(netbench_expected_recv_bytes 1 $NUM_THREADS $FILE_SIZE $SEND_SIZE $RECV_SIZE)"

assert_eq "$(json_value "$ELB_JSON" "$NET_PHASE" last_done bytes)" "$EXPECTED" \
    "the tcp run sent exactly $EXPECTED bytes"
assert_eq "$(netbench_log_count_since "$CLIENT_LOG" "$CLIENT_MARK" 'endpoint: tcp socket:')" \
    "$NUM_THREADS" "every client thread connected to a tcp endpoint this time"

################## Shut the services down ##################

netbench_quit_services
assert_ok $? "all service instances terminate on \"--quit\""
