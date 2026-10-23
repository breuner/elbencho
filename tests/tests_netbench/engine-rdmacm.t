#!/bin/bash
#
# Netbench mode over RDMA with the rdmacm engine ("--netengine rdmacm").
#
# The same reliable connections as the ibverbs engine, but set up by RDMA CM:
# the server listens on its interface address, the client resolves and
# connects to it and the adapters negotiate ECE among themselves. This needs
# an RDMA-capable interface (InfiniBand or RoCE) with an IPv4 address on this
# host. Server and client run on that same address, so the traffic loops
# through the adapter. Skipped where no such interface exists.
#
# A server started with "--netport" listens on that fixed CM port, and the
# same services then run a tcp engine benchmark, which shows that the engine
# can change between runs without a service restart.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/netbench.sh" || exit 1

require_build_feature rdmacm

NET_PHASE="NET/rdmacm"

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
tap_plan 20

NET_PORT="$(find_free_port)"

# 0: server, 1: client, 2: server with a fixed CM port
netbench_start_service --netdevs "$RDMA_IP" && netbench_start_service --netdevs "$RDMA_IP" && \
    netbench_start_service --netdevs "$RDMA_IP" --netport "$NET_PORT"
if [ $? -ne 0 ]; then
    tap_bail "Unable to start 3 elbencho service instances."
fi

tap_ok "3 elbencho service instances are listening"

SERVERS="$RDMA_IP:${NETBENCH_PORTS[0]}"
CLIENTS="$RDMA_IP:${NETBENCH_PORTS[1]}"
SERVER_LOG="${NETBENCH_LOGS[0]}"
CLIENT_LOG="${NETBENCH_LOGS[1]}"

################## Ping-pong streams over rdmacm ##################

SERVER_MARK="$(netbench_log_mark "$SERVER_LOG")"
CLIENT_MARK="$(netbench_log_mark "$CLIENT_LOG")"

run_elbencho rdmacm \
    --netbench --netengine rdmacm \
    --servers "$SERVERS" --clients "$CLIENTS" \
    -s $FILE_SIZE --sendsize $SEND_SIZE --recvsize $RECV_SIZE -t $NUM_THREADS --iodepth $IO_DEPTH
assert_ok $? "netbench run with the rdmacm engine succeeds"

EXPECTED="$(netbench_expected_send_bytes 1 $NUM_THREADS $FILE_SIZE $SEND_SIZE $RECV_SIZE)"
EXPECTED_RECV="$(netbench_expected_recv_bytes 1 $NUM_THREADS $FILE_SIZE $SEND_SIZE $RECV_SIZE)"

assert_eq "$(json_value "$ELB_JSON" "$NET_PHASE" last_done bytes)" "$EXPECTED" \
    "the rdmacm run sent exactly $EXPECTED bytes"
assert_eq "$(json_value_rwmix "$ELB_JSON" "$NET_PHASE" last_done bytes)" "$EXPECTED_RECV" \
    "the rdmacm receive side reports the received bytes"
assert_eq "$(netbench_log_count_since "$CLIENT_LOG" "$CLIENT_MARK" 'endpoint: rdmacm rc:')" \
    "$NUM_THREADS" "every client thread connected to an rdmacm endpoint"
assert_eq "$(netbench_log_count_since "$SERVER_LOG" "$SERVER_MARK" 'Accepted stream from')" \
    "$NUM_THREADS" "the server accepted one stream per client thread"
# (rdmacm endpoints carry their CM port, so compare the IP parts)
ENDPOINT_IPS=""
for addr in $(netbench_log_endpoints_since "$CLIENT_LOG" "$CLIENT_MARK"); do
    ENDPOINT_IPS="$ENDPOINT_IPS $(netbench_ip_part "$addr")"
done

assert_eq "$(echo $ENDPOINT_IPS | tr ' ' '\n' | sort -u | tr '\n' ' ' | sed -e 's/ $//')" \
    "$RDMA_IP" "the server endpoints are on the RDMA address"

################## One-directional streams over rdmacm ##################

