#!/bin/bash
#
# Netbench mode with one-directional streams and several rounds in flight.
#
# A receive size of 0 gives send-only streams (the client sends, the server
# only receives), a send size of 0 gives receive-only streams (the server
# sends a continuous stream of blocks). Both are run with the default IO depth
# of 1 and with several rounds in flight ("--iodepth"), against the same pair
# of service instances. The receiving side of such a stream posts
# "--netrecvpct" percent of the sender's IO depth as receive buffers; with the
# tcp engine, fewer buffers than blocks in flight only delay the reads, so the
# runs with 50% and 150% have to transfer the same amounts.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/netbench.sh" || exit 1

NUM_SERVERS=1
NUM_CLIENTS=1
NUM_THREADS=2
NUM_SERVICES=$((NUM_SERVERS + NUM_CLIENTS))

LARGE_SIZE=$((8 * 1024 * 1024))
LARGE_BLOCK=$((1024 * 1024))
SMALL_SIZE=$((1024 * 1024))
SMALL_BLOCK=4096
IO_DEPTH=8

test_init
tap_plan 23

netbench_start_services $NUM_SERVICES
if [ $? -ne 0 ]; then
    tap_bail "Unable to start $NUM_SERVICES elbencho service instances."
fi

tap_ok "$NUM_SERVICES elbencho service instances are listening"

SERVERS="$(netbench_hosts 0 $NUM_SERVERS)"
CLIENTS="$(netbench_hosts $NUM_SERVERS $NUM_CLIENTS)"
SERVER_LOG="${NETBENCH_LOGS[0]}"

################## Send-only streams ##################

SERVER_MARK="$(netbench_log_mark "$SERVER_LOG")"

run_elbencho sendonly \
    --netbench \
    --servers "$SERVERS" --clients "$CLIENTS" \
    -s $LARGE_SIZE --sendsize $LARGE_BLOCK --recvsize 0 -t $NUM_THREADS
assert_ok $? "netbench run with send-only streams succeeds"

EXPECTED="$(netbench_expected_send_bytes $NUM_CLIENTS $NUM_THREADS $LARGE_SIZE $LARGE_BLOCK 0)"
EXPECTED_RECV="$(netbench_expected_recv_bytes $NUM_CLIENTS $NUM_THREADS $LARGE_SIZE $LARGE_BLOCK 0)"

assert_eq "$(json_value "$ELB_JSON" "$NET_PHASE" last_done bytes)" "$EXPECTED" \
    "the send-only run sent exactly $EXPECTED bytes"
assert_eq "$(json_value_rwmix "$ELB_JSON" "$NET_PHASE" last_done bytes)" "0" \
    "the send-only run received nothing"
assert_gt "$(json_value "$ELB_JSON" "$NET_PHASE" last_done 'bytes/s')" 0 \
    "the send-only run reports a non-zero throughput"
assert_eq "$(netbench_log_count_since "$SERVER_LOG" "$SERVER_MARK" 'Accepted stream from')" \
    "$NUM_THREADS" "the server accepted one stream per client thread"

################## Receive-only streams ##################

run_elbencho recvonly \
    --netbench \
    --servers "$SERVERS" --clients "$CLIENTS" \
    -s $LARGE_SIZE --sendsize 0 --recvsize $LARGE_BLOCK -t $NUM_THREADS
assert_ok $? "netbench run with receive-only streams succeeds"

EXPECTED="$(netbench_expected_send_bytes $NUM_CLIENTS $NUM_THREADS $LARGE_SIZE 0 $LARGE_BLOCK)"
EXPECTED_RECV="$(netbench_expected_recv_bytes $NUM_CLIENTS $NUM_THREADS $LARGE_SIZE 0 $LARGE_BLOCK)"

assert_eq "$(json_config "$ELB_JSON" "$NET_PHASE" block_size)" "0" \
    "the receive-only run reports a block size of zero"
assert_eq "$(json_value "$ELB_JSON" "$NET_PHASE" last_done bytes)" "0" \
    "the receive-only run sent nothing"
assert_eq "$(json_value_rwmix "$ELB_JSON" "$NET_PHASE" last_done bytes)" "$EXPECTED_RECV" \
    "the receive-only run received exactly $EXPECTED_RECV bytes"

################## Several rounds in flight ##################

run_elbencho pingpongdepth \
    --netbench \
    --servers "$SERVERS" --clients "$CLIENTS" \
    -s $SMALL_SIZE --sendsize $SMALL_BLOCK --recvsize $SMALL_BLOCK -t $NUM_THREADS \
    --iodepth $IO_DEPTH
assert_ok $? "netbench run with $IO_DEPTH rounds in flight succeeds"

EXPECTED="$(netbench_expected_send_bytes \
    $NUM_CLIENTS $NUM_THREADS $SMALL_SIZE $SMALL_BLOCK $SMALL_BLOCK)"
