#!/bin/bash
#
# Argument checks of netbench mode.
#
# All of these are rejected while the arguments are being checked, i.e. before
# any service instance is contacted, so this test needs no services at all and
# runs in milliseconds. It is the cheapest regression guard for the netbench
# option surface.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1

test_init
tap_plan 14

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
    --servers "$SERVER" -s 1m

check_rejected "netbench mode with clients but no servers" \
    'At least one server needs to be defined for netbench mode\.' \
    --clients "$CLIENT" -s 1m

# "--hosts" is not read at all in netbench mode, so it does not satisfy the
# servers and clients requirement.
check_rejected "netbench mode with \"--hosts\" instead of servers and clients" \
    'Missing servers & clients definition for netbench mode\.' \
    --hosts "$SERVER" -s 1m

# The same service cannot act as server and client at the same time.
check_rejected "the same host as server and client" \
    'List of hosts contains duplicates' \
    --servers "$SERVER" --clients "$SERVER" -s 1m

################## Missing or zero sizes ##################

SIZE_MSG='Blocksize, response size and file size must not be zero in netbench mode\.'

# Block size and response size have non-zero defaults, so the file size is the
# one that has to be given explicitly.
check_rejected "a missing file size" "$SIZE_MSG" \
    --servers "$SERVER" --clients "$CLIENT"
check_rejected "a file size of zero" "$SIZE_MSG" \
    --servers "$SERVER" --clients "$CLIENT" -s 0
check_rejected "a response size of zero" "$SIZE_MSG" \
    --servers "$SERVER" --clients "$CLIENT" -s 1m --respsize 0
# A zero block size is caught by the more specific block size check before the
# netbench check gets a chance to complain about it.
check_rejected "a block size of zero" \
    'Block size must not be 0\.' \
    --servers "$SERVER" --clients "$CLIENT" -s 1m -b 0

################## Phases other than the implied write phase ##################

# Netbench mode always runs the write phase and nothing else, so every other
# phase selection is rejected.
PHASE_MSG='Netbench mode only run in write phase\.'

check_rejected "a read phase" "$PHASE_MSG" \
    --servers "$SERVER" --clients "$CLIENT" -s 1m -r
check_rejected "a delete files phase" "$PHASE_MSG" \
    --servers "$SERVER" --clients "$CLIENT" -s 1m -F
check_rejected "a create dirs phase" "$PHASE_MSG" \
    --servers "$SERVER" --clients "$CLIENT" -s 1m -d
check_rejected "a stat files phase" "$PHASE_MSG" \
    --servers "$SERVER" --clients "$CLIENT" -s 1m --stat

################## Unsupported options ##################

check_rejected "a start offset" \
    'Netbench mode does not support "--offset"\.' \
    --servers "$SERVER" --clients "$CLIENT" -s 1m --offset 4k