run_elbencho rdmacmsendonly \
    --netbench --netengine rdmacm \
    --servers "$SERVERS" --clients "$CLIENTS" \
    -s $FILE_SIZE --sendsize $SEND_SIZE --recvsize 0 -t $NUM_THREADS --iodepth $IO_DEPTH
assert_ok $? "send-only netbench run with the rdmacm engine succeeds"

EXPECTED="$(netbench_expected_send_bytes 1 $NUM_THREADS $FILE_SIZE $SEND_SIZE 0)"
EXPECTED_RECV="$(netbench_expected_recv_bytes 1 $NUM_THREADS $FILE_SIZE $SEND_SIZE 0)"

assert_eq "$(json_value "$ELB_JSON" "$NET_PHASE" last_done bytes)" "$EXPECTED" \
    "the send-only rdmacm run sent exactly $EXPECTED bytes"

run_elbencho rdmacmrecvonly \
    --netbench --netengine rdmacm \
    --servers "$SERVERS" --clients "$CLIENTS" \
    -s $FILE_SIZE --sendsize 0 --recvsize $SEND_SIZE -t $NUM_THREADS --iodepth $IO_DEPTH
assert_ok $? "receive-only netbench run with the rdmacm engine succeeds"

EXPECTED="$(netbench_expected_send_bytes 1 $NUM_THREADS $FILE_SIZE 0 $SEND_SIZE)"
EXPECTED_RECV="$(netbench_expected_recv_bytes 1 $NUM_THREADS $FILE_SIZE 0 $SEND_SIZE)"

assert_eq "$(json_value_rwmix "$ELB_JSON" "$NET_PHASE" last_done bytes)" "$EXPECTED_RECV" \
    "the receive-only rdmacm run received exactly $EXPECTED_RECV bytes"

################## Provoked receiver-not-ready retries ##################

# With eight small blocks in flight and two receive buffers per stream, the
# client posts blocks faster than the server reposts its buffers, so the server
# answers nearly every block with an RNR NAK and the adapter counts them (where
# its sysfs counters exist); the stream still completes through the retries.
# (Blocks of 64 KiB would leave the server time to repost while the client
# fills the next buffer, which makes the count small and load dependent.)
RNR_SEND_SIZE=4096
RNR_IO_DEPTH=8
RNR_BEFORE="$(rdma_rnr_nak_count "$RDMA_IP")"

run_elbencho rdmacmrnr \
    --netbench --netengine rdmacm \
    --servers "$SERVERS" --clients "$CLIENTS" \
    -s $FILE_SIZE --sendsize $RNR_SEND_SIZE --recvsize 0 -t $NUM_THREADS --iodepth $RNR_IO_DEPTH \
    --netrecvpct 25
assert_ok $? "send-only rdmacm run with a quarter as many receive buffers as blocks in flight succeeds"

EXPECTED="$(netbench_expected_send_bytes 1 $NUM_THREADS $FILE_SIZE $RNR_SEND_SIZE 0)"

assert_eq "$(json_value "$ELB_JSON" "$NET_PHASE" last_done bytes)" "$EXPECTED" \
    "the rdmacm run with fewer receive buffers sent exactly $EXPECTED bytes"

if [ -n "$RNR_BEFORE" ]; then
    assert_gt "$(rdma_rnr_nak_count "$RDMA_IP")" "$RNR_BEFORE" \
        "the adapter counted receiver-not-ready NAKs during the run"
else
    tap_skip "the RDMA device has no receiver-not-ready NAK counter in sysfs"
fi

################## A fixed CM listen port ##################

CLIENT_MARK="$(netbench_log_mark "$CLIENT_LOG")"

run_elbencho rdmacmport \
    --netbench --netengine rdmacm \
    --servers "$RDMA_IP:${NETBENCH_PORTS[2]}" --clients "$CLIENTS" \
    -s $FILE_SIZE --sendsize $SEND_SIZE --recvsize $RECV_SIZE -t $NUM_THREADS
assert_ok $? "netbench run against the server with a fixed CM port succeeds"

assert_eq "$(netbench_log_endpoint_ports_since "$CLIENT_LOG" "$CLIENT_MARK")" \
    "$NET_PORT $((NET_PORT + 1))" "the server endpoints listen on the fixed port and the next one"

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
