#!/bin/bash
#
# Mixed read/write workloads on an SPDK namespace.
#
# elbencho offers two ways to mix reads and writes: "--rwmixpct" makes every
# worker thread issue a percentage of its operations as reads, and "--rwmixthr"
# dedicates a subset of the worker threads to reading. Both are deterministic,
# so this test asserts exact operation counts rather than just "some reads
# happened", and cross-checks them against the target's own statistics.
#
# Note that the phase name changes in these modes, from "WRITE" to "RWMIX<pct>"
# respectively "MIX-T<numthreads>".

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/spdk.sh" || exit 1

require_spdk

test_init
tap_plan 24

NS_IDX=1
BLOCK_SIZE=4096
PCT_LENGTH=$((4 * 1024 * 1024))
READ_PERCENT=30

# The read/write decision per operation is "(workerRank + opNum) % 100 < pct",
# so with a single worker thread and 1024 operations the split is exact: 10 full
# hundreds of 30 reads each, plus 24 reads in the remaining 24 operations.
TOTAL_OPS=$((PCT_LENGTH / BLOCK_SIZE))
EXPECTED_READS=$(( (TOTAL_OPS / 100) * READ_PERCENT + (TOTAL_OPS % 100) ))
EXPECTED_WRITES=$((TOTAL_OPS - EXPECTED_READS))
EXPECTED_READ_BYTES=$((EXPECTED_READS * BLOCK_SIZE))
EXPECTED_WRITE_BYTES=$((EXPECTED_WRITES * BLOCK_SIZE))
PCT_PHASE="RWMIX$READ_PERCENT"

start_nvmf_tgt
if [ $? -ne 0 ]; then
    tap_bail "Unable to start an SPDK NVMe-oF target (see nvmf_tgt.log)."
fi

NS_UUID="$(spdk_ns_uuid $NS_IDX)"
BDEV="$(spdk_bdev_name $NS_IDX)"

################## A read percentage, in both IO engines ##################

# The selection formula is the same in the synchronous and the asynchronous
# engine, so both must produce exactly the same split. That is a strong check
# that the two engines agree.
for iodepth in 1 8; do
    OPSLOG="$TEST_DIR/pct$iodepth.opslog"

    spdk_rpc bdev_reset_iostat > /dev/null 2>&1

    run_elbencho "pct$iodepth" "${SPDK_OPTS[@]}" \
        -w -t 1 --iodepth $iodepth -b $BLOCK_SIZE -s $PCT_LENGTH \
        --rwmixpct $READ_PERCENT \
        --opslog "$OPSLOG" \
        "$NS_UUID"
    assert_ok $? "write phase with $READ_PERCENT% reads and IO depth $iodepth succeeds"

    assert_eq "$(json_phases "$ELB_JSON")" "$PCT_PHASE" \
        "the phase is reported as \"$PCT_PHASE\" with IO depth $iodepth"

    assert_eq "$(opslog_count "$OPSLOG" spdkRead)" "$EXPECTED_READS" \
        "exactly $EXPECTED_READS read operations with IO depth $iodepth"
    assert_eq "$(opslog_count "$OPSLOG" spdkWrite)" "$EXPECTED_WRITES" \
        "exactly $EXPECTED_WRITES write operations with IO depth $iodepth"

    assert_eq "$(json_value "$ELB_JSON" "$PCT_PHASE" last_done bytes)" "$EXPECTED_WRITE_BYTES" \
        "the result reports $EXPECTED_WRITE_BYTES written bytes with IO depth $iodepth"
    assert_eq "$(json_value_rwmix "$ELB_JSON" "$PCT_PHASE" last_done bytes)" "$EXPECTED_READ_BYTES" \
        "the result reports $EXPECTED_READ_BYTES read bytes with IO depth $iodepth"

    assert_eq "$(opslog_error_count "$OPSLOG")" "0" \
        "no failed operation with IO depth $iodepth"
done

################## Dedicated reader threads ##################

THR_LENGTH=$((16 * 1024 * 1024))
NUM_THREADS=4
NUM_READERS=2
THR_PHASE="MIX-T$NUM_READERS"
THRLOG="$TEST_DIR/thr.opslog"

run_elbencho rwmixthr "${SPDK_OPTS[@]}" \
    -w -t $NUM_THREADS --iodepth 4 -b 65536 -s $THR_LENGTH \
    --rwmixthr $NUM_READERS \
    --opslog "$THRLOG" \
    "$NS_UUID"
assert_ok $? "write phase with $NUM_READERS dedicated reader threads succeeds"

assert_eq "$(json_phases "$ELB_JSON")" "$THR_PHASE" \
    "the phase is reported as \"$THR_PHASE\""

# Each of the 4 threads covers a quarter of the range, and 2 of them read, so
# reads and writes each cover exactly half of the range.
assert_eq "$(json_value "$ELB_JSON" "$THR_PHASE" last_done bytes)" "$((THR_LENGTH / 2))" \
    "the writer threads wrote exactly half of the range"
assert_eq "$(json_value_rwmix "$ELB_JSON" "$THR_PHASE" last_done bytes)" "$((THR_LENGTH / 2))" \
    "the reader threads read exactly half of the range"

# This is what actually pins the semantics of the option: the first threads read
# and the remaining ones write.
assert_eq "$(opslog_ranks_for_op "$THRLOG" spdkRead)" "0,1" \
    "exactly the first $NUM_READERS worker threads issued the reads"
assert_eq "$(opslog_ranks_for_op "$THRLOG" spdkWrite)" "2,3" \
    "exactly the remaining worker threads issued the writes"
assert_eq "$(opslog_error_count "$THRLOG")" "0" \
    "no failed operation with dedicated reader threads"

################## Mutually exclusive options ##################

run_elbencho bothmix "${SPDK_OPTS[@]}" \
    -w -t 2 -b $BLOCK_SIZE -s $BLOCK_SIZE --rwmixpct 30 --rwmixthr 1 \
    "$NS_UUID" > /dev/null 2>&1
assert_nok $? "combining a read percentage with dedicated reader threads is rejected"

run_elbencho mixverify "${SPDK_OPTS[@]}" \
    -w -t 1 -b $BLOCK_SIZE -s $BLOCK_SIZE --rwmixpct 30 --verify 1 --blockvarpct 0 \
    "$NS_UUID" > /dev/null 2>&1
assert_nok $? "combining a read percentage with a data integrity check is rejected"
assert_match "$(cat "$ELB_OUT")" 'cannot be used together with option "--verify"' \
    "the rejection names the conflicting options"
