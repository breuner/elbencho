#!/bin/bash
#
# Argument checks of netbench mode.
#
# All of these are rejected while the arguments are being checked, i.e. before
# any service instance is contacted, so this test needs no services at all and
# runs in milliseconds. It is the cheapest regression guard for the netbench
# option surface. The provider listing of "--netprov list" is covered here as well,
# because it also ends before any service is contacted.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1


test_init
tap_plan 26

# A host pair that is never actually contacted, because every run below fails
# before that point.
SERVER="localhost:20001"
CLIENT="localhost:20002"

# check_rejected TAG EXPECTED_MESSAGE ARGS...
check_rejected()
{
    local tag="$1"
    local expected="$2"
    shift 2

    run_elbencho "$tag" --netbench "$@" > /dev/null 2>&1

    if [ $? -eq 0 ]; then
        tap_fail "$tag is rejected"
        tap_diag "  the run unexpectedly succeeded"
        return
    fi

    assert_match "$(cat "$ELB_OUT")" "$expected" "$tag is rejected as expected"
}

################## Missing servers and clients ##################

check_rejected "netbench mode without any hosts" \
    'Missing servers & clients definition for netbench mode\.'

check_rejected "netbench mode with servers but no clients" \
    'At least one client needs to be defined for netbench mode\.' \
    --servers "$SERVER"

check_rejected "netbench mode with clients but no servers" \
    'At least one server needs to be defined for netbench mode\.' \
    --clients "$CLIENT"

# "--hosts" is not read at all in netbench mode, so it does not satisfy the
# servers and clients requirement.
check_rejected "netbench mode with \"--hosts\" instead of servers and clients" \
    'Missing servers & clients definition for netbench mode\.' \
    --hosts "$SERVER"

# The same service cannot act as server and client at the same time.
check_rejected "the same host as server and client" \
    'List of hosts contains duplicates' \
    --servers "$SERVER" --clients "$SERVER"

################## Sizes ##################

# Either the send size or the receive size may be zero (one-directional
# stream), but not both. The file size has a default in netbench mode, so only
# an explicit zero can be wrong.
check_rejected "a send size and receive size of zero" \
    'Send size and receive size must not both be zero in netbench mode\.' \
    --servers "$SERVER" --clients "$CLIENT" --sendsize 0 --recvsize 0
check_rejected "a file size of zero" \
    'File size must not be zero in netbench mode\.' \
    --servers "$SERVER" --clients "$CLIENT" -s 0
check_rejected "an IO depth of zero" \
    'IO depth must not be zero in netbench mode\.' \
    --servers "$SERVER" --clients "$CLIENT" --iodepth 0

# "--sendsize" is the netbench name of "-b", so both together must agree.
check_rejected "a send size that contradicts the block size" \
    'are the same option in netbench mode, but were given with different values' \
    --servers "$SERVER" --clients "$CLIENT" -b 1k --sendsize 2k

# "--netmemmax" limits the send and receive buffers of a client and of a server.
# A server needs (client threads per server) x "--iodepth" x "--sendsize" plus
# one "--recvsize" buffer per endpoint; a client "--iodepth" x ("--sendsize" +
# "--recvsize") per thread (ping-pong streams have one receive buffer per round
# in flight). Here: 3 clients x 8 threads / 2 servers = 12 streams x 16 x 1 MiB
# + 8 x 1 B on the server, 8 x 16 x (1 MiB + 1 B) on the client.
check_rejected "a buffer memory limit below the need" \
    '192MB per server \(12 streams x 16 receive buffers x send size 1024KB \+ 8 endpoints x receive size 1B\) and 128MB per client' \
    --servers "$SERVER,localhost:20003" --clients "$CLIENT,localhost:20004,localhost:20005" \
    -t 8 --iodepth 16 --sendsize 1m --netmemmax 191m
# Streams per server are rounded up: 3 clients x 3 threads / 2 servers = 5.
check_rejected "a buffer memory limit with rounded-up streams per server" \
    '5120KB per server \(5 streams x 1 receive buffers x send size 1024KB' \
    --servers "$SERVER,localhost:20003" --clients "$CLIENT,localhost:20004,localhost:20005" \
    -t 3 --sendsize 1m --netmemmax 4m
