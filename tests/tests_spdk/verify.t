#!/bin/bash
#
# Data integrity checks on an SPDK namespace.
#
# "--verify" writes a pattern derived from the salt and the absolute offset of
# each block, which can then be checked in a read phase using the same salt.
# Reading with a different salt, or reading the right data at the wrong offset,
# must fail - that is what proves the check is actually effective rather than
# silently passing.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/spdk.sh" || exit 1

require_spdk

test_init
tap_plan 18

NS_IDX=3
NUM_THREADS=2
LENGTH=$((16 * 1024 * 1024))
SALT=1
WRONG_SALT=99

start_nvmf_tgt
if [ $? -ne 0 ]; then
    tap_bail "Unable to start an SPDK NVMe-oF target (see nvmf_tgt.log)."
fi

NS_UUID="$(spdk_ns_uuid $NS_IDX)"

################## Write the pattern, then read it back ##################

run_elbencho write "${SPDK_OPTS[@]}" \
    -w -t $NUM_THREADS --iodepth 1 -b 4096 -s $LENGTH \
    --verify $SALT --blockvarpct 0 \
    "$NS_UUID"
assert_ok $? "write phase with an integrity check pattern succeeds"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$LENGTH" \
    "write phase reports exactly $LENGTH written bytes"

# A different block size on read means the check has to work across the
# boundaries of the blocks that were written.
run_elbencho readsync "${SPDK_OPTS[@]}" \
    -r -t $NUM_THREADS --iodepth 1 -b 65536 -s $LENGTH \
    --verify $SALT \
    "$NS_UUID"
assert_ok $? "synchronous read with the matching salt and a different block size verifies"
assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$LENGTH" \
    "the verifying read phase reports exactly $LENGTH read bytes"

run_elbencho readasync "${SPDK_OPTS[@]}" \
    -r -t $NUM_THREADS --iodepth 16 -b 4096 -s $LENGTH \
    --verify $SALT \
    "$NS_UUID"
assert_ok $? "asynchronous read with the matching salt verifies"

################## A wrong salt must fail ##################

for iodepth in 1 8; do
    run_elbencho "badsalt$iodepth" "${SPDK_OPTS[@]}" \
        -r -t 1 --iodepth $iodepth -b 4096 -s $LENGTH \
        --verify $WRONG_SALT \
        "$NS_UUID" > /dev/null 2>&1
    assert_nok $? "reading with a mismatching salt fails with IO depth $iodepth"
    assert_match "$(cat "$ELB_OUT")" \
        'Data verification failed\. Offset: [0-9]+; Expected value: [0-9]+; Actual value: [0-9]+' \
        "the mismatch is reported as a data verification failure with IO depth $iodepth"
done

################## The pattern is anchored to the absolute offset ##################

# Writing at an offset and then verifying the same salt at a different offset
# must fail, because the expected content depends on the absolute offset. This
# also confirms that offsets survive the translation to namespace block numbers
# and back.
SHIFT=$((4 * 1024 * 1024))

run_elbencho shiftwrite "${SPDK_OPTS[@]}" \
    -w -t 1 --iodepth 1 -b 4096 --offset $SHIFT -s $((1024 * 1024)) \
    --verify 7 --blockvarpct 0 \
    "$NS_UUID"
assert_ok $? "write phase at an offset with an integrity check pattern succeeds"

run_elbencho shiftread "${SPDK_OPTS[@]}" \
    -r -t 1 --iodepth 1 -b 4096 --offset $SHIFT -s $((1024 * 1024)) \
    --verify 7 \
    "$NS_UUID"
assert_ok $? "reading it back at the same offset verifies"

run_elbencho shiftreadbad "${SPDK_OPTS[@]}" \
    -r -t 1 --iodepth 1 -b 4096 --offset 0 -s $((1024 * 1024)) \
    --verify 7 \
    "$NS_UUID" > /dev/null 2>&1
assert_nok $? "reading the same salt at a different offset fails"
assert_match "$(cat "$ELB_OUT")" 'Data verification failed\.' \
    "the offset mismatch is reported as a data verification failure"

################## Verification right after writing each block ##################

run_elbencho direct "${SPDK_OPTS[@]}" \
    -w -t 1 --iodepth 1 -b 4096 -s $((1024 * 1024)) \
    --verify $SALT --blockvarpct 0 --verifydirect \
    "$NS_UUID"
assert_ok $? "write phase with immediate verification of each block succeeds"

################## Argument checks ##################

# An integrity check write does not need "--blockvarpct 0" to be given
# explicitly: elbencho disables block variance implicitly in that case. Reading
# the result back with the same salt confirms that this really happened, because
# any leftover block variance would make the verification fail.
run_elbencho novariance "${SPDK_OPTS[@]}" \
    -w -t 1 --iodepth 1 -b 4096 -s $((1024 * 1024)) --verify $SALT \
    "$NS_UUID"
assert_ok $? "an integrity check write without an explicit block variance succeeds"

run_elbencho novarianceread "${SPDK_OPTS[@]}" \
    -r -t 1 --iodepth 1 -b 4096 -s $((1024 * 1024)) --verify $SALT \
    "$NS_UUID"
assert_ok $? "its data verifies, so block variance was disabled implicitly"

run_elbencho randverify "${SPDK_OPTS[@]}" \
    -w -t 1 -b 4096 -s $((1024 * 1024)) --verify $SALT --blockvarpct 0 --rand \
    "$NS_UUID" > /dev/null 2>&1
assert_nok $? "an integrity check write at random offsets is rejected"

run_elbencho directdepth "${SPDK_OPTS[@]}" \
    -w -t 1 --iodepth 2 -b 4096 -s $((1024 * 1024)) \
    --verify $SALT --blockvarpct 0 --verifydirect \
    "$NS_UUID" > /dev/null 2>&1
assert_nok $? "immediate verification together with an IO depth above 1 is rejected"
