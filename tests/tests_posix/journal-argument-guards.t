#!/bin/bash
#
# Argument checks around journaled data verification.
#
# Journaling makes assumptions about the workload that cannot be relaxed: every
# IO has to be aligned to the journal's block size, the content has to be fully
# determined by the journal, and the journal has to live in one process. So a
# run that would violate any of that is rejected up front rather than producing
# verification failures later.
#
# All of these fail before any IO happens, which makes them cheap. The positive
# counterparts at the end matter as much as the rejections: a guard that also
# rejects legitimate combinations would be just as wrong.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/journal.sh" || exit 1

FILE_SIZE=$((4 * 1024 * 1024))
BLOCK_SIZE=4096

test_init
tap_plan 18

DATA_DIR="$TEST_DIR/data"
JOURNAL_DIR="$TEST_DIR/journal"
mkdir -p "$DATA_DIR" "$JOURNAL_DIR"

TARGET="$DATA_DIR/file1"

# check_rejected TAG EXPECTED_MESSAGE ARGS...
check_rejected()
{
    local tag="$1"
    local expected="$2"
    shift 2

    run_elbencho "$tag" "$@" > /dev/null 2>&1

    if [ $? -eq 0 ]; then
        tap_fail "$tag is rejected"
        tap_diag "  the run unexpectedly succeeded"
        return
    fi

    assert_match "$(cat "$ELB_OUT")" "$expected" "$tag is rejected as expected"
}

################## What a journal needs from its target ##################

# Dir mode opens a different file per thread and per iteration, so there is no
# stable target a journal could belong to.
check_rejected "journaling in dir mode" \
    'requires benchmark paths to be files or block devices' \
    -d -w -t 1 -n 1 -N 1 -s $BLOCK_SIZE -b $BLOCK_SIZE \
    --journaldir "$JOURNAL_DIR" --blockvarpct 0 "$DATA_DIR"

################## What a journal needs from the content ##################

# "--verify" also decides the content of a block, so the two would compete over
# what a block is supposed to contain. Unlike block variance below, that is not
# something one side can just win: both are what the user asked to be written.
check_rejected "journaling together with \"--verify\"" \
    'cannot be used together with "--verify"' \
    -w -t 1 -s $FILE_SIZE -b $BLOCK_SIZE --verify 1 \
    --journaldir "$JOURNAL_DIR" --blockvarpct 0 "$TARGET"

# Block variance is not rejected but implicitly disabled, the same way "--verify"
# does it, so a journaled write needs no "--blockvarpct 0" of its own. Reading
# the result back is what proves the variance really went away: any leftover
# random refill would make the verification fail.
VARIANCE_JOURNAL="$TEST_DIR/journal-variance"
mkdir -p "$VARIANCE_JOURNAL"

run_elbencho variancewrite \
    -w -t 1 -s $FILE_SIZE -b $BLOCK_SIZE --blockvarpct 50 \
    --journaldir "$VARIANCE_JOURNAL" \
    "$TARGET"
assert_ok $? "a journaled write with an explicit block variance is accepted"

run_elbencho varianceread \
    -r -t 1 -s $FILE_SIZE -b $BLOCK_SIZE \
    --journaldir "$VARIANCE_JOURNAL" \
    "$TARGET"
assert_ok $? "its data verifies, so block variance was disabled implicitly"

################## What a journal needs from the alignment ##################

# The journal tracks whole blocks, so every IO has to cover whole blocks. That
# applies to each size of a block size mix as well.
check_rejected "a block size that is not a multiple of the journal block size" \
    "Block size is not a multiple of the write journal's block size" \
    -w -t 1 -s $FILE_SIZE -b 6144 \
    --journaldir "$JOURNAL_DIR" --blockvarpct 0 "$TARGET"

check_rejected "a block size mix with one size that is not a multiple" \
    "Block size is not a multiple of the write journal's block size" \
    -w -t 1 -s $FILE_SIZE -b 4K:1,6K:1 \
    --journaldir "$JOURNAL_DIR" --blockvarpct 0 "$TARGET"

