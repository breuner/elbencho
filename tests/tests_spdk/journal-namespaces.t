#!/bin/bash
#
# Journaled data verification across several NVMe-oF namespaces.
#
# A journal belongs to one target, so a run over several namespaces keeps one
# journal per namespace, keyed by the namespace selector that was given on the
# command line. That is what this checks, together with the two things that only
# a block device target can show: that the journals and the data survive a
# restart of the target underneath them, and that a corruption injected into a
# namespace's backing file while the target is down is caught afterwards.
#
# The SPDK engine has its own copy of the submission loop, so the journal hooks
# into it separately from the libaio and the synchronous engine - hence the
# range, read/write mix and contention cases are repeated here rather than being
# left to the file mode tests.
#
# Namespace 0 is used for the "--offset"/"--size" case only and deliberately
# kept out of the multi namespace section, so that its untouched regions are
# still untouched when they are checked against its backing file.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/spdk.sh" || exit 1
source "$ELBENCHO_TEST_LIB/journal.sh" || exit 1

require_spdk

test_init
tap_plan 59

RANGE_NS_IDX=0
MULTI_NS_IDXS=(1 2 3)

BLOCK_SIZE=$((64 * 1024))
BIG_BLOCK_SIZE=$((256 * 1024))
JOURNAL_BLOCK=4096

