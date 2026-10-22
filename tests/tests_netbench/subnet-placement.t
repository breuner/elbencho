#!/bin/bash
#
# Netbench stream placement by IP subnet on multi-homed hosts.
#
# The coordinator asks every service for its interfaces at the start of a run
# and pairs client and server interfaces of the same subnet only, balancing the
# streams over the servers first and then over the interfaces. Loopback has
# addresses in two subnets of different families, 127.0.0.0/24 and ::1/128,
# which stand in for two separate networks here. A client in neither of them is
# rejected before anything is prepared, unless "--netcrosssubnet" allows the
# pairing; an address family mismatch is never allowed. A client without
# "--netdevs" offers one interface per subnet, so it is paired like one that
# lists them. "--netdevsubnets" takes the subnets from the services' "--netdevs"
# entries instead of the interface netmasks; loopback cannot show a /32 address
# without root, so this is checked through the interfaces that the coordinator
# logs with "--log 1" for a server whose entry is wider than the netmask of lo.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/netbench.sh" || exit 1

NUM_THREADS=4
FILE_SIZE=$((4 * 1024 * 1024))
SEND_SIZE=$((1024 * 1024))
RECV_SIZE=4096
BOTH_SUBNETS="127.0.0.0/24,::1"
OTHER_V4="127.0.1.2"           # lo covers it as 127.0.0.0/8, which 127.0.0.0/24 does not contain
WIDE_V4="127.0.0.0/7"          # wider than the /8 netmask of lo; its network is 126.0.0.0/7

[ "$(uname -s)" = "Linux" ] || tap_skip_all "needs the Linux 127.0.0.0/8 loopback range"
grep -q "^00000000000000000000000000000001" /proc/net/if_inet6 2>/dev/null || \
    tap_skip_all "this host has no IPv6 loopback address"

test_init
tap_plan 26

# 0: server in both subnets, 1: client in both, 2: client in another IPv4 subnet,
# 3: client without "--netdevs", 4: server with IPv6 only, 5: client with IPv4 only,
# 6: server with a subnet entry wider than the netmask of lo
netbench_start_service --netdevs "$BOTH_SUBNETS" && \
    netbench_start_service --netdevs "$BOTH_SUBNETS" && \
    netbench_start_service --netdevs "$OTHER_V4" && \
    netbench_start_service && \
    netbench_start_service --netdevs "::1" && \
    netbench_start_service --netdevs "127.0.0.1" && \
    netbench_start_service --netdevs "$WIDE_V4"
if [ $? -ne 0 ]; then
    tap_bail "Unable to start 7 elbencho service instances."
fi

tap_ok "7 elbencho service instances are listening"

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

# pairs_with PAIRS PATTERN - number of connection pairs matching the pattern
pairs_with()
{
    echo "$1" | grep -c -- "$2"
}

################## Both sides in both subnets: streams stay within a subnet ##################

CLIENT_LOG="${NETBENCH_LOGS[1]}"
CLIENT_MARK="$(netbench_log_mark "$CLIENT_LOG")"

netbench_run both "$(netbench_hosts 0 1)" "$(netbench_hosts 1 1)"
assert_ok $? "netbench run between services in both subnets succeeds"

assert_eq "$(json_value "$ELB_JSON" "$NET_PHASE" last_done bytes)" "$EXPECTED" \
    "the run sent exactly $EXPECTED bytes"

PAIRS="$(netbench_log_conn_pairs_since "$CLIENT_LOG" "$CLIENT_MARK")"

assert_eq "$(echo "$PAIRS" | wc -l)" "$NUM_THREADS" \
    "the client log shows one connection per thread"
assert_eq "$(pairs_with "$PAIRS" '^127.0.0.1 127.0.0.1$')" "2" \
    "two streams run from the IPv4 address to the IPv4 endpoint"
assert_eq "$(pairs_with "$PAIRS" '^::1 ::1$')" "2" \
    "two streams run from the IPv6 address to the IPv6 endpoint"

################## A client in another subnet is rejected, unless allowed ##################

netbench_run othersubnet "$(netbench_hosts 0 1)" "$(netbench_hosts 2 1)"
assert_nok $? "a client whose only subnet no server is in is rejected"
assert_match "$(cat "$ELB_OUT")" "has no interface in a subnet of any server" \
    "the rejection names the missing common subnet"
assert_match "$(cat "$ELB_OUT")" "\"--netcrosssubnet\"" \
    "the rejection points to the option that allows the pairing"

CLIENT_LOG="${NETBENCH_LOGS[2]}"
CLIENT_MARK="$(netbench_log_mark "$CLIENT_LOG")"

netbench_run crosssubnet "$(netbench_hosts 0 1)" "$(netbench_hosts 2 1)" --netcrosssubnet
assert_ok $? "the same run succeeds with \"--netcrosssubnet\""

