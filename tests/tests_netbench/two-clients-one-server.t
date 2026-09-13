#!/bin/bash
#
# Netbench mode with two clients connecting to a single server.
#
# Three elbencho service instances are started on localhost. The coordinator
# declares the first one as the server and the other two as clients, which is
# the fan-in direction of the client to server mapping.
#
# The reported byte total is asserted exactly. The coordinator sums the counters
# of the servers and of the clients, and because each side counts a different
# direction, both the send and the receive total come out as
# "clients * threads * (size / blocksize) * (blocksize + respsize)". A premature
# disconnect of a data connection is not reported as an error by elbencho, it
# only results in fewer transferred bytes, so this exact comparison is the only
# thing that would catch it.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/netbench.sh" || exit 1

NUM_SERVERS=1
NUM_CLIENTS=2
NUM_THREADS=1
FILE_SIZE=$((16 * 1024 * 1024))
BLOCK_SIZE=$((1024 * 1024))
RESP_SIZE=1                     # the default, kept small on purpose (see below)

NUM_SERVICES=$((NUM_SERVERS + NUM_CLIENTS))
NUM_BLOCKS=$((FILE_SIZE / BLOCK_SIZE))
EXPECTED_CONNS=$((NUM_CLIENTS * NUM_THREADS))

test_init
tap_plan 17

netbench_start_services $NUM_SERVICES
if [ $? -ne 0 ]; then
    tap_bail "Unable to start $NUM_SERVICES elbencho service instances."
fi

tap_ok "$NUM_SERVICES elbencho service instances are listening"

netbench_data_ports_free || tap_skip_all "a netbench data port is used by another process"

SERVERS="$(netbench_hosts 0 $NUM_SERVERS)"
CLIENTS="$(netbench_hosts $NUM_SERVERS $NUM_CLIENTS)"
SERVER_LOG="${NETBENCH_LOGS[0]}"

EXPECTED_BYTES="$(netbench_expected_bytes \
    $NUM_CLIENTS $NUM_THREADS $FILE_SIZE $BLOCK_SIZE $RESP_SIZE)"

################## Run the benchmark ##################

SERVER_MARK="$(netbench_log_mark "$SERVER_LOG")"

# No benchmark path and no write phase option are given: netbench mode needs no
# path and always runs the write phase.
run_elbencho netbench \
    --netbench \
    --servers "$SERVERS" --clients "$CLIENTS" \
    -s $FILE_SIZE -b $BLOCK_SIZE --respsize $RESP_SIZE -t $NUM_THREADS
assert_ok $? "netbench run with $NUM_CLIENTS clients and $NUM_SERVERS server succeeds"

################## The result ##################

assert_eq "$(json_phases "$ELB_JSON")" "NET" \
    "the netbench phase is the only phase in the result"
assert_eq "$(json_config "$ELB_JSON" NET path_type)" "net" \
    "the result reports \"net\" as benchmark path type"
assert_eq "$(json_config "$ELB_JSON" NET hosts)" "$NUM_SERVICES" \
    "the result reports all $NUM_SERVICES services, servers and clients"
assert_eq "$(json_config "$ELB_JSON" NET threads)" "$NUM_THREADS" \
    "the result confirms $NUM_THREADS thread per service"
assert_eq "$(json_config "$ELB_JSON" NET file_size)" "$FILE_SIZE" \
    "the result confirms the given transfer size per client thread"
assert_eq "$(json_config "$ELB_JSON" NET block_size)" "$BLOCK_SIZE" \
    "the result confirms the given block size"

# Netbench mode forces these two, which is a fingerprint of the mode.
assert_eq "$(json_config "$ELB_JSON" NET dirs)" "0" \
    "netbench mode reports zero dirs"
assert_eq "$(json_config "$ELB_JSON" NET files)" "1" \
    "netbench mode reports one file"

assert_eq "$(json_value "$ELB_JSON" NET last_done bytes)" "$EXPECTED_BYTES" \
    "the run transferred exactly $EXPECTED_BYTES bytes"
assert_eq "$(json_value_rwmix "$ELB_JSON" NET last_done bytes)" "$EXPECTED_BYTES" \
    "the receive side reports the same $EXPECTED_BYTES bytes as the send side"

# With a response size of 1 byte the response share of the total is exactly the
# number of blocks, so this shows that the server side counters really are part
# of the aggregate rather than the clients being counted alone.
assert_eq "$(( EXPECTED_BYTES - NUM_CLIENTS * NUM_THREADS * FILE_SIZE ))" \
    "$((NUM_CLIENTS * NUM_THREADS * NUM_BLOCKS * RESP_SIZE))" \
    "the response bytes of the server are included in the total"

# Entry counters are not used in netbench mode, only byte counters.
assert_eq "$(json_value "$ELB_JSON" NET last_done entries)" "0" \
    "netbench mode reports no entries"

assert_gt "$(json_value "$ELB_JSON" NET last_done 'bytes/s')" 0 \
    "the run reports a non-zero throughput"

################## The single server saw both clients ##################

assert_eq "$(netbench_log_count_since "$SERVER_LOG" "$SERVER_MARK" 'Accepted new connection')" \
    "$EXPECTED_CONNS" \
    "the server accepted one connection per client thread, $EXPECTED_CONNS in total"

################## Shut the services down ##################

netbench_quit_services
assert_ok $? "all service instances terminate on \"--quit\""
