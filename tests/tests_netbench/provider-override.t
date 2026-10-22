#!/bin/bash
#
# Netbench mode with an explicitly selected libfabric provider.
#
# "--netprov" names the provider instead of the tcp default or the automatic
# RDMA choice. The tcp provider is part of every libfabric build, so it is the
# one selected explicitly here; the client log shows which provider the
# endpoints ended up on.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/netbench.sh" || exit 1

require_build_feature libfabric

NET_PHASE="NET/libfabric"

NUM_THREADS=2
FILE_SIZE=$((4 * 1024 * 1024))
SEND_SIZE=$((1024 * 1024))
RECV_SIZE=4096

test_init
tap_plan 5

netbench_start_services 2
if [ $? -ne 0 ]; then
    tap_bail "Unable to start 2 elbencho service instances."
fi

tap_ok "2 elbencho service instances are listening"

CLIENT_LOG="${NETBENCH_LOGS[1]}"
CLIENT_MARK="$(netbench_log_mark "$CLIENT_LOG")"

run_elbencho provtcp \
    --netbench --netprov tcp \
    --servers "$(netbench_hosts 0 1)" --clients "$(netbench_hosts 1 1)" \
    -s $FILE_SIZE --sendsize $SEND_SIZE --recvsize $RECV_SIZE -t $NUM_THREADS
assert_ok $? "netbench run with the explicitly selected tcp provider succeeds"

EXPECTED="$(netbench_expected_send_bytes 1 $NUM_THREADS $FILE_SIZE $SEND_SIZE $RECV_SIZE)"
EXPECTED_RECV="$(netbench_expected_recv_bytes 1 $NUM_THREADS $FILE_SIZE $SEND_SIZE $RECV_SIZE)"

assert_eq "$(json_value "$ELB_JSON" "$NET_PHASE" last_done bytes)" "$EXPECTED" \
    "the run sent exactly $EXPECTED bytes"
assert_eq "$(netbench_log_count_since "$CLIENT_LOG" "$CLIENT_MARK" 'endpoint: tcp')" \
    "$NUM_THREADS" "every client thread connected to a tcp provider endpoint"

netbench_quit_services
assert_ok $? "all service instances terminate on \"--quit\""
