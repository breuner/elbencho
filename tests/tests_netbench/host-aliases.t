#!/bin/bash
#
# Netbench host aliases and hostname labels.
#
# The interface query at the start of a run brings a per-process service ID and
# the hostname of every service. A host entry that reaches a service already
# listed under another address is an alias and is left out, with a NOTE and
# the corrected server and client counts at the normal log level and the alias
# pairs only with "--log 1". A service listed as server and as client is a
# client alias of itself and leaves the run without a client, which is an
# error. Literal duplicates (the same "host:port" twice) are rejected as
# before. Services given as IP address show as "address:port (hostname)" in
# the coordinator's per-service output and in the client logs; services given
# by name are shown as given. One service reached as "localhost:P" and as
# "127.0.0.1:P" reproduces all of this on any host.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/netbench.sh" || exit 1

NUM_THREADS=2
FILE_SIZE=$((4 * 1024 * 1024))
SEND_SIZE=$((1024 * 1024))
RECV_SIZE=4096

# the services take their hostname from these scheduler variables before the
# kernel hostname, so the label check needs them out of the way
unset PMI_HOSTNAME MPIR_CVAR_CH3_INTERFACE_HOSTNAME SLURMD_NODENAME PMIX_HOSTNAME

HOSTNAME_LABEL="$(hostname)"
[ -n "$HOSTNAME_LABEL" ] || tap_skip_all "unable to determine the hostname"

test_init
tap_plan 19

netbench_start_services 2
if [ $? -ne 0 ]; then
    tap_bail "Unable to start 2 elbencho service instances."
fi

tap_ok "2 elbencho service instances are listening"

SERVER_PORT="${NETBENCH_PORTS[0]}"
CLIENT_PORT="${NETBENCH_PORTS[1]}"
SERVER_LOG="${NETBENCH_LOGS[0]}"
CLIENT_LOG="${NETBENCH_LOGS[1]}"
EXPECTED="$(netbench_expected_send_bytes 1 $NUM_THREADS $FILE_SIZE $SEND_SIZE $RECV_SIZE)"

# netbench_run TAG SERVERS CLIENTS [OPTIONS...]
netbench_run()
{
    local tag="$1" servers="$2" clients="$3"
    shift 3

    run_elbencho "$tag" \
        --netbench --servers "$servers" --clients "$clients" \
        -s $FILE_SIZE --sendsize $SEND_SIZE --recvsize $RECV_SIZE -t $NUM_THREADS "$@"
}

################## Both services listed under two addresses each ##################

SERVER_MARK="$(netbench_log_mark "$SERVER_LOG")"

netbench_run aliases "localhost:$SERVER_PORT,127.0.0.1:$SERVER_PORT" \
    "localhost:$CLIENT_PORT,127.0.0.1:$CLIENT_PORT" --svcelapsed
assert_ok $? "netbench run with both services listed under two addresses each succeeds"
assert_match "$(cat "$ELB_OUT")" \
    "NOTE: 2 of the given netbench hosts are aliases .* Servers: 1; clients: 1\." \
    "the NOTE gives the number of aliases and the corrected server and client counts"
assert_eq "$(grep -c "is the same service as" "$ELB_OUT")" "0" \
    "the alias pairs are not shown at the normal log level"
assert_eq "$(json_value "$ELB_JSON" "$NET_PHASE" last_done bytes)" "$EXPECTED" \
    "the run sent the bytes of one client only"
assert_eq "$(netbench_log_count_since "$SERVER_LOG" "$SERVER_MARK" 'Accepted stream from')" \
    "$NUM_THREADS" "the server accepted the streams of one client only"
assert_eq "$(grep -c "localhost:$SERVER_PORT (" "$ELB_OUT")" "0" \
    "a service given by name gets no hostname label"

################## The alias pairs with verbose logging ##################

netbench_run aliasesverbose "localhost:$SERVER_PORT,127.0.0.1:$SERVER_PORT" \
    "localhost:$CLIENT_PORT,127.0.0.1:$CLIENT_PORT" --log 1
assert_ok $? "the same run with \"--log 1\" succeeds"
assert_match "$(cat "$ELB_OUT")" \
    "alias: 127\.0\.0\.1:$SERVER_PORT \[server\] is the same service as localhost:$SERVER_PORT \[server\]" \
    "the server alias is named with verbose logging"
assert_match "$(cat "$ELB_OUT")" \
    "alias: 127\.0\.0\.1:$CLIENT_PORT \[client\] is the same service as localhost:$CLIENT_PORT \[client\]" \
    "the client alias is named with verbose logging"

################## A server that is its own client leaves no client ##################

netbench_run selfclient "localhost:$SERVER_PORT" "127.0.0.1:$SERVER_PORT"
assert_nok $? "a run whose only client is an alias of the server is rejected"
assert_match "$(cat "$ELB_OUT")" "at least one server and one client after leaving out the aliases" \
    "the rejection names the missing client"

################## Literal duplicates are still rejected ##################

netbench_run duplicates "localhost:$SERVER_PORT,localhost:$SERVER_PORT" "localhost:$CLIENT_PORT"
assert_nok $? "the same \"host:port\" twice is rejected"
assert_match "$(cat "$ELB_OUT")" "List of hosts contains duplicates" \
    "the rejection reports the duplicates"

################## Hostname labels for services given as IP address ##################

CLIENT_MARK="$(netbench_log_mark "$CLIENT_LOG")"

netbench_run labels "127.0.0.1:$SERVER_PORT" "127.0.0.1:$CLIENT_PORT" --svcelapsed
assert_ok $? "netbench run with services given as IP address succeeds"
assert_match "$(cat "$ELB_OUT")" "127\.0\.0\.1:$SERVER_PORT \($HOSTNAME_LABEL\)=" \
    "the per-service completion times label the server with its hostname"
assert_match "$(cat "$ELB_OUT")" "127\.0\.0\.1:$CLIENT_PORT \($HOSTNAME_LABEL\)=" \
    "the per-service completion times label the client with its hostname"
assert_eq "$(netbench_log_count_since "$CLIENT_LOG" "$CLIENT_MARK" \
    "Established connection to: 127.0.0.1:$SERVER_PORT ($HOSTNAME_LABEL);")" "$NUM_THREADS" \
    "the client log names the server with its hostname"

netbench_quit_services
assert_ok $? "all service instances terminate on \"--quit\""
