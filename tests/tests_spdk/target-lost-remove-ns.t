#!/bin/bash
#
# The benchmarked namespace is removed while elbencho is running.
#
# Unlike killing the whole target, this leaves the target process up and the
# connection intact, and only takes the namespace away underneath the running
# benchmark. That is a different failure path than a dropped connection, so it
# gets its own test.
#
# As in the killed target test, elbencho must notice, report an error and
# terminate by itself, and the run therefore must not be wrapped in a timeout.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/spdk.sh" || exit 1

require_spdk

test_init
tap_plan 10

NS_IDX=2
RATE=8m
GRACE_SECS=30
MAX_SECS=20

start_nvmf_tgt
if [ $? -ne 0 ]; then
    tap_bail "Unable to start an SPDK NVMe-oF target (see nvmf_tgt.log)."
fi

NS_UUID="$(spdk_ns_uuid $NS_IDX)"
NS_NQN="$(spdk_ns_nqn $NS_IDX)"
NS_NSID="$(spdk_ns_nsid $NS_IDX)"
BDEV="$(spdk_bdev_name $NS_IDX)"

assert_eq "$(spdk_tgt_ns_count "$NS_NQN")" "2" \
    "the subsystem has both of its namespaces before the test"

################## Start a long enough run ##################

spdk_start_background_elbencho removens \
    "${SPDK_OPTS[@]}" \
    -w -t 1 --iodepth 4 -b 65536 -s "$(spdk_ns_size $NS_IDX)" --limitwrite $RATE \
    "$NS_UUID"

if ! spdk_wait_for_target_io "$BDEV" 30; then
    tap_diag "elbencho never sent any IO to the target. Output:"
    tap_diag_file "$ELB_BG_OUT"
    spdk_kill_background_elbencho
    tap_bail "Unable to get the benchmark into a running state."
fi

tap_ok "elbencho is writing to the namespace"

sleep 1 # let some steady state IO accumulate

################## Pull the namespace away ##################

spdk_remove_ns "$NS_NQN" "$NS_NSID"
assert_ok $? "the namespace can be removed while the benchmark is running"

assert_eq "$(spdk_tgt_ns_count "$NS_NQN")" "1" \
    "the target confirms the namespace is gone"

################## elbencho has to deal with it ##################

spdk_wait_for_own_exit $GRACE_SECS
spdk_assert_clean_termination "the benchmarked namespace was removed" $MAX_SECS

# The target itself must still be alive, i.e. this really was a namespace
# removal and not an accidental target crash.
if spdk_tgt_alive; then
    tap_ok "the target process is still running afterwards"
else
    tap_fail "the target process is still running afterwards"
    tap_diag "target log:"
    tap_diag_file "$TEST_DIR/nvmf_tgt.log"
fi
