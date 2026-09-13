#!/bin/bash
#
# Simulated corruption of journaled data has to be caught.
#
# A verification that never fails is worthless, so this modifies the target
# behind elbencho's back and checks that the next journaled read reports it, at
# the right offset, in both IO engines.
#
# The stale generation case is the interesting one: the journal counts a
# generation per journal block and advances it on every rewrite, so content that
# is perfectly valid but from an *earlier* write of the same block is rejected
# as well. A salt based "--verify" cannot see that, because the same salt and
# offset always yield the same expected content.
#
# The last case is the opposite direction: corruption inside a region the
# journal has never seen written must NOT be reported, because such a block is
# skipped by design. Otherwise a journal on a partially written target would
# drown in false positives.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/journal.sh" || exit 1

FILE_SIZE=$((4 * 1024 * 1024))
HALF_SIZE=$((FILE_SIZE / 2))
BLOCK_SIZE=$((64 * 1024))
JOURNAL_BLOCK=4096
IO_DEPTH=8

# Corrupt one whole journal block in the middle of the range.
CORRUPT_OFFSET=$((1024 * 1024))
CORRUPT_END=$((CORRUPT_OFFSET + JOURNAL_BLOCK))

# A single byte, to show the granularity is not the IO block size.
BYTE_OFFSET=$((3 * 1024 * 1024 + 17))

# Well inside the half that is deliberately left uninitialized.
UNINIT_OFFSET=$((3 * 1024 * 1024))

# Matches the message of postReadJournalVerifyBuf(), which names the offset of
# the first mismatching byte plus the expected and the found value.
FAILURE_RE='Journal data verification failed\. Offset: [0-9]+; Expected value: [0-9]+; Actual value: [0-9]+'

test_init
tap_plan 22

DATA_DIR="$TEST_DIR/data"
mkdir -p "$DATA_DIR"

# write_journaled TAG TARGET JOURNALDIR [EXTRA_ARGS...]
write_journaled()
{
    local tag="$1"
    local target="$2"
    local journaldir="$3"
    shift 3

    run_elbencho "$tag" \
        -w -t 1 -s $FILE_SIZE -b $BLOCK_SIZE \
        --journaldir "$journaldir" \
        "$@" \
        "$target"
}

################## A corrupted journal block, in both engines ##################

for iodepth in 1 $IO_DEPTH; do

    if [ "$iodepth" != "1" ] && ! has_build_feature libaio; then
        tap_skip "the corrupted block is detected with IO depth $iodepth"
        tap_skip "the failure names an offset inside the corrupted region with IO depth $iodepth"
        tap_skip "rewriting the range makes it verify cleanly again with IO depth $iodepth"
        continue
    fi

    TARGET="$DATA_DIR/corrupt$iodepth"
    JOURNAL_DIR="$TEST_DIR/journal-corrupt$iodepth"
    mkdir -p "$JOURNAL_DIR"

    write_journaled "write$iodepth" "$TARGET" "$JOURNAL_DIR" --iodepth $iodepth
    if [ $? -ne 0 ]; then
        tap_bail "Unable to write the journaled target for IO depth $iodepth."
    fi

    corrupt_file_region "$TARGET" $CORRUPT_OFFSET $JOURNAL_BLOCK

    run_elbencho "corruptread$iodepth" \
        -r -t 1 -s $FILE_SIZE -b $BLOCK_SIZE --iodepth $iodepth \
        --journaldir "$JOURNAL_DIR" \
        "$TARGET" > /dev/null 2>&1
    assert_nok $? "the corrupted block is detected with IO depth $iodepth"

    assert_match "$(cat "$ELB_OUT")" "$FAILURE_RE" \
        "the failure names an offset inside the corrupted region with IO depth $iodepth"

    # Writing the range again repairs both the content and the journal, so a
    # clean verification afterwards proves the failure above really came from
    # the corruption and not from a detector stuck in a failed state.
    write_journaled "repair$iodepth" "$TARGET" "$JOURNAL_DIR" --iodepth $iodepth > /dev/null 2>&1

    run_elbencho "repairread$iodepth" \
        -r -t 1 -s $FILE_SIZE -b $BLOCK_SIZE --iodepth $iodepth \
        --journaldir "$JOURNAL_DIR" \
        "$TARGET"
    assert_ok $? "rewriting the range makes it verify cleanly again with IO depth $iodepth"
done

# The reported offset has to lie in the corrupted journal block, not just
# anywhere. Checked once, on the synchronous run, where exactly one operation
# covers the corrupted region.
REPORTED_OFFSET="$(grep -oP 'Journal data verification failed\. Offset: \K[0-9]+' \
    "$TEST_DIR/corruptread1.out" | head -1)"
