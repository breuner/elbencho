#!/bin/bash
#
# Netbench mode with both data directions.
#
# A block size larger than the response size simulates writes from the clients
# to the servers, and a response size larger than the block size simulates
# reads. Both are run against the same pair of service instances, which also
# covers that service instances can be reused for a second netbench run.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/netbench.sh" || exit 1

NUM_SERVERS=1
NUM_CLIENTS=1
NUM_THREADS=1
NUM_SERVICES=$((NUM_SERVERS + NUM_CLIENTS))

# Write direction: a large request and a small response.
WRITE_SIZE=$((8 * 1024 * 1024))
WRITE_BLOCK=$((1024 * 1024))
WRITE_RESP=4096

# Read direction: a small request and a large response. The transfer size is
# reduced so that the run stays just as short, because the response bytes
# dominate the total here.
READ_SIZE=$((256 * 1024))
READ_BLOCK=4096
READ_RESP=$((1024 * 1024))

test_init
tap_plan 14

netbench_start_services $NUM_SERVICES
if [ $? -ne 0 ]; then
    tap_bail "Unable to start $NUM_SERVICES elbencho service instances."
fi

tap_ok "$NUM_SERVICES elbencho service instances are listening"

netbench_data_ports_free || tap_skip_all "a netbench data port is used by another process"

SERVERS="$(netbench_hosts 0 $NUM_SERVERS)"
CLIENTS="$(netbench_hosts $NUM_SERVERS $NUM_CLIENTS)"

WRITE_EXPECTED="$(netbench_expected_bytes \
    $NUM_CLIENTS $NUM_THREADS $WRITE_SIZE $WRITE_BLOCK $WRITE_RESP)"
READ_EXPECTED="$(netbench_expected_bytes \
    $NUM_CLIENTS $NUM_THREADS $READ_SIZE $READ_BLOCK $READ_RESP)"

################## Write direction: block size above response size ##################

run_elbencho writedir \
    --netbench \
    --servers "$SERVERS" --clients "$CLIENTS" \
    -s $WRITE_SIZE -b $WRITE_BLOCK --respsize $WRITE_RESP -t $NUM_THREADS
assert_ok $? "netbench run with a block size above the response size succeeds"

assert_eq "$(json_config "$ELB_JSON" NET block_size)" "$WRITE_BLOCK" \
    "the write direction run confirms the given block size"
assert_eq "$(json_value "$ELB_JSON" NET last_done bytes)" "$WRITE_EXPECTED" \
    "the write direction run transferred exactly $WRITE_EXPECTED bytes"
assert_eq "$(json_value_rwmix "$ELB_JSON" NET last_done bytes)" "$WRITE_EXPECTED" \
    "the write direction receive side reports the same total"
assert_gt "$(json_value "$ELB_JSON" NET last_done 'bytes/s')" 0 \
    "the write direction run reports a non-zero throughput"

################## Read direction: response size above block size ##################

run_elbencho readdir \
    --netbench \
    --servers "$SERVERS" --clients "$CLIENTS" \
    -s $READ_SIZE -b $READ_BLOCK --respsize $READ_RESP -t $NUM_THREADS
assert_ok $? "netbench run with a response size above the block size succeeds"

assert_eq "$(json_config "$ELB_JSON" NET block_size)" "$READ_BLOCK" \
    "the read direction run confirms the given block size"
assert_eq "$(json_value "$ELB_JSON" NET last_done bytes)" "$READ_EXPECTED" \
    "the read direction run transferred exactly $READ_EXPECTED bytes"
assert_eq "$(json_value_rwmix "$ELB_JSON" NET last_done bytes)" "$READ_EXPECTED" \
    "the read direction receive side reports the same total"
assert_gt "$(json_value "$ELB_JSON" NET last_done 'bytes/s')" 0 \
    "the read direction run reports a non-zero throughput"

################## The response size really dominates the read direction ##################

# In the read direction the responses make up almost all of the transferred
# data, while in the write direction it is the requests. This compares the
# response share of the total between the two runs.
WRITE_RESP_SHARE=$(( NUM_CLIENTS * NUM_THREADS * (WRITE_SIZE / WRITE_BLOCK) * WRITE_RESP ))
READ_RESP_SHARE=$(( NUM_CLIENTS * NUM_THREADS * (READ_SIZE / READ_BLOCK) * READ_RESP ))

assert_gt "$READ_RESP_SHARE" "$(( READ_EXPECTED - READ_RESP_SHARE ))" \
    "the responses dominate the total in the read direction"
assert_gt "$(( WRITE_EXPECTED - WRITE_RESP_SHARE ))" "$WRITE_RESP_SHARE" \
    "the requests dominate the total in the write direction"

################## Shut the services down ##################

netbench_quit_services
assert_ok $? "all service instances terminate on \"--quit\""
