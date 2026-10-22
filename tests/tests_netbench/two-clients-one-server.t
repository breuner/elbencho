#!/bin/bash
#
# Netbench mode with two clients connecting to a single server.
#
# Three elbencho service instances are started on localhost. The coordinator
# declares the first one as the server and the other two as clients, which is
# the fan-in direction of the client to server mapping.
#
# The reported bytes are asserted exactly. The coordinator counts the clients
# only (the servers see the same bytes from the other side), so the sent bytes
# are "clients * threads * (size / sendsize) * sendsize" and the received bytes
# are the servers' responses, "clients * threads * (size / sendsize) * recvsize".
#
# A second run with "--svcminmax" shows that netbench reports the slowest and
# fastest clients and servers as separate groups.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/netbench.sh" || exit 1

NUM_SERVERS=1
NUM_CLIENTS=2
NUM_THREADS=1
FILE_SIZE=$((16 * 1024 * 1024))
SEND_SIZE=$((1024 * 1024))
RECV_SIZE=1                     # the default, kept small on purpose (see below)

NUM_SERVICES=$((NUM_SERVERS + NUM_CLIENTS))
NUM_ROUNDS=$((FILE_SIZE / SEND_SIZE))
EXPECTED_CONNS=$((NUM_CLIENTS * NUM_THREADS))

test_init
tap_plan 26

netbench_start_services $NUM_SERVICES
if [ $? -ne 0 ]; then
    tap_bail "Unable to start $NUM_SERVICES elbencho service instances."
fi

tap_ok "$NUM_SERVICES elbencho service instances are listening"

SERVERS="$(netbench_hosts 0 $NUM_SERVERS)"
CLIENTS="$(netbench_hosts $NUM_SERVERS $NUM_CLIENTS)"
SERVER_LOG="${NETBENCH_LOGS[0]}"

EXPECTED_BYTES="$(netbench_expected_send_bytes \
    $NUM_CLIENTS $NUM_THREADS $FILE_SIZE $SEND_SIZE $RECV_SIZE)"
EXPECTED_BYTES_RECV="$(netbench_expected_recv_bytes \
    $NUM_CLIENTS $NUM_THREADS $FILE_SIZE $SEND_SIZE $RECV_SIZE)"

################## Run the benchmark ##################

SERVER_MARK="$(netbench_log_mark "$SERVER_LOG")"

# No benchmark path and no write phase option are given: netbench mode needs no
# path and always runs the write phase.
run_elbencho netbench \
    --netbench \
    --servers "$SERVERS" --clients "$CLIENTS" \
    -s $FILE_SIZE --sendsize $SEND_SIZE --recvsize $RECV_SIZE -t $NUM_THREADS
assert_ok $? "netbench run with $NUM_CLIENTS clients and $NUM_SERVERS server succeeds"

################## The result ##################

assert_eq "$(json_phases "$ELB_JSON")" "$NET_PHASE" \
    "the netbench phase is the only phase in the result"
assert_eq "$(json_config "$ELB_JSON" "$NET_PHASE" path_type)" "net" \
    "the result reports \"net\" as benchmark path type"
assert_eq "$(json_config "$ELB_JSON" "$NET_PHASE" hosts)" "$NUM_SERVICES" \
    "the result reports all $NUM_SERVICES services, servers and clients"
assert_eq "$(json_config "$ELB_JSON" "$NET_PHASE" threads)" "$NUM_THREADS" \
    "the result confirms $NUM_THREADS thread per service"
assert_eq "$(json_config "$ELB_JSON" "$NET_PHASE" file_size)" "$FILE_SIZE" \
    "the result confirms the given transfer size per client thread"
assert_eq "$(json_config "$ELB_JSON" "$NET_PHASE" block_size)" "$SEND_SIZE" \
    "the result confirms the given send size as block size"

# Netbench mode forces these two, which is a fingerprint of the mode.
assert_eq "$(json_config "$ELB_JSON" "$NET_PHASE" dirs)" "0" \
    "netbench mode reports zero dirs"
assert_eq "$(json_config "$ELB_JSON" "$NET_PHASE" files)" "1" \
    "netbench mode reports one file"

assert_eq "$(json_value "$ELB_JSON" "$NET_PHASE" last_done bytes)" "$EXPECTED_BYTES" \
    "the run sent exactly $EXPECTED_BYTES bytes"
assert_eq "$(json_value_rwmix "$ELB_JSON" "$NET_PHASE" last_done bytes)" "$EXPECTED_BYTES_RECV" \
    "the receive side reports the received bytes"

# With a receive size of 1 byte the received bytes are exactly the number of
# rounds, which shows that the responses of the server are what the clients
# count on their receive side.
assert_eq "$EXPECTED_BYTES_RECV" "$((NUM_CLIENTS * NUM_THREADS * NUM_ROUNDS * RECV_SIZE))" \
    "the received bytes are the responses of the server, one per round"

# Entry counters are not used in netbench mode, only byte counters.
assert_eq "$(json_value "$ELB_JSON" "$NET_PHASE" last_done entries)" "0" \
    "netbench mode reports no entries"

assert_gt "$(json_value "$ELB_JSON" "$NET_PHASE" last_done 'bytes/s')" 0 \
    "the run reports a non-zero throughput"

################## The single server saw both clients ##################

assert_eq "$(netbench_log_count_since "$SERVER_LOG" "$SERVER_MARK" 'Accepted stream from')" \
    "$EXPECTED_CONNS" \
    "the server accepted one stream per client thread, $EXPECTED_CONNS in total"

################## Per-service min/avg/max for clients and servers ##################

run_elbencho svcminmax \
    --netbench --svcminmax \
    --servers "$SERVERS" --clients "$CLIENTS" \
    -s $FILE_SIZE --sendsize $SEND_SIZE --recvsize $RECV_SIZE -t $NUM_THREADS
assert_ok $? "netbench run with \"--svcminmax\" succeeds"

for ROW in "Clients IOPS" "Clients MiB/s" "Servers IOPS" "Servers MiB/s"; do
    assert_eq "$(res_row_count "$ELB_RES" "$ROW")" "1" \
        "the result table has one \"$ROW\" row"
done

CLIENT_MIN_HOST="$(jq -r --arg p "$NET_PHASE" 'select(.phase_type == $p) | .last_done.clients.iops.min_host' \
    "$ELB_JSON" | head -1)"
CLIENT_MAX_HOST="$(jq -r --arg p "$NET_PHASE" 'select(.phase_type == $p) | .last_done.clients.iops.max_host' \
    "$ELB_JSON" | head -1)"
SERVER_MIN_HOST="$(jq -r --arg p "$NET_PHASE" 'select(.phase_type == $p) | .last_done.servers.iops.min_host' \
    "$ELB_JSON" | head -1)"
CLIENT_PORTS="$(echo "$CLIENTS" | sed -e 's/localhost://g' -e 's/,/|/g')"

assert_match "$CLIENT_MIN_HOST" "^localhost:($CLIENT_PORTS)\$" \
    "the slowest client is one of the client services"
assert_match "$CLIENT_MAX_HOST" "^localhost:($CLIENT_PORTS)\$" \
    "the fastest client is one of the client services"
assert_eq "$SERVER_MIN_HOST" "$SERVERS" \
    "the server group contains the server service"
assert_eq "$(json_has_key "$ELB_JSON" "$NET_PHASE" last_done services)" "false" \
    "netbench mode has no combined services subtree"

################## Shut the services down ##################

netbench_quit_services
assert_ok $? "all service instances terminate on \"--quit\""