PAIRS="$(netbench_log_conn_pairs_since "$CLIENT_LOG" "$CLIENT_MARK")"

assert_eq "$(pairs_with "$PAIRS" "^127.0.0.1 $OTHER_V4\$")" "$NUM_THREADS" \
    "all streams run from the other subnet to the IPv4 endpoint"

################## An address family mismatch cannot be allowed ##################

netbench_run family "$(netbench_hosts 4 1)" "$(netbench_hosts 5 1)" --netcrosssubnet
assert_nok $? "an IPv4-only client is rejected by an IPv6-only server even with the option"
assert_match "$(cat "$ELB_OUT")" "has no interface in a subnet of any server" \
    "the family mismatch is reported as missing common subnet"

################## A client without "--netdevs" offers one interface per subnet ##################

CLIENT_LOG="${NETBENCH_LOGS[3]}"
CLIENT_MARK="$(netbench_log_mark "$CLIENT_LOG")"

netbench_run plainclient "$(netbench_hosts 0 1)" "$(netbench_hosts 3 1)"
assert_ok $? "netbench run with a client without \"--netdevs\" succeeds"

PAIRS="$(netbench_log_conn_pairs_since "$CLIENT_LOG" "$CLIENT_MARK")"

assert_eq "$(pairs_with "$PAIRS" '^127.0.0.1 127.0.0.1$')" "2" \
    "the plain client uses its IPv4 loopback address for the IPv4 endpoint"
assert_eq "$(pairs_with "$PAIRS" '^::1 ::1$')" "2" \
    "the plain client uses its IPv6 loopback address for the IPv6 endpoint"

################## Two servers, one of them in a single subnet: still balanced ##################

SERVER0_LOG="${NETBENCH_LOGS[0]}"
SERVER4_LOG="${NETBENCH_LOGS[4]}"
SERVER0_MARK="$(netbench_log_mark "$SERVER0_LOG")"
SERVER4_MARK="$(netbench_log_mark "$SERVER4_LOG")"
CLIENT_LOG="${NETBENCH_LOGS[1]}"
CLIENT_MARK="$(netbench_log_mark "$CLIENT_LOG")"

netbench_run twoservers "$(netbench_hosts 0 1),$(netbench_hosts 4 1)" "$(netbench_hosts 1 1)"
assert_ok $? "netbench run with a two-subnet server and an IPv6-only server succeeds"

assert_eq "$(netbench_log_count_since "$SERVER0_LOG" "$SERVER0_MARK" 'Accepted stream from')" \
    "$((NUM_THREADS / 2))" "the two-subnet server got half of the streams"
assert_eq "$(netbench_log_count_since "$SERVER4_LOG" "$SERVER4_MARK" 'Accepted stream from')" \
    "$((NUM_THREADS / 2))" "the IPv6-only server got the other half"

PAIRS="$(netbench_log_conn_pairs_since "$CLIENT_LOG" "$CLIENT_MARK")"

assert_eq "$(pairs_with "$PAIRS" '^127.0.0.1 ')" "1" \
    "the two-subnet server was reached once over IPv4 and once over IPv6"

################## Subnets from the "--netdevs" entries ##################

# The coordinator logs the interfaces every service reported, with the subnet
# that the placement uses: the interface netmask by default, the "--netdevs"
# entry with "--netdevsubnets". A client without "--netdevs" is not affected.
SERVER6="$(netbench_hosts 6 1)"
CLIENT3="$(netbench_hosts 3 1)"

netbench_run widedefault "$SERVER6" "$CLIENT3" --log 1
assert_ok $? "netbench run against a server with a subnet entry wider than its netmask succeeds"
assert_match "$(cat "$ELB_OUT")" "interfaces of $SERVER6 \[server\]: 127\.0\.0\.1 \(127\.0\.0\.0/8\)" \
    "the server's subnet is the one of its interface netmask by default"

netbench_run widedevsubnets "$SERVER6" "$CLIENT3" --log 1 --netdevsubnets
assert_ok $? "the same run with \"--netdevsubnets\" succeeds"
assert_match "$(cat "$ELB_OUT")" "interfaces of $SERVER6 \[server\]: 127\.0\.0\.1 \(126\.0\.0\.0/7\)" \
    "the server's subnet is its \"--netdevs\" entry with \"--netdevsubnets\""
assert_match "$(cat "$ELB_OUT")" "interfaces of $CLIENT3 \[client\]: 127\.0\.0\.1 \(127\.0\.0\.0/8\)" \
    "a client without \"--netdevs\" keeps the subnets of its interface netmasks"

netbench_quit_services
assert_ok $? "all service instances terminate on \"--quit\""