EXPECTED_RECV="$(netbench_expected_recv_bytes \
    $NUM_CLIENTS $NUM_THREADS $SMALL_SIZE $SMALL_BLOCK $SMALL_BLOCK)"

assert_eq "$(json_value "$ELB_JSON" "$NET_PHASE" last_done bytes)" "$EXPECTED" \
    "the run with $IO_DEPTH rounds in flight sent exactly $EXPECTED bytes"

run_elbencho sendonlydepth \
    --netbench \
    --servers "$SERVERS" --clients "$CLIENTS" \
    -s $SMALL_SIZE --sendsize $SMALL_BLOCK --recvsize 0 -t $NUM_THREADS --iodepth $IO_DEPTH
assert_ok $? "send-only netbench run with $IO_DEPTH rounds in flight succeeds"

EXPECTED="$(netbench_expected_send_bytes $NUM_CLIENTS $NUM_THREADS $SMALL_SIZE $SMALL_BLOCK 0)"
EXPECTED_RECV="$(netbench_expected_recv_bytes $NUM_CLIENTS $NUM_THREADS $SMALL_SIZE $SMALL_BLOCK 0)"

assert_eq "$(json_value "$ELB_JSON" "$NET_PHASE" last_done bytes)" "$EXPECTED" \
    "the send-only run with $IO_DEPTH rounds in flight sent exactly $EXPECTED bytes"

run_elbencho recvonlydepth \
    --netbench \
    --servers "$SERVERS" --clients "$CLIENTS" \
    -s $SMALL_SIZE --sendsize 0 --recvsize $SMALL_BLOCK -t $NUM_THREADS --iodepth $IO_DEPTH
assert_ok $? "receive-only netbench run with $IO_DEPTH rounds in flight succeeds"

EXPECTED="$(netbench_expected_send_bytes $NUM_CLIENTS $NUM_THREADS $SMALL_SIZE 0 $SMALL_BLOCK)"
EXPECTED_RECV="$(netbench_expected_recv_bytes $NUM_CLIENTS $NUM_THREADS $SMALL_SIZE 0 $SMALL_BLOCK)"

assert_eq "$(json_value_rwmix "$ELB_JSON" "$NET_PHASE" last_done bytes)" "$EXPECTED_RECV" \
    "the receive-only run with $IO_DEPTH rounds in flight received exactly $EXPECTED_RECV bytes"

################## Fewer and more receive buffers than blocks in flight ##################

run_elbencho sendonlyfewer \
    --netbench \
    --servers "$SERVERS" --clients "$CLIENTS" \
    -s $SMALL_SIZE --sendsize $SMALL_BLOCK --recvsize 0 -t $NUM_THREADS --iodepth $IO_DEPTH \
    --netrecvpct 50
assert_ok $? "send-only netbench run with half as many receive buffers as blocks in flight succeeds"

EXPECTED="$(netbench_expected_send_bytes $NUM_CLIENTS $NUM_THREADS $SMALL_SIZE $SMALL_BLOCK 0)"

assert_eq "$(json_value "$ELB_JSON" "$NET_PHASE" last_done bytes)" "$EXPECTED" \
    "the send-only run with fewer receive buffers sent exactly $EXPECTED bytes"

run_elbencho recvonlyfewer \
    --netbench \
    --servers "$SERVERS" --clients "$CLIENTS" \
    -s $SMALL_SIZE --sendsize 0 --recvsize $SMALL_BLOCK -t $NUM_THREADS --iodepth $IO_DEPTH \
    --netrecvpct 50
assert_ok $? "receive-only netbench run with half as many receive buffers as blocks in flight succeeds"

EXPECTED_RECV="$(netbench_expected_recv_bytes $NUM_CLIENTS $NUM_THREADS $SMALL_SIZE 0 $SMALL_BLOCK)"

assert_eq "$(json_value_rwmix "$ELB_JSON" "$NET_PHASE" last_done bytes)" "$EXPECTED_RECV" \
    "the receive-only run with fewer receive buffers received exactly $EXPECTED_RECV bytes"

run_elbencho sendonlymore \
    --netbench \
    --servers "$SERVERS" --clients "$CLIENTS" \
    -s $SMALL_SIZE --sendsize $SMALL_BLOCK --recvsize 0 -t $NUM_THREADS --iodepth $IO_DEPTH \
    --netrecvpct 150
assert_ok $? "send-only netbench run with 150% receive buffers succeeds"

EXPECTED="$(netbench_expected_send_bytes $NUM_CLIENTS $NUM_THREADS $SMALL_SIZE $SMALL_BLOCK 0)"

assert_eq "$(json_value "$ELB_JSON" "$NET_PHASE" last_done bytes)" "$EXPECTED" \
    "the send-only run with 150% receive buffers sent exactly $EXPECTED bytes"

################## Shut the services down ##################

netbench_quit_services
assert_ok $? "all service instances terminate on \"--quit\""
