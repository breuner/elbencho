#!/bin/bash
#
# Netbench mode with a single client connecting to two servers.
#
# Three elbencho service instances are started on localhost. The coordinator
# declares the first two as servers and the last one as the client, which is the
# fan-out direction of the client to server mapping: the client's threads get
# connected round-robin to the servers.
#
# The thread count is a multiple of the server count on purpose, so that the
# connections divide evenly and the expected number per server is unambiguous.
# That the threads really did fan out across both servers is verified from the
# service logs, because the result files have no per-host breakdown.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/netbench.sh" || exit 1

NUM_SERVERS=2
NUM_CLIENTS=1
NUM_THREADS=4                   # a multiple of NUM_SERVERS, see above
FILE_SIZE=$((8 * 1024 * 1024))
BLOCK_SIZE=$((1024 * 1024))
RESP_SIZE=4096

NUM_SERVICES=$((NUM_SERVERS + NUM_CLIENTS))
CONNS_PER_SERVER=$(( (NUM_CLIENTS * NUM_THREADS) / NUM_SERVERS ))

test_init
tap_plan 18

netbench_start_services $NUM_SERVICES
if [ $? -ne 0 ]; then
    tap_bail "Unable to start $NUM_SERVICES elbencho service instances."
fi

tap_ok "$NUM_SERVICES elbencho service instances are listening"

netbench_data_ports_free || tap_skip_all "a netbench data port is used by another process"

SERVERS="$(netbench_hosts 0 $NUM_SERVERS)"
CLIENTS="$(netbench_hosts $NUM_SERVERS $NUM_CLIENTS)"
SERVER0_LOG="${NETBENCH_LOGS[0]}"
SERVER1_LOG="${NETBENCH_LOGS[1]}"
CLIENT_LOG="${NETBENCH_LOGS[2]}"

EXPECTED_BYTES="$(netbench_expected_bytes \
    $NUM_CLIENTS $NUM_THREADS $FILE_SIZE $BLOCK_SIZE $RESP_SIZE)"

# The two data ports that the client's threads are expected to connect to.
EXPECTED_TARGETS="$(printf '%s\n%s\n' \
    "$(netbench_data_port 0)" "$(netbench_data_port 1)" | \
    sort -u | tr '\n' ' ' | sed -e 's/ $//')"

################## Run the benchmark ##################

SERVER0_MARK="$(netbench_log_mark "$SERVER0_LOG")"
SERVER1_MARK="$(netbench_log_mark "$SERVER1_LOG")"
CLIENT_MARK="$(netbench_log_mark "$CLIENT_LOG")"

run_elbencho netbench \
    --netbench \
    --servers "$SERVERS" --clients "$CLIENTS" \
    -s $FILE_SIZE -b $BLOCK_SIZE --respsize $RESP_SIZE -t $NUM_THREADS
assert_ok $? "netbench run with $NUM_CLIENTS client and $NUM_SERVERS servers succeeds"

################## The result ##################

assert_eq "$(json_phases "$ELB_JSON")" "NET" \
    "the netbench phase is the only phase in the result"
assert_eq "$(json_config "$ELB_JSON" NET path_type)" "net" \
    "the result reports \"net\" as benchmark path type"
assert_eq "$(json_config "$ELB_JSON" NET hosts)" "$NUM_SERVICES" \
    "the result reports all $NUM_SERVICES services, servers and clients"
assert_eq "$(json_config "$ELB_JSON" NET threads)" "$NUM_THREADS" \
    "the result confirms $NUM_THREADS threads per service"

assert_eq "$(json_value "$ELB_JSON" NET last_done bytes)" "$EXPECTED_BYTES" \
    "the run transferred exactly $EXPECTED_BYTES bytes"
assert_eq "$(json_value_rwmix "$ELB_JSON" NET last_done bytes)" "$EXPECTED_BYTES" \
    "the receive side reports the same $EXPECTED_BYTES bytes as the send side"
assert_eq "$(json_value "$ELB_JSON" NET last_done entries)" "0" \
    "netbench mode reports no entries"
assert_gt "$(json_value "$ELB_JSON" NET last_done 'bytes/s')" 0 \
    "the run reports a non-zero throughput"

################## The client threads fanned out across both servers ##################

assert_eq "$(netbench_log_count_since "$SERVER0_LOG" "$SERVER0_MARK" 'Accepted new connection')" \
    "$CONNS_PER_SERVER" \
    "the first server accepted $CONNS_PER_SERVER of the client's connections"
assert_eq "$(netbench_log_count_since "$SERVER1_LOG" "$SERVER1_MARK" 'Accepted new connection')" \
    "$CONNS_PER_SERVER" \
    "the second server accepted $CONNS_PER_SERVER of the client's connections"

# This proves both the round-robin assignment and that the data port really is
# the service port plus the fixed offset, because the expected ports are
# computed here rather than read from elbencho.
assert_eq "$(netbench_log_conn_targets_since "$CLIENT_LOG" "$CLIENT_MARK")" \
    "$EXPECTED_TARGETS" \
    "the client's threads connected to the data ports of both servers"

################## The same mapping at the minimum even split ##################

# One connection per server, i.e. the smallest thread count that still reaches
# every server.
SERVER0_MARK="$(netbench_log_mark "$SERVER0_LOG")"
SERVER1_MARK="$(netbench_log_mark "$SERVER1_LOG")"

run_elbencho minthreads \
    --netbench \
    --servers "$SERVERS" --clients "$CLIENTS" \
    -s $FILE_SIZE -b $BLOCK_SIZE --respsize $RESP_SIZE -t $NUM_SERVERS
assert_ok $? "netbench run with one thread per server succeeds"

assert_eq "$(json_value "$ELB_JSON" NET last_done bytes)" \
    "$(netbench_expected_bytes $NUM_CLIENTS $NUM_SERVERS $FILE_SIZE $BLOCK_SIZE $RESP_SIZE)" \
    "the run with one thread per server transferred the expected bytes"

assert_eq "$(netbench_log_count_since "$SERVER0_LOG" "$SERVER0_MARK" 'Accepted new connection')" \
    "1" "the first server accepted exactly one connection"
assert_eq "$(netbench_log_count_since "$SERVER1_LOG" "$SERVER1_MARK" 'Accepted new connection')" \
    "1" "the second server accepted exactly one connection"

################## Shut the services down ##################

netbench_quit_services
assert_ok $? "all service instances terminate on \"--quit\""
