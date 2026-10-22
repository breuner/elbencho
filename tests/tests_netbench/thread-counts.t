#!/bin/bash
#
# Netbench mode with different thread counts for servers and clients.
#
# "-t" is the number of threads of every service instance. "--tservers" and
# "--tclients" let fewer of them take part in the benchmark, the remaining
# threads finish the phase immediately. Here one server with 4 threads serves
# two clients with 1 active thread each, so only 2 of the 4 server endpoints
# get a stream. The run is repeated with an explicit "-t" and once with "-t"
# alone to show that the default behavior is unchanged.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/netbench.sh" || exit 1

NUM_SERVERS=1
NUM_CLIENTS=2
NUM_THREADS=4                   # threads per service ("-t")
CLIENT_THREADS=1                # active threads per client ("--tclients")
FILE_SIZE=$((16 * 1024 * 1024))
SEND_SIZE=$((1024 * 1024))
RECV_SIZE=1

NUM_SERVICES=$((NUM_SERVERS + NUM_CLIENTS))

test_init
tap_plan 16

netbench_start_services $NUM_SERVICES
if [ $? -ne 0 ]; then
    tap_bail "Unable to start $NUM_SERVICES elbencho service instances."
fi

tap_ok "$NUM_SERVICES elbencho service instances are listening"

SERVERS="$(netbench_hosts 0 $NUM_SERVERS)"
CLIENTS="$(netbench_hosts $NUM_SERVERS $NUM_CLIENTS)"
SERVER_LOG="${NETBENCH_LOGS[0]}"
CLIENT0_LOG="${NETBENCH_LOGS[1]}"
CLIENT1_LOG="${NETBENCH_LOGS[2]}"

EXPECTED_BYTES_REDUCED="$(netbench_expected_send_bytes \
    $NUM_CLIENTS $CLIENT_THREADS $FILE_SIZE $SEND_SIZE $RECV_SIZE)"
EXPECTED_BYTES_REDUCED_RECV="$(netbench_expected_recv_bytes \
    $NUM_CLIENTS $CLIENT_THREADS $FILE_SIZE $SEND_SIZE $RECV_SIZE)"
EXPECTED_BYTES_FULL="$(netbench_expected_send_bytes \
    $NUM_CLIENTS $NUM_THREADS $FILE_SIZE $SEND_SIZE $RECV_SIZE)"
EXPECTED_BYTES_FULL_RECV="$(netbench_expected_recv_bytes \
    $NUM_CLIENTS $NUM_THREADS $FILE_SIZE $SEND_SIZE $RECV_SIZE)"

################## Implicit "-t" from "--tservers" and "--tclients" ##################

SERVER_MARK="$(netbench_log_mark "$SERVER_LOG")"
CLIENT0_MARK="$(netbench_log_mark "$CLIENT0_LOG")"
CLIENT1_MARK="$(netbench_log_mark "$CLIENT1_LOG")"

run_elbencho implicit-threads \
    --netbench \
    --servers "$SERVERS" --clients "$CLIENTS" \
    -s $FILE_SIZE --sendsize $SEND_SIZE --recvsize $RECV_SIZE \
    --tservers $NUM_THREADS --tclients $CLIENT_THREADS
assert_ok $? "netbench run with --tservers $NUM_THREADS --tclients $CLIENT_THREADS succeeds"

assert_eq "$(json_config "$ELB_JSON" "$NET_PHASE" threads)" "$NUM_THREADS" \
    "\"-t\" is implicitly the larger of --tservers and --tclients"
assert_eq "$(json_value "$ELB_JSON" "$NET_PHASE" last_done bytes)" "$EXPECTED_BYTES_REDUCED" \
    "the run transferred the bytes of $CLIENT_THREADS stream per client"
assert_eq "$(json_value_rwmix "$ELB_JSON" "$NET_PHASE" last_done bytes)" "$EXPECTED_BYTES_REDUCED_RECV" \
    "the receive side reports the same bytes as the send side"

assert_eq "$(netbench_log_count_since "$SERVER_LOG" "$SERVER_MARK" 'endpoint ready')" \
    "$NUM_THREADS" \
    "the server opened one endpoint per server thread, $NUM_THREADS in total"
assert_eq "$(netbench_log_count_since "$SERVER_LOG" "$SERVER_MARK" 'Accepted stream from')" \
    "$((NUM_CLIENTS * CLIENT_THREADS))" \
    "the server accepted one stream per active client thread"
assert_eq "$(netbench_log_count_since "$CLIENT0_LOG" "$CLIENT0_MARK" 'Established connection')" \
    "$CLIENT_THREADS" \
    "the first client established $CLIENT_THREADS connection"
assert_eq "$(netbench_log_count_since "$CLIENT1_LOG" "$CLIENT1_MARK" 'Established connection')" \
    "$CLIENT_THREADS" \
    "the second client established $CLIENT_THREADS connection"

################## Explicit "-t" with reduced client threads ##################

SERVER_MARK="$(netbench_log_mark "$SERVER_LOG")"

run_elbencho explicit-threads \
    --netbench \
    --servers "$SERVERS" --clients "$CLIENTS" \
    -s $FILE_SIZE --sendsize $SEND_SIZE --recvsize $RECV_SIZE \
    -t $NUM_THREADS --tclients $CLIENT_THREADS
assert_ok $? "netbench run with -t $NUM_THREADS --tclients $CLIENT_THREADS succeeds"

assert_eq "$(json_value "$ELB_JSON" "$NET_PHASE" last_done bytes)" "$EXPECTED_BYTES_REDUCED" \
    "the run with explicit \"-t\" transferred the same bytes"
assert_eq "$(netbench_log_count_since "$SERVER_LOG" "$SERVER_MARK" 'Accepted stream from')" \
    "$((NUM_CLIENTS * CLIENT_THREADS))" \
    "the server again accepted one stream per active client thread"

################## "-t" alone: all threads take part ##################

SERVER_MARK="$(netbench_log_mark "$SERVER_LOG")"

run_elbencho all-threads \
    --netbench \
    --servers "$SERVERS" --clients "$CLIENTS" \
    -s $FILE_SIZE --sendsize $SEND_SIZE --recvsize $RECV_SIZE \
    -t $NUM_THREADS
assert_ok $? "netbench run with -t $NUM_THREADS alone succeeds"

assert_eq "$(json_value "$ELB_JSON" "$NET_PHASE" last_done bytes)" "$EXPECTED_BYTES_FULL" \
    "without the thread options all $NUM_THREADS client threads run streams"
assert_eq "$(netbench_log_count_since "$SERVER_LOG" "$SERVER_MARK" 'Accepted stream from')" \
    "$((NUM_CLIENTS * NUM_THREADS))" \
    "the server accepted one stream per client thread"

################## Shut the services down ##################

netbench_quit_services
assert_ok $? "all service instances terminate on \"--quit\""
