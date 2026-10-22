#!/bin/bash
#
# Netbench mode with both data directions.
#
# A send size larger than the receive size simulates writes from the clients to
# the servers, and a receive size larger than the send size simulates reads.
# Both are run against the same pair of service instances, which also covers
# that service instances can be reused for a second netbench run. The write
# direction is given as "-b", the read direction as "--sendsize", because both
# names of the option have to work.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/netbench.sh" || exit 1

NUM_SERVERS=1
NUM_CLIENTS=1
NUM_THREADS=1
NUM_SERVICES=$((NUM_SERVERS + NUM_CLIENTS))

# Write direction: a large request and a small response.
WRITE_SIZE=$((8 * 1024 * 1024))
WRITE_SEND=$((1024 * 1024))
WRITE_RECV=4096

# Read direction: a small request and a large response. The transfer size is
# reduced so that the run stays just as short, because the response bytes
# dominate the total here.
READ_SIZE=$((256 * 1024))
READ_SEND=4096
READ_RECV=$((1024 * 1024))

test_init
tap_plan 14

netbench_start_services $NUM_SERVICES
if [ $? -ne 0 ]; then
    tap_bail "Unable to start $NUM_SERVICES elbencho service instances."
fi

tap_ok "$NUM_SERVICES elbencho service instances are listening"

SERVERS="$(netbench_hosts 0 $NUM_SERVERS)"
CLIENTS="$(netbench_hosts $NUM_SERVERS $NUM_CLIENTS)"

WRITE_EXPECTED="$(netbench_expected_send_bytes \
    $NUM_CLIENTS $NUM_THREADS $WRITE_SIZE $WRITE_SEND $WRITE_RECV)"
WRITE_EXPECTED_RECV="$(netbench_expected_recv_bytes \
    $NUM_CLIENTS $NUM_THREADS $WRITE_SIZE $WRITE_SEND $WRITE_RECV)"
READ_EXPECTED="$(netbench_expected_send_bytes \
    $NUM_CLIENTS $NUM_THREADS $READ_SIZE $READ_SEND $READ_RECV)"
READ_EXPECTED_RECV="$(netbench_expected_recv_bytes \
    $NUM_CLIENTS $NUM_THREADS $READ_SIZE $READ_SEND $READ_RECV)"

################## Write direction: send size above receive size ##################

run_elbencho writedir \
    --netbench \
    --servers "$SERVERS" --clients "$CLIENTS" \
    -s $WRITE_SIZE -b $WRITE_SEND --recvsize $WRITE_RECV -t $NUM_THREADS
assert_ok $? "netbench run with a send size above the receive size succeeds"

assert_eq "$(json_config "$ELB_JSON" "$NET_PHASE" block_size)" "$WRITE_SEND" \
    "the write direction run confirms the given send size as block size"
assert_eq "$(json_value "$ELB_JSON" "$NET_PHASE" last_done bytes)" "$WRITE_EXPECTED" \
    "the write direction run transferred exactly $WRITE_EXPECTED bytes"
assert_eq "$(json_value_rwmix "$ELB_JSON" "$NET_PHASE" last_done bytes)" "$WRITE_EXPECTED_RECV" \
    "the write direction receive side reports the received bytes"
assert_gt "$(json_value "$ELB_JSON" "$NET_PHASE" last_done 'bytes/s')" 0 \
    "the write direction run reports a non-zero throughput"

################## Read direction: receive size above send size ##################

run_elbencho readdir \
    --netbench \
    --servers "$SERVERS" --clients "$CLIENTS" \
    -s $READ_SIZE --sendsize $READ_SEND --recvsize $READ_RECV -t $NUM_THREADS
assert_ok $? "netbench run with a receive size above the send size succeeds"

assert_eq "$(json_config "$ELB_JSON" "$NET_PHASE" block_size)" "$READ_SEND" \
    "the read direction run confirms the given send size as block size"
assert_eq "$(json_value "$ELB_JSON" "$NET_PHASE" last_done bytes)" "$READ_EXPECTED" \
    "the read direction run transferred exactly $READ_EXPECTED bytes"
assert_eq "$(json_value_rwmix "$ELB_JSON" "$NET_PHASE" last_done bytes)" "$READ_EXPECTED_RECV" \
    "the read direction receive side reports the received bytes"
assert_gt "$(json_value "$ELB_JSON" "$NET_PHASE" last_done 'bytes/s')" 0 \
    "the read direction run reports a non-zero throughput"

################## The receive size really dominates the read direction ##################

# In the read direction the responses make up almost all of the transferred
# data, while in the write direction it is the requests. This compares the
# response share of the total between the two runs.
WRITE_RECV_SHARE=$(( NUM_CLIENTS * NUM_THREADS * (WRITE_SIZE / WRITE_SEND) * WRITE_RECV ))
READ_RECV_SHARE=$(( NUM_CLIENTS * NUM_THREADS * (READ_SIZE / READ_SEND) * READ_RECV ))

assert_gt "$READ_RECV_SHARE" "$(( READ_EXPECTED - READ_RECV_SHARE ))" \
    "the responses dominate the total in the read direction"
assert_gt "$(( WRITE_EXPECTED - WRITE_RECV_SHARE ))" "$WRITE_RECV_SHARE" \
    "the requests dominate the total in the write direction"

################## Shut the services down ##################

netbench_quit_services
assert_ok $? "all service instances terminate on \"--quit\""
