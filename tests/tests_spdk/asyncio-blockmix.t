#!/bin/bash
#
# Asynchronous SPDK IO and mixed block sizes.
#
# "--iodepth" greater than 1 selects the asynchronous IO path, which is a
# separate implementation from the synchronous one, so both need their own
# coverage. The same workload is run synchronously and asynchronously to confirm
# that both engines cover exactly the same range.
#
# The block size mix is drawn randomly per operation without a seed, so the
# number of blocks per size is not predictable. What is predictable is the total
# amount of data, that only sizes from the mix are used and that the last block
# of a range may be clipped to fit.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/spdk.sh" || exit 1

require_spdk

test_init
tap_plan 24

NS_IDX=2
NUM_THREADS=2
IO_DEPTH=8
LENGTH=$((16 * 1024 * 1024))
SMALL_BLOCK=4096
LARGE_BLOCK=65536

start_nvmf_tgt
if [ $? -ne 0 ]; then
    tap_bail "Unable to start an SPDK NVMe-oF target (see nvmf_tgt.log)."
fi

NS_UUID="$(spdk_ns_uuid $NS_IDX)"
NS_NAME="$(spdk_ns_name $NS_IDX)"
BDEV="$(spdk_bdev_name $NS_IDX)"

################## Asynchronous write with a single block size ##################

run_elbencho asyncwrite "${SPDK_OPTS[@]}" \
    -w -t $NUM_THREADS --iodepth $IO_DEPTH -b $LARGE_BLOCK -s $LENGTH \
    --verify 1 --blockvarpct 0 \
    --opslog "$TEST_DIR/async.opslog" \
    "$NS_UUID"
assert_ok $? "asynchronous write with an IO depth of $IO_DEPTH succeeds"

assert_eq "$(json_config "$ELB_JSON" WRITE io_depth)" "$IO_DEPTH" \
    "the result confirms an IO depth of $IO_DEPTH"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$LENGTH" \
    "asynchronous write phase reports exactly $LENGTH written bytes"
assert_gt "$(json_value "$ELB_JSON" WRITE last_done iops)" 0 \
    "asynchronous write phase reports non-zero IOPS"

assert_eq "$(opslog_bytes_sum "$TEST_DIR/async.opslog")" "$LENGTH" \
    "the logged block sizes of the asynchronous run sum up to $LENGTH"
assert_eq "$(opslog_min_offset "$TEST_DIR/async.opslog")" "0" \
    "the asynchronous run starts at offset 0"
assert_eq "$(opslog_max_end "$TEST_DIR/async.opslog")" "$LENGTH" \
    "the asynchronous run ends exactly at the given size"
assert_eq "$(opslog_entry_names "$TEST_DIR/async.opslog")" "$NS_NAME" \
    "all asynchronous operations went to the selected namespace only"
assert_eq "$(opslog_error_count "$TEST_DIR/async.opslog")" "0" \
    "operations log of the asynchronous run contains no failed operation"
assert_eq "$(spdk_tgt_iostat "$BDEV" bytes_written)" "$LENGTH" \
    "the target's own statistics confirm the asynchronously written bytes"

################## Read it back asynchronously with an integrity check ##################

# This exercises the completion path of the asynchronous engine, which has to
# run the data integrity check for each completed slot.
run_elbencho asyncread "${SPDK_OPTS[@]}" \
    -r -t $NUM_THREADS --iodepth 16 -b $SMALL_BLOCK -s $LENGTH \
    --verify 1 \
    "$NS_UUID"
assert_ok $? "asynchronous read with an integrity check succeeds"
assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$LENGTH" \
    "asynchronous read phase reports exactly $LENGTH read bytes"

################## Same workload synchronously, for comparison ##################

run_elbencho syncwrite "${SPDK_OPTS[@]}" \
    -w -t $NUM_THREADS --iodepth 1 -b $LARGE_BLOCK -s $LENGTH \
    --verify 1 --blockvarpct 0 \
    "$NS_UUID"
assert_ok $? "the same workload also succeeds synchronously"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$LENGTH" \
    "the synchronous engine covers exactly the same amount of data"

################## Mixed block sizes, asynchronously ##################

MIXLOG="$TEST_DIR/mix.opslog"

run_elbencho blockmix "${SPDK_OPTS[@]}" \
    -w -t $NUM_THREADS --iodepth $IO_DEPTH -b "${SMALL_BLOCK}:3,${LARGE_BLOCK}:1" -s $LENGTH \
    --opslog "$MIXLOG" \
    "$NS_UUID"
assert_ok $? "write with a mix of block sizes succeeds"

assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$LENGTH" \
    "the block size mix still covers exactly $LENGTH bytes"
assert_eq "$(opslog_bytes_sum "$MIXLOG")" "$LENGTH" \
    "the logged block sizes of the mix sum up to exactly $LENGTH"

assert_gt "$(opslog_length_count "$MIXLOG" $SMALL_BLOCK)" 0 \
    "the mix used the small block size"
assert_gt "$(opslog_length_count "$MIXLOG" $LARGE_BLOCK)" 0 \
    "the mix used the large block size"
assert_eq "$(opslog_count_oversized "$MIXLOG" $LARGE_BLOCK)" "0" \
    "no operation of the mix is larger than the largest size in the mix"
assert_eq "$(opslog_nonaligned_count "$MIXLOG" $SPDK_SECTOR_SIZE)" "0" \
    "every offset and length of the mix is aligned to the sector size"
assert_eq "$(opslog_error_count "$MIXLOG")" "0" \
    "operations log of the mix contains no failed operation"

# The number of blocks per size is not asserted, because the mix is drawn
# randomly without a seed. The distribution is only reported for information.
tap_diag "block size distribution: ${SMALL_BLOCK}=$(opslog_length_count "$MIXLOG" $SMALL_BLOCK) ${LARGE_BLOCK}=$(opslog_length_count "$MIXLOG" $LARGE_BLOCK)"
tap_diag "block sizes seen (a clipped last block per thread range is expected): $(opslog_distinct_lengths "$MIXLOG")"

assert_eq "$(opslog_min_offset "$MIXLOG")" "0" \
    "the mixed block size run starts at offset 0"
assert_eq "$(opslog_max_end "$MIXLOG")" "$LENGTH" \
    "the mixed block size run ends exactly at the given size"
