#!/bin/bash
#
# Uninitialized journal blocks, and verified read/write mix phases.
#
# A journal block that was never written holds no content the journal could
# predict. In a pure read phase such a block is therefore skipped without being
# read at all and accepted as OK, which is what makes a journal usable on a
# target that was only partially written - e.g. after a crash.
#
# In a read/write mix the same block is treated the other way round: the read is
# converted into a write that initializes it, so the mix can start on an empty
# target and still verify everything it reads. That is the whole point of the
# feature, because "--rwmixpct" cannot be combined with "--verify" at all.
#
# The read/write decision itself is deterministic - "(workerRank + opNum) % 100
# < pct" - so all counts below are exact rather than "some reads happened". Each
# mix is run twice against the same journal: the first run finds everything
# uninitialized and converts every read, the second one finds everything
# initialized and really reads.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/journal.sh" || exit 1

FILE_SIZE=$((8 * 1024 * 1024))
HALF_SIZE=$((FILE_SIZE / 2))
# The exact byte accounting below needs the IO block size to equal the journal
# block size. With a larger IO block, an IO that covers initialized *and*
# uninitialized journal blocks is read as a whole and counted as a whole, so the
# byte counter would exceed the initialized amount.
BLOCK_SIZE=4096

MIX_SIZE=$((4 * 1024 * 1024))
READ_PERCENT=30

# Same formula as tests_spdk/rwmix.t: with a single worker thread the split is
# exact, i.e. full hundreds of 30 reads each plus the remainder.
MIX_OPS=$((MIX_SIZE / BLOCK_SIZE))
MIX_READS=$(( (MIX_OPS / 100) * READ_PERCENT + (MIX_OPS % 100) ))
MIX_WRITES=$((MIX_OPS - MIX_READS))
PCT_PHASE="RWMIX$READ_PERCENT"

# Dedicated reader threads get their own disjoint range of the target, so with
# 2 of 4 threads reading, each side covers exactly half of it.
THR_THREADS=4
THR_READERS=2
THR_PHASE="MIX-T$THR_READERS"
THR_READ_BYTES=$((FILE_SIZE / 2))
THR_WRITE_BYTES=$((FILE_SIZE - THR_READ_BYTES))

test_init
tap_plan 34

DATA_DIR="$TEST_DIR/data"
mkdir -p "$DATA_DIR"

################## A read of never written blocks is skipped ##################

SKIP_TARGET="$DATA_DIR/skipfile"
SKIP_JOURNAL="$TEST_DIR/journal-skip"
mkdir -p "$SKIP_JOURNAL"

# Create the target without a journal first, so it exists at full size and is
# full of content that no journal knows anything about.
run_elbencho prefill \
    -w -t 2 -s $FILE_SIZE -b $BLOCK_SIZE \
    "$SKIP_TARGET"
assert_ok $? "the target is created without a journal first"

OPSLOG="$TEST_DIR/skipread.opslog"

run_elbencho skipread \
    -r -t 2 -s $FILE_SIZE -b $BLOCK_SIZE \
    --journaldir "$SKIP_JOURNAL" \
    --opslog "$OPSLOG" \
    "$SKIP_TARGET"
assert_ok $? "a read phase against a brand new journal succeeds"

assert_match "$(cat "$ELB_OUT")" 'Created new journal for target' \
    "the read phase created the journal for the whole range"
assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "0" \
    "no bytes are reported as read, because every block was skipped"
assert_eq "$(opslog_count "$OPSLOG" pread)" "0" \
    "not a single read operation was issued for an uninitialized block"

################## Partial coverage is accounted exactly ##################

# Writing only the first half of the journal's range. That is contained in the
# range the journal was created for, so it resumes rather than being rejected.
run_elbencho halfwrite \
    -w -t 2 -s $HALF_SIZE -b $BLOCK_SIZE --offset 0 \
    --journaldir "$SKIP_JOURNAL" \
    "$SKIP_TARGET"