assert_ge "$REPORTED_OFFSET" "$CORRUPT_OFFSET" \
    "the reported offset is not below the corrupted region"
assert_lt "$REPORTED_OFFSET" "$CORRUPT_END" \
    "the reported offset is not above the corrupted region"

################## A single corrupted byte ##################

# The journal tracks 4 KiB blocks while the IO block size here is 64 KiB, so
# this also shows that the comparison covers the whole IO and not just its
# first journal block.
BYTE_TARGET="$DATA_DIR/bytefile"
BYTE_JOURNAL="$TEST_DIR/journal-byte"
mkdir -p "$BYTE_JOURNAL"

write_journaled bytewrite "$BYTE_TARGET" "$BYTE_JOURNAL"
assert_ok $? "the target for the single byte case is written"

corrupt_file_byte "$BYTE_TARGET" $BYTE_OFFSET

run_elbencho byteread \
    -r -t 1 -s $FILE_SIZE -b $BLOCK_SIZE \
    --journaldir "$BYTE_JOURNAL" \
    "$BYTE_TARGET" > /dev/null 2>&1
assert_nok $? "a single corrupted byte is detected"
assert_match "$(cat "$ELB_OUT")" "$FAILURE_RE" \
    "the single byte corruption is reported as a verification failure"
assert_eq "$(grep -oP 'Journal data verification failed\. Offset: \K[0-9]+' "$ELB_OUT" | head -1)" \
    "$BYTE_OFFSET" \
    "the failure names exactly the offset of the corrupted byte"

################## Valid content from an earlier generation ##################

GEN_TARGET="$DATA_DIR/genfile"
GEN_JOURNAL="$TEST_DIR/journal-gen"
mkdir -p "$GEN_JOURNAL"

write_journaled genwrite1 "$GEN_TARGET" "$GEN_JOURNAL"
assert_ok $? "the first generation of the content is written"

cp "$GEN_TARGET" "$TEST_DIR/gen1.bin"

write_journaled genwrite2 "$GEN_TARGET" "$GEN_JOURNAL"
assert_ok $? "the second generation of the content is written"

# The generation is part of the expected content, so a rewrite of every block
# has to change every block.
assert_nok "$(cmp -s "$TEST_DIR/gen1.bin" "$GEN_TARGET"; echo $?)" \
    "the second generation differs from the first one on disk"

run_elbencho genread2 \
    -r -t 1 -s $FILE_SIZE -b $BLOCK_SIZE \
    --journaldir "$GEN_JOURNAL" \
    "$GEN_TARGET"
assert_ok $? "the current generation verifies"

# Put the older, entirely valid looking content back.
cp "$TEST_DIR/gen1.bin" "$GEN_TARGET"

run_elbencho genreadstale \
    -r -t 1 -s $FILE_SIZE -b $BLOCK_SIZE \
    --journaldir "$GEN_JOURNAL" \
    "$GEN_TARGET" > /dev/null 2>&1
assert_nok $? "content from an earlier generation is rejected"
assert_match "$(cat "$ELB_OUT")" "$FAILURE_RE" \
    "the stale generation is reported as a verification failure"

################## Corruption of never written blocks is not a failure ##################

UNINIT_TARGET="$DATA_DIR/uninitfile"
UNINIT_JOURNAL="$TEST_DIR/journal-uninit"
mkdir -p "$UNINIT_JOURNAL"

# Create the target at full size without a journal, then journal only its first
# half, so the second half stays uninitialized in the journal.
run_elbencho uninitprefill \
    -w -t 1 -s $FILE_SIZE -b $BLOCK_SIZE \
    "$UNINIT_TARGET"
assert_ok $? "the target for the uninitialized case is created"

run_elbencho uninitread \
    -r -t 1 -s $FILE_SIZE -b $BLOCK_SIZE \
    --journaldir "$UNINIT_JOURNAL" \
    "$UNINIT_TARGET"
assert_ok $? "the journal for the whole range is created by a read phase"

run_elbencho uninithalfwrite \
    -w -t 1 -s $HALF_SIZE -b $BLOCK_SIZE --offset 0 \
    --journaldir "$UNINIT_JOURNAL" \
    "$UNINIT_TARGET"
assert_ok $? "only the first half of the range is journaled"

corrupt_file_region "$UNINIT_TARGET" $UNINIT_OFFSET $JOURNAL_BLOCK

run_elbencho uninitcorruptread \
    -r -t 1 -s $FILE_SIZE -b $BLOCK_SIZE \
    --journaldir "$UNINIT_JOURNAL" \
    "$UNINIT_TARGET"
assert_ok $? "corruption in a never written region is not reported as a failure"