# Per namespace share of the multi namespace section. Kept an exact multiple of
# block size times threads, so the random full coverage generator below really
# covers every block.
NS_LENGTH=$((8 * 1024 * 1024))
NUM_THREADS=4
IO_DEPTH=8
NUM_MULTI_NS=${#MULTI_NS_IDXS[@]}
MULTI_TOTAL=$((NUM_MULTI_NS * NS_LENGTH))
MULTI_BLOCKS=$((MULTI_TOTAL / BLOCK_SIZE))

# The window for the range case, in a namespace of 128 MiB.
OFFSET_MIB=8
LENGTH_MIB=16
END_MIB=$((OFFSET_MIB + LENGTH_MIB))
OFFSET=$((OFFSET_MIB * 1024 * 1024))
LENGTH=$((LENGTH_MIB * 1024 * 1024))
END=$((OFFSET + LENGTH))

CORRUPT_OFFSET=$((2 * 1024 * 1024))
CORRUPT_END=$((CORRUPT_OFFSET + JOURNAL_BLOCK))

TIMELIMIT=5

FAILURE_RE='Journal data verification failed\. Offset: [0-9]+; Expected value: [0-9]+; Actual value: [0-9]+'

# Prefill the range namespace with 0xFF, so that written and untouched regions
# can be told apart in its backing file later. This only happens while the
# dataset is not preserved, which is why preserving is switched on further down
# and not here.
SPDK_PREFILL_FF_IDXS=($RANGE_NS_IDX)

start_nvmf_tgt
if [ $? -ne 0 ]; then
    tap_bail "Unable to start an SPDK NVMe-oF target (see nvmf_tgt.log)."
fi

RANGE_NS="$(spdk_ns_uuid $RANGE_NS_IDX)"
RANGE_BDEV="$(spdk_bdev_name $RANGE_NS_IDX)"
RANGE_FILE="$(spdk_bdev_file $RANGE_NS_IDX)"
RANGE_NS_SIZE_MIB="$(spdk_ns_size_mib $RANGE_NS_IDX)"

# The namespaces of the multi namespace section, as selectors and as the names
# that the operations log uses for them.
MULTI_NS=()
MULTI_NAMES=()
for idx in "${MULTI_NS_IDXS[@]}"; do
    MULTI_NS+=("$(spdk_ns_uuid "$idx")")
    MULTI_NAMES+=("$(spdk_ns_name "$idx")")
done

RANGE_JOURNAL="$TEST_DIR/journal-range"
MULTI_JOURNAL="$TEST_DIR/journal-multi"
mkdir -p "$RANGE_JOURNAL" "$MULTI_JOURNAL"

################## A window of one namespace ##################

OPSLOG="$TEST_DIR/rangewrite.opslog"

run_elbencho rangewrite "${SPDK_OPTS[@]}" \
    -w -t 1 --iodepth 1 -b $BLOCK_SIZE --offset $OFFSET -s $LENGTH \
    --journaldir "$RANGE_JOURNAL" \
    --opslog "$OPSLOG" \
    "$RANGE_NS"
assert_ok $? "a journaled write of a window of one namespace succeeds"

assert_eq "$(json_config "$ELB_JSON" WRITE path_type)" "blockdev" \
    "an SPDK namespace is journaled as a block device target"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$LENGTH" \
    "the write phase reports exactly $LENGTH written bytes"

assert_eq "$(opslog_min_offset "$OPSLOG")" "$OFFSET" \
    "the lowest accessed offset is exactly the given offset"
assert_eq "$(opslog_max_end "$OPSLOG")" "$END" \
    "the highest accessed offset plus length is exactly offset plus size"
assert_eq "$(opslog_bytes_sum "$OPSLOG")" "$LENGTH" \
    "the logged block sizes sum up to exactly the given size"
assert_eq "$(opslog_nonaligned_count "$OPSLOG" $JOURNAL_BLOCK)" "0" \
    "every logged offset and length is aligned to the journal block size"

# The target's own statistics are an independent witness that nothing outside
# the window was touched.
assert_eq "$(spdk_tgt_iostat "$RANGE_BDEV" bytes_written)" "$LENGTH" \
    "the target's own statistics confirm exactly $LENGTH written bytes"

assert_eq "$(journal_field "$RANGE_JOURNAL" "$RANGE_NS" target)" "$RANGE_NS" \
    "the sidecar names the namespace selector as its target"
assert_eq "$(journal_field "$RANGE_JOURNAL" "$RANGE_NS" targetOffset)" "$OFFSET" \
    "the sidecar records the given offset"
assert_eq "$(journal_field "$RANGE_JOURNAL" "$RANGE_NS" targetSize)" "$LENGTH" \
    "the sidecar records the given size"

run_elbencho rangeread "${SPDK_OPTS[@]}" \
    -r -t 1 --iodepth 1 -b $BIG_BLOCK_SIZE --offset $OFFSET -s $LENGTH \
    --journaldir "$RANGE_JOURNAL" \
    "$RANGE_NS"
assert_ok $? "the window verifies when read back with a larger block size"
assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$LENGTH" \
    "the read phase reports exactly $LENGTH read bytes"

################## Several namespaces in one run ##################

OPSLOG="$TEST_DIR/multiwrite.opslog"

run_elbencho multiwrite "${SPDK_OPTS[@]}" \
    -w -t $NUM_THREADS --iodepth $IO_DEPTH -b $BLOCK_SIZE -s $NS_LENGTH \
    --journaldir "$MULTI_JOURNAL" \
    --opslog "$OPSLOG" \
    "${MULTI_NS[@]}"
assert_ok $? "a journaled write across $NUM_MULTI_NS namespaces succeeds"

assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$MULTI_TOTAL" \
    "the write phase reports $MULTI_TOTAL bytes, i.e. $NS_LENGTH per namespace"

assert_eq "$(journal_sidecar_count "$MULTI_JOURNAL")" "$NUM_MULTI_NS" \
    "one journal was created per namespace"
assert_eq "$(journal_file_count "$MULTI_JOURNAL")" "$((2 * NUM_MULTI_NS))" \
    "each of those journals has a sidecar and a binary file"
assert_eq "$(journal_targets "$MULTI_JOURNAL")" "${MULTI_NS[*]}" \
    "the journals name exactly the selected namespaces"
assert_eq "$(grep -c 'Created new journal for target' "$ELB_OUT")" "$NUM_MULTI_NS" \
    "the run reports one newly created journal per namespace"

assert_eq "$(opslog_entry_names "$OPSLOG")" "${MULTI_NAMES[*]}" \
    "all $NUM_MULTI_NS namespaces really took IO"
assert_eq "$(opslog_count "$OPSLOG" spdkWrite)" "$MULTI_BLOCKS" \
    "the operations log has exactly $MULTI_BLOCKS write operations"
assert_eq "$(opslog_error_count "$OPSLOG")" "0" \
    "no operation was logged as failed"

for idx in "${MULTI_NS_IDXS[@]}"; do
    assert_eq "$(spdk_tgt_iostat "$(spdk_bdev_name "$idx")" bytes_written)" "$NS_LENGTH" \
        "the target confirms $NS_LENGTH written bytes on $(spdk_ns_name "$idx")"
done

################## Reading them back in both engines ##################

for iodepth in 1 $IO_DEPTH; do
    run_elbencho "multiread$iodepth" "${SPDK_OPTS[@]}" \
        -r -t $NUM_THREADS --iodepth $iodepth -b $BLOCK_SIZE -s $NS_LENGTH \
        --journaldir "$MULTI_JOURNAL" \
        "${MULTI_NS[@]}"
    assert_ok $? "all $NUM_MULTI_NS namespaces verify with IO depth $iodepth"

    assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$MULTI_TOTAL" \
        "the read phase with IO depth $iodepth reports $MULTI_TOTAL bytes"
    assert_eq "$(grep -c 'Resumed existing journal for target' "$ELB_OUT")" "$NUM_MULTI_NS" \
        "each namespace's journal was resumed with IO depth $iodepth"
done

################## Random offsets across the namespaces ##################

run_elbencho multirandwrite "${SPDK_OPTS[@]}" \
    -w -t $NUM_THREADS --iodepth $IO_DEPTH -b $BLOCK_SIZE -s $NS_LENGTH --rand \
    --journaldir "$MULTI_JOURNAL" \
    "${MULTI_NS[@]}"
assert_ok $? "a journaled random write with full block coverage succeeds"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$MULTI_TOTAL" \
    "the random write phase reports $MULTI_TOTAL written bytes"

run_elbencho multirandverify "${SPDK_OPTS[@]}" \
    -r -t $NUM_THREADS --iodepth $IO_DEPTH -b $BLOCK_SIZE -s $NS_LENGTH \
    --journaldir "$MULTI_JOURNAL" \
    "${MULTI_NS[@]}"
assert_ok $? "the randomly written content verifies across all namespaces"
assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$MULTI_TOTAL" \
    "nothing was skipped, so the random write covered every block"

################## A verified read/write mix on block devices ##################

# The journals are fully initialized by now, so the reader threads really read
# and verify instead of initializing what they find.
THR_PHASE="MIX-T2"

run_elbencho multimix "${SPDK_OPTS[@]}" \
    -w -t $NUM_THREADS --rwmixthr 2 --iodepth $IO_DEPTH -b $BLOCK_SIZE -s $NS_LENGTH \
    --journaldir "$MULTI_JOURNAL" \
    "${MULTI_NS[@]}"
assert_ok $? "a read/write mix across the namespaces succeeds"
assert_eq "$(json_phases "$ELB_JSON")" "$THR_PHASE" \
    "the phase is reported as \"$THR_PHASE\""
assert_gt "$(json_value_rwmix "$ELB_JSON" "$THR_PHASE" last_done bytes)" 0 \
    "the reader threads verified data inside the mixed phase"

################## Contention on the SPDK engine ##################

# A block size mix makes the small IOs collide with the lock region of a large
# IO of the same thread, so most submissions have to be deferred. A deadlock
# here would show up as the command timeout rather than as a failed assertion.
run_elbencho multistress "${SPDK_OPTS[@]}" \
    -w -t $NUM_THREADS --iodepth $IO_DEPTH -b 4K:1,1M:1 -s $NS_LENGTH \
    --rand --randalgo balanced_single --infloop --timelimit $TIMELIMIT \
    --journaldir "$MULTI_JOURNAL" \
    "${MULTI_NS[@]}"
assert_ok $? "a contended mixed block size run ends by itself at the time limit"
assert_match "$(cat "$ELB_OUT")" 'Terminating due to phase time limit' \
    "the contended run really ended because of the time limit"
assert_eq "$(grep -l 'Resource deadlock avoided' "$TEST_DIR"/*.out 2>/dev/null | wc -l)" "0" \
    "no run reported a resource deadlock"

# The time limit interrupts writes that are in flight, and a write that did not
# complete leaves its journal blocks marked as never written. Writing the whole
# range again makes the state of every block defined, which the corruption case
# further down relies on.
run_elbencho multirewrite "${SPDK_OPTS[@]}" \
    -w -t $NUM_THREADS --iodepth $IO_DEPTH -b $BLOCK_SIZE -s $NS_LENGTH \
    --journaldir "$MULTI_JOURNAL" \
    "${MULTI_NS[@]}"
assert_ok $? "rewriting the whole range after the contended run succeeds"

################## Journals and data survive a restart of the target ##################

# From here on the backing files have to be kept across the restarts.
SPDK_TGT_PRESERVE_DATA=1

stop_nvmf_tgt

# The target has closed its backing file, so its content is complete now. This
# is the second, independent check of the window from the very first section.
assert_eq "$(region_is_not_ff "$RANGE_FILE" 0 $OFFSET_MIB)" "0" \
    "the ${OFFSET_MIB} MiB below the given offset are untouched in the backing file"
assert_eq "$(region_is_not_ff "$RANGE_FILE" $END_MIB $((RANGE_NS_SIZE_MIB - END_MIB)))" "0" \
    "everything above offset plus size is untouched in the backing file"
assert_gt "$(region_is_not_ff "$RANGE_FILE" $OFFSET_MIB $LENGTH_MIB)" 0 \
    "the region within offset and size was written in the backing file"

start_nvmf_tgt
if [ $? -ne 0 ]; then
    tap_bail "Unable to restart the SPDK NVMe-oF target (see nvmf_tgt.log)."
fi

# The target comes back on a different TCP port, but the namespace UUIDs are
# what the journals were keyed on, so they still match.
run_elbencho restartverify "${SPDK_OPTS[@]}" \
    -r -t $NUM_THREADS --iodepth $IO_DEPTH -b $BLOCK_SIZE -s $NS_LENGTH \
    --journaldir "$MULTI_JOURNAL" \
    "${MULTI_NS[@]}"
assert_ok $? "all namespaces still verify after a restart of the target"
assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$MULTI_TOTAL" \
    "the run after the restart read all $MULTI_TOTAL bytes"

run_elbencho restartrangeverify "${SPDK_OPTS[@]}" \
    -r -t 1 --iodepth 1 -b $BLOCK_SIZE --offset $OFFSET -s $LENGTH \
    --journaldir "$RANGE_JOURNAL" \
    "$RANGE_NS"
assert_ok $? "the windowed namespace also still verifies after the restart"

################## The same namespace under a different selector ##################

# A journal is matched by the selector string, not by the namespace behind it.
# Selecting the same namespace by its name rather than by its UUID is therefore
# a different target with its own journal - which starts out empty, so the run
# skips everything instead of verifying it.
NAME_JOURNAL="$TEST_DIR/journal-byname"
mkdir -p "$NAME_JOURNAL"

run_elbencho bynameread "${SPDK_OPTS[@]}" \
    -r -t 1 --iodepth 1 -b $BLOCK_SIZE -s $NS_LENGTH \
    --journaldir "$NAME_JOURNAL" \
    "${MULTI_NAMES[0]}"
assert_ok $? "selecting the same namespace by name succeeds"
assert_eq "$(journal_field "$NAME_JOURNAL" "${MULTI_NAMES[0]}" target)" "${MULTI_NAMES[0]}" \
    "it got its own journal, keyed on the name instead of the uuid"
assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "0" \
    "that journal is empty, so everything was skipped rather than verified"

################## Corruption in a namespace's backing file ##################

CORRUPT_NS_IDX=${MULTI_NS_IDXS[0]}
CORRUPT_NS="$(spdk_ns_uuid $CORRUPT_NS_IDX)"
CORRUPT_FILE="$(spdk_bdev_file $CORRUPT_NS_IDX)"

stop_nvmf_tgt

corrupt_file_region "$CORRUPT_FILE" $CORRUPT_OFFSET $JOURNAL_BLOCK
assert_ok $? "a region of the namespace's backing file is corrupted while the target is down"

start_nvmf_tgt
if [ $? -ne 0 ]; then
    tap_bail "Unable to restart the SPDK NVMe-oF target after the corruption."
fi

for iodepth in 1 $IO_DEPTH; do
    run_elbencho "corruptread$iodepth" "${SPDK_OPTS[@]}" \
        -r -t 1 --iodepth $iodepth -b $BLOCK_SIZE -s $NS_LENGTH \
        --journaldir "$MULTI_JOURNAL" \
        "$CORRUPT_NS" > /dev/null 2>&1
    assert_nok $? "the corrupted namespace fails verification with IO depth $iodepth"
    assert_match "$(cat "$ELB_OUT")" "$FAILURE_RE" \
        "the failure is reported as a journal verification failure with IO depth $iodepth"
done

REPORTED_OFFSET="$(grep -oP 'Journal data verification failed\. Offset: \K[0-9]+' \
    "$TEST_DIR/corruptread1.out" | head -1)"
assert_ge "$REPORTED_OFFSET" "$CORRUPT_OFFSET" \
    "the reported offset is not below the corrupted region"
assert_lt "$REPORTED_OFFSET" "$CORRUPT_END" \
    "the reported offset is not above the corrupted region"

# The other namespaces were not touched, so they still verify. That rules out
# the failure above being a general breakage after the restart.
run_elbencho intactread "${SPDK_OPTS[@]}" \
    -r -t 1 --iodepth 1 -b $BLOCK_SIZE -s $NS_LENGTH \
    --journaldir "$MULTI_JOURNAL" \
    "${MULTI_NS[@]:1}"
assert_ok $? "the namespaces that were not corrupted still verify"