assert_ok $? "writing only the first half of the journaled range succeeds"
assert_match "$(cat "$ELB_OUT")" 'Resumed existing journal for target' \
    "the partial write resumed the existing journal"

OPSLOG="$TEST_DIR/halfread.opslog"

run_elbencho halfread \
    -r -t 2 -s $FILE_SIZE -b $BLOCK_SIZE \
    --journaldir "$SKIP_JOURNAL" \
    --opslog "$OPSLOG" \
    "$SKIP_TARGET"
assert_ok $? "reading the whole range with only half of it initialized succeeds"

assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$HALF_SIZE" \
    "exactly the initialized half is reported as read"
assert_eq "$(opslog_count "$OPSLOG" pread)" "$((HALF_SIZE / BLOCK_SIZE))" \
    "exactly the initialized half was read, the rest was skipped"

################## A read percentage initializes what it cannot verify ##################

PCT_TARGET="$DATA_DIR/pctfile"
PCT_JOURNAL="$TEST_DIR/journal-pct"
mkdir -p "$PCT_JOURNAL"

OPSLOG="$TEST_DIR/pctfresh.opslog"

run_elbencho pctfresh \
    -w -t 1 -s $MIX_SIZE -b $BLOCK_SIZE --rwmixpct $READ_PERCENT \
    --journaldir "$PCT_JOURNAL" \
    --opslog "$OPSLOG" \
    "$PCT_TARGET"
assert_ok $? "a read/write mix phase on a fresh journal succeeds"

assert_eq "$(json_phases "$ELB_JSON")" "$PCT_PHASE" \
    "the phase is reported as \"$PCT_PHASE\""
assert_eq "$(opslog_count "$OPSLOG" pread)" "0" \
    "no read happened, because every read target was uninitialized"
assert_eq "$(opslog_count "$OPSLOG" pwrite)" "$MIX_OPS" \
    "all $MIX_OPS operations became initializing writes"
assert_eq "$(json_value_rwmix "$ELB_JSON" "$PCT_PHASE" last_done bytes)" "0" \
    "a converted read is not counted as a mix read"

OPSLOG="$TEST_DIR/pctinit.opslog"

# The very same command again. The read/write decision per operation does not
# depend on the journal, so the operations selected as reads are the same ones
# as in the run above - only now they find initialized blocks and are verified
# instead of being converted.
run_elbencho pctinit \
    -w -t 1 -s $MIX_SIZE -b $BLOCK_SIZE --rwmixpct $READ_PERCENT \
    --journaldir "$PCT_JOURNAL" \
    --opslog "$OPSLOG" \
    "$PCT_TARGET"
assert_ok $? "the same mix phase on the now initialized journal succeeds"

assert_eq "$(opslog_count "$OPSLOG" pread)" "$MIX_READS" \
    "exactly $MIX_READS operations were reads this time"
assert_eq "$(opslog_count "$OPSLOG" pwrite)" "$MIX_WRITES" \
    "exactly $MIX_WRITES operations were writes this time"
assert_eq "$(json_value "$ELB_JSON" "$PCT_PHASE" last_done bytes)" \
    "$((MIX_WRITES * BLOCK_SIZE))" \
    "the write side reports $((MIX_WRITES * BLOCK_SIZE)) bytes"
assert_eq "$(json_value_rwmix "$ELB_JSON" "$PCT_PHASE" last_done bytes)" \
    "$((MIX_READS * BLOCK_SIZE))" \
    "the read side reports $((MIX_READS * BLOCK_SIZE)) verified bytes"
assert_eq "$(opslog_error_count "$OPSLOG")" "0" \
    "no operation of the verified mix phase failed"

################## Dedicated reader threads ##################

THR_TARGET="$DATA_DIR/thrfile"
THR_JOURNAL="$TEST_DIR/journal-thr"
mkdir -p "$THR_JOURNAL"

run_elbencho thrfresh \
    -w -t $THR_THREADS --rwmixthr $THR_READERS -s $FILE_SIZE -b $BLOCK_SIZE \
    --journaldir "$THR_JOURNAL" \
    "$THR_TARGET"