# The default limit of 1 GiB catches big client buffers as well.
check_rejected "the default buffer memory limit on the client side" \
    '2048MB per client \(16 threads x \(IO depth 64 x send size 1024KB \+ 64 receive buffers x receive size 1024KB\)\), which exceeds "--netmemmax" \(1024MB\)' \
    --servers "$SERVER" --clients "$CLIENT" -t 16 --iodepth 64 --sendsize 1m --recvsize 1m
# One-directional streams post "--netrecvpct" percent of the sender's IO depth
# as receive buffers (rounded up): 300% of 1 on the server of a send-only
# stream, 50% of 3 on the client of a receive-only stream.
check_rejected "a buffer memory limit with extra receive buffers on the server" \
    '3072KB per server \(1 streams x 3 receive buffers x send size 1024KB \+ 1 endpoints x receive size 0B\)' \
    --servers "$SERVER" --clients "$CLIENT" --sendsize 1m --recvsize 0 --netrecvpct 300 \
    --netmemmax 2m
check_rejected "a buffer memory limit with fewer receive buffers on the client" \
    '2048KB per client \(1 threads x \(IO depth 3 x send size 0B \+ 2 receive buffers x receive size 1024KB\)\)' \
    --servers "$SERVER" --clients "$CLIENT" --sendsize 0 --recvsize 1m --iodepth 3 \
    --netrecvpct 50 --netmemmax 1m
check_rejected "a receive buffer percentage of zero" \
    '"--netrecvpct" must be at least 1\.' \
    --servers "$SERVER" --clients "$CLIENT" --netrecvpct 0

# "--tservers" and "--tclients" may only reduce the thread count given by "-t".
check_rejected "more server threads than threads per service" \
    '"--tservers" and "--tclients" may not exceed the number of threads per service' \
    --servers "$SERVER" --clients "$CLIENT" -t 2 --tservers 4

################## Phases other than the implied write phase ##################

# Netbench mode always runs the write phase and nothing else, so every other
# phase selection is rejected.
PHASE_MSG='Netbench mode only run in write phase\.'

check_rejected "a read phase" "$PHASE_MSG" \
    --servers "$SERVER" --clients "$CLIENT" -r
check_rejected "a delete files phase" "$PHASE_MSG" \
    --servers "$SERVER" --clients "$CLIENT" -F
check_rejected "a create dirs phase" "$PHASE_MSG" \
    --servers "$SERVER" --clients "$CLIENT" -d
check_rejected "a stat files phase" "$PHASE_MSG" \
    --servers "$SERVER" --clients "$CLIENT" --stat

################## Unsupported options ##################

check_rejected "a start offset" \
    'Netbench mode does not support "--offset"\.' \
    --servers "$SERVER" --clients "$CLIENT" --offset 4k
check_rejected "unshared service paths" \
    'Netbench mode does not support "--nosvcshare"\.' \
    --servers "$SERVER" --clients "$CLIENT" --nosvcshare

# "--netrnr" is an InfiniBand timer code (5 bits) for the queue pairs of the
# ibverbs and rdmacm engines; the default engine is tcp.
check_rejected "a receiver-not-ready timer code above 31" \
    '"--netrnr" must be between 0 and 31\.' \
    --servers "$SERVER" --clients "$CLIENT" --netrnr 32
check_rejected "a receiver-not-ready timer with the tcp engine" \
    '"--netrnr" is only supported by the ibverbs and rdmacm engines' \
    --servers "$SERVER" --clients "$CLIENT" --netrnr 5

################## Provider listing ##################

# "--netprov list" prints the libfabric providers and interfaces of this host and
# then ends with an error, like the SPDK namespace listing. The tcp provider is
# part of every libfabric build (as "tcp;ofi_rxm" before libfabric 1.18). A
# build without the libfabric engine rejects the option differently, so the
# listing checks are skipped there.
if has_build_feature libfabric; then
    check_rejected "the provider listing" \
        'Providers and interfaces usable for netbench on this host:' \
        --netprov list
    check_rejected "the provider listing with the tcp provider" \
        '  tcp(;ofi_rxm)?: protocol' \
        --netprov list
else
    tap_skip "the provider listing needs the libfabric engine"
    tap_skip "the provider listing with the tcp provider needs the libfabric engine"
fi
