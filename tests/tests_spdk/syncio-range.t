#!/bin/bash
#
# Synchronous SPDK IO and containment of all accesses within "--offset"/"--size".
#
# Uses "--iodepth 1" to select the synchronous IO path, which logs exactly one
# operation per block and therefore allows exact assertions on the accessed
# range. Containment is checked in three independent ways: the offsets elbencho
# logged, the contents of the backing file, and the target's own IO statistics.
# The backing file is prefilled with 0xFF beforehand, so that written and
# untouched regions can be told apart byte-exactly on any file system.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/spdk.sh" || exit 1

require_spdk

test_init
tap_plan 24

NS_IDX=0
OFFSET=$((8 * 1024 * 1024))
LENGTH=$((16 * 1024 * 1024))
BLOCK_SIZE=4096
EXPECTED_OPS=$((LENGTH / BLOCK_SIZE))

OFFSET_MIB=$((OFFSET / 1024 / 1024))
LENGTH_MIB=$((LENGTH / 1024 / 1024))
END_MIB=$((OFFSET_MIB + LENGTH_MIB))

# Prefill the namespace under test with 0xFF, so that written and untouched
# regions can be told apart afterwards. start_nvmf_tgt applies this whenever it
# creates the backing files, including on a retry.
SPDK_PREFILL_FF_IDXS=($NS_IDX)

start_nvmf_tgt
if [ $? -ne 0 ]; then
    tap_bail "Unable to start an SPDK NVMe-oF target (see nvmf_tgt.log)."
fi

NS_UUID="$(spdk_ns_uuid $NS_IDX)"
NS_NAME="$(spdk_ns_name $NS_IDX)"
NS_SIZE_MIB="$(spdk_ns_size_mib $NS_IDX)"
BDEV="$(spdk_bdev_name $NS_IDX)"
BACKING_FILE="$(spdk_bdev_file $NS_IDX)"
OPSLOG="$TEST_DIR/write.opslog"
LIVECSV="$TEST_DIR/write.livecsv"

################## Write a limited range synchronously ##################

run_elbencho write "${SPDK_OPTS[@]}" \
    -w -t 1 --iodepth 1 -b $BLOCK_SIZE --offset $OFFSET -s $LENGTH \
    --verify 1 --blockvarpct 0 \
    --opslog "$OPSLOG" --livecsv "$LIVECSV" --liveint 200 \
    "$NS_UUID"
assert_ok $? "synchronous write of a limited range succeeds"

assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$LENGTH" \
    "write phase reports exactly $LENGTH written bytes"
assert_gt "$(json_value "$ELB_JSON" WRITE last_done 'bytes/s')" 0 \
    "write phase reports a non-zero throughput"
assert_eq "$(json_config "$ELB_JSON" WRITE path_type)" "blockdev" \
    "an SPDK namespace is reported as a block device benchmark path"
assert_eq "$(json_config "$ELB_JSON" WRITE io_depth)" "1" \
    "the result confirms an IO depth of 1"
assert_eq "$(json_config "$ELB_JSON" WRITE file_size)" "$LENGTH" \
    "the result confirms the given size"

################## Containment check 1: the logged operations ##################

assert_eq "$(opslog_count "$OPSLOG" spdkWrite)" "$EXPECTED_OPS" \
    "operations log contains exactly $EXPECTED_OPS completed write operations"
assert_eq "$(opslog_count "$OPSLOG" spdkRead)" "0" \
    "operations log contains no read operation"
assert_eq "$(opslog_error_count "$OPSLOG")" "0" \
    "operations log contains no failed operation"
assert_eq "$(opslog_entry_names "$OPSLOG")" "$NS_NAME" \
    "all operations went to the selected namespace only"

# The write covers the given range completely and sequentially, so these are
# exact values rather than just bounds.
assert_eq "$(opslog_min_offset "$OPSLOG")" "$OFFSET" \
    "the lowest accessed offset is exactly the given offset"
assert_eq "$(opslog_max_end "$OPSLOG")" "$((OFFSET + LENGTH))" \
    "the highest accessed offset plus length is exactly offset plus size"
assert_eq "$(opslog_bytes_sum "$OPSLOG")" "$LENGTH" \
    "the logged block sizes sum up to exactly the given size"
assert_eq "$(opslog_nonaligned_count "$OPSLOG" $SPDK_SECTOR_SIZE)" "0" \
    "every logged offset and length is aligned to the sector size"

################## Containment check 2: the target's own statistics ##################

assert_eq "$(spdk_tgt_iostat "$BDEV" bytes_written)" "$LENGTH" \
    "the target's own statistics confirm exactly $LENGTH written bytes"
assert_eq "$(spdk_tgt_iostat "$BDEV" num_write_ops)" "$EXPECTED_OPS" \
    "the target's own statistics confirm exactly $EXPECTED_OPS write operations"

################## Read the range back with a different block size ##################

run_elbencho read "${SPDK_OPTS[@]}" \
    -r -t 1 --iodepth 1 -b 65536 --offset $OFFSET -s $LENGTH \
    --verify 1 \
    --opslog "$TEST_DIR/read.opslog" \
    "$NS_UUID"
assert_ok $? "reading the range back with a larger block size and integrity check succeeds"

assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$LENGTH" \
    "read phase reports exactly $LENGTH read bytes"
assert_eq "$(opslog_min_offset "$TEST_DIR/read.opslog")" "$OFFSET" \
    "the read phase also starts exactly at the given offset"
assert_eq "$(opslog_max_end "$TEST_DIR/read.opslog")" "$((OFFSET + LENGTH))" \
    "the read phase also ends exactly at offset plus size"

assert_eq "$(livecsv_header "$LIVECSV")" "$LIVECSV_HEADER" \
    "live statistics csv file has the expected header line"

################## Containment check 3: the backing file contents ##################

# Stopping the target closes the backing file, so its contents are complete.
stop_nvmf_tgt

tap_diag "backing file allocated bytes: $(du --block-size=1 "$BACKING_FILE" | cut -f1)"

assert_eq "$(region_is_not_ff "$BACKING_FILE" 0 $OFFSET_MIB)" "0" \
    "the ${OFFSET_MIB} MiB below the given offset are untouched in the backing file"
assert_eq "$(region_is_not_ff "$BACKING_FILE" $END_MIB $((NS_SIZE_MIB - END_MIB)))" "0" \
    "everything above offset plus size is untouched in the backing file"
assert_gt "$(region_is_not_ff "$BACKING_FILE" $OFFSET_MIB $LENGTH_MIB)" 0 \
    "the region within offset and size was written in the backing file"