check_rejected "an offset that is not a multiple of the journal block size" \
    'Offset \("--offset"\) is not a multiple' \
    -w -t 1 -s $FILE_SIZE -b $BLOCK_SIZE --offset 2048 \
    --journaldir "$JOURNAL_DIR" --blockvarpct 0 "$TARGET"

# A size that is not a multiple is not rejected but rounded down, the same way elbencho
# already rounds it for direct and random IO. The sidecar is where the result of the
# rounding is observable, so this checks the arithmetic and not just that the run survived:
# 6144 bytes with a 4096 journal block has to become exactly 4096.
ROUND_JOURNAL="$TEST_DIR/journal-rounded"
mkdir -p "$ROUND_JOURNAL"

run_elbencho roundsize \
    -w -t 1 -s 6144 -b $BLOCK_SIZE \
    --journaldir "$ROUND_JOURNAL" \
    "$TARGET"
assert_ok $? "a size that is not a multiple of the journal block size is accepted"
assert_match "$(cat "$ELB_OUT")" 'Reducing file size.*Old: 6144; New: 4096' \
    "the run reports that it rounded the size down"
assert_eq "$(journal_field "$ROUND_JOURNAL" "$TARGET" targetSize)" "4096" \
    "the journal was created for the rounded size"

# There is nothing left to round down to below one journal block, so that is an error.
check_rejected "a size smaller than one journal block" \
    'is smaller than the write journal.s block size' \
    -w -t 1 -s 1000 -b 1000 --journalblock 4096 \
    --journaldir "$JOURNAL_DIR" "$TARGET"

check_rejected "unaligned random IO" \
    'requires block-aligned random I/O' \
    -w -t 1 -s $FILE_SIZE -b $BLOCK_SIZE --rand --norandalign \
    --journaldir "$JOURNAL_DIR" --blockvarpct 0 "$TARGET"

################## A journal cannot be shared between processes ##################

# The journal's locking is in-process only, so neither a coordinator with remote
# services nor a service instance itself can use one.
check_rejected "journaling together with a host list" \
    'is not supported together with "--hosts"' \
    -w -t 1 -s $FILE_SIZE -b $BLOCK_SIZE --hosts "localhost:$(find_free_port)" \
    --journaldir "$JOURNAL_DIR" --blockvarpct 0 "$TARGET"

# This is rejected while the arguments are checked, long before anything is
# bound or daemonized, but it still gets a free port rather than the default
# one so that it can never disturb a test running in parallel.
check_rejected "journaling in service mode" \
    'is not supported in service mode' \
    --service --foreground --port "$(find_free_port)" --journaldir "$JOURNAL_DIR"

################## Legitimate combinations are accepted ##################

# A non-default journal block size, with an IO block size equal to it.
BIG_JOURNAL_DIR="$TEST_DIR/journal-8k"
mkdir -p "$BIG_JOURNAL_DIR"

run_elbencho bigjournalblock \
    -w -t 1 -s $FILE_SIZE -b 8192 --journalblock 8K \
    --journaldir "$BIG_JOURNAL_DIR" --blockvarpct 0 \
    "$TARGET"
assert_ok $? "a non-default journal block size is accepted"
assert_eq "$(journal_field "$BIG_JOURNAL_DIR" "$TARGET" journalBlockSize)" "8192" \
    "the sidecar records the given journal block size"
assert_eq "$(file_size "$(journal_binary_for "$BIG_JOURNAL_DIR" "$TARGET")")" \
    "$(journal_expected_binary_size $FILE_SIZE 8192)" \
    "the larger journal block size halves the size of the binary journal"

# An IO block size that is a multiple of the journal block size, which is the
# normal case: the journal tracks finer than the IO.
MULT_JOURNAL_DIR="$TEST_DIR/journal-multiple"
mkdir -p "$MULT_JOURNAL_DIR"

# The explicit "--blockvarpct 0" here is redundant since journaling disables
# block variance by itself, but it is kept so that spelling stays covered too.
run_elbencho blockmultiple \
    -w -t 1 -s $FILE_SIZE -b 65536 \
    --journaldir "$MULT_JOURNAL_DIR" --blockvarpct 0 \
    "$TARGET"
assert_ok $? "a block size that is a multiple of the journal block size is accepted"