assert_ok $? "a phase with dedicated reader threads on a fresh journal succeeds"

assert_eq "$(json_phases "$ELB_JSON")" "$THR_PHASE" \
    "the phase is reported as \"$THR_PHASE\""
# The reader threads found their own range uninitialized and wrote it, so the
# whole range counts as written and nothing counts as a mix read.
assert_eq "$(json_value "$ELB_JSON" "$THR_PHASE" last_done bytes)" "$FILE_SIZE" \
    "the reader threads initialized their own range, so all $FILE_SIZE bytes were written"
assert_eq "$(json_value_rwmix "$ELB_JSON" "$THR_PHASE" last_done bytes)" "0" \
    "nothing is counted as a mix read on the first run"

run_elbencho thrinit \
    -w -t $THR_THREADS --rwmixthr $THR_READERS -s $FILE_SIZE -b $BLOCK_SIZE \
    --journaldir "$THR_JOURNAL" \
    "$THR_TARGET"
assert_ok $? "the same phase on the now initialized journal succeeds"

assert_eq "$(json_value "$ELB_JSON" "$THR_PHASE" last_done bytes)" "$THR_WRITE_BYTES" \
    "the $((THR_THREADS - THR_READERS)) writer threads wrote their half, $THR_WRITE_BYTES bytes"
assert_eq "$(json_value_rwmix "$ELB_JSON" "$THR_PHASE" last_done bytes)" "$THR_READ_BYTES" \
    "the $THR_READERS reader threads verified their half, $THR_READ_BYTES bytes"

################## The same mixes in the asynchronous engine ##################

# The decision whether to read or to initialize sits in a different code path
# there, because the journal has to be consulted before the request is prepped.
if has_build_feature libaio; then

    ASYNC_TARGET="$DATA_DIR/asyncmixfile"
    ASYNC_JOURNAL="$TEST_DIR/journal-asyncmix"
    mkdir -p "$ASYNC_JOURNAL"

    OPSLOG="$TEST_DIR/asyncpctfresh.opslog"

    run_elbencho asyncpctfresh \
        -w -t 1 -s $MIX_SIZE -b $BLOCK_SIZE --iodepth 8 --rwmixpct $READ_PERCENT \
        --journaldir "$ASYNC_JOURNAL" \
        --opslog "$OPSLOG" \
        "$ASYNC_TARGET"
    assert_ok $? "an asynchronous read/write mix on a fresh journal succeeds"
    assert_eq "$(opslog_count "$OPSLOG" aioread)" "0" \
        "no asynchronous read happened, every read target was uninitialized"
    assert_eq "$(opslog_count "$OPSLOG" aiowrite)" "$MIX_OPS" \
        "all $MIX_OPS asynchronous operations became initializing writes"

    OPSLOG="$TEST_DIR/asyncpctinit.opslog"

    run_elbencho asyncpctinit \
        -w -t 1 -s $MIX_SIZE -b $BLOCK_SIZE --iodepth 8 --rwmixpct $READ_PERCENT \
        --journaldir "$ASYNC_JOURNAL" \
        --opslog "$OPSLOG" \
        "$ASYNC_TARGET"
    assert_ok $? "the same asynchronous mix on the initialized journal succeeds"
    assert_eq "$(opslog_count "$OPSLOG" aioread)" "$MIX_READS" \
        "exactly $MIX_READS asynchronous operations were reads this time"
    assert_eq "$(opslog_count "$OPSLOG" aiowrite)" "$MIX_WRITES" \
        "exactly $MIX_WRITES asynchronous operations were writes this time"

else
    tap_skip "an asynchronous read/write mix on a fresh journal succeeds"
    tap_skip "no asynchronous read happened, every read target was uninitialized"
    tap_skip "all asynchronous operations became initializing writes"
    tap_skip "the same asynchronous mix on the initialized journal succeeds"
    tap_skip "the expected number of asynchronous operations were reads this time"
    tap_skip "the expected number of asynchronous operations were writes this time"
fi
