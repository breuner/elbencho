#!/bin/bash
#
# Resuming an existing journal.
#
# A journal outlives the process that created it - that is what makes it usable
# after a crash - so elbencho scans the journal dir on every start and resumes
# what it finds. A journal is matched to a target by exact string equality
# between the sidecar's "target" field and the benchmark path as it was typed on
# the command line, which is also what allows a journal to be pointed at a
# renamed target by editing that one field.
#
# The resumed journal has to tolerate a different block size and even a
# different block size mix, because the tracked block size is a property of the
# journal and not of the IO that happens to use it.
#
# The error cases get one journal dir each, because elbencho parses every
# sidecar in the dir it is given, so a deliberately broken one would otherwise
# poison the runs after it.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/journal.sh" || exit 1

FILE_SIZE=$((4 * 1024 * 1024))
SMALL_BLOCK=4096
BIG_BLOCK=$((1024 * 1024))
BLOCK_MIX="4K:1,64K:1"
JOURNAL_BLOCK=4096

test_init
tap_plan 46

DATA_DIR="$TEST_DIR/data"
mkdir -p "$DATA_DIR"

TARGET1="$DATA_DIR/file1"
TARGET2="$DATA_DIR/file2"
JOURNAL_DIR="$TEST_DIR/journal"
mkdir -p "$JOURNAL_DIR"

################## A second run resumes instead of creating ##################

run_elbencho firstwrite \
    -w -t 1 -s $FILE_SIZE -b $SMALL_BLOCK \
    --journaldir "$JOURNAL_DIR" \
    "$TARGET1"
assert_ok $? "the first journaled write creates the journal"
assert_match "$(cat "$ELB_OUT")" 'Created new journal for target' \
    "the first run reports that it created a journal"

JOURNAL_SEED="$(journal_field "$JOURNAL_DIR" "$TARGET1" journalSeed)"

run_elbencho secondwrite \
    -w -t 1 -s $FILE_SIZE -b $SMALL_BLOCK \
    --journaldir "$JOURNAL_DIR" \
    "$TARGET1"
assert_ok $? "a second journaled write of the same target succeeds"
assert_match "$(cat "$ELB_OUT")" 'Resumed existing journal for target' \
    "the second run reports that it resumed the journal"
assert_eq "$(journal_sidecar_count "$JOURNAL_DIR")" "1" \
    "no second journal was created for the same target"
# The seed determines the expected content, so a resumed journal that came back
# with a new seed would make every later verification fail.
assert_eq "$(journal_field "$JOURNAL_DIR" "$TARGET1" journalSeed)" "$JOURNAL_SEED" \
    "the resumed journal kept its seed"

################## Resuming with a different block size and mix ##################

run_elbencho bigblockread \
    -r -t 1 -s $FILE_SIZE -b $BIG_BLOCK \
    --journaldir "$JOURNAL_DIR" \
    "$TARGET1"
assert_ok $? "the journal can be resumed with a much larger block size"
assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$FILE_SIZE" \
    "the larger block size read the whole range"

# A block size mix is drawn unseeded, so the number of blocks per size is not
# predictable - the total amount is.
run_elbencho mixwrite \
    -w -t 1 -s $FILE_SIZE -b "$BLOCK_MIX" \
    --journaldir "$JOURNAL_DIR" \
    "$TARGET1"
assert_ok $? "the journal can be resumed with a block size mix"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$FILE_SIZE" \
    "the block size mix write covered the whole range"

run_elbencho mixread \
    -r -t 1 -s $FILE_SIZE -b "$BLOCK_MIX" \
    --journaldir "$JOURNAL_DIR" \
    "$TARGET1"
assert_ok $? "content written with a block size mix verifies with a mix as well"

run_elbencho mixreaduniform \
    -r -t 1 -s $FILE_SIZE -b $SMALL_BLOCK \
    --journaldir "$JOURNAL_DIR" \
    "$TARGET1"
assert_ok $? "and it verifies with a uniform block size too"
assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$FILE_SIZE" \
    "so the block size mix really initialized every block"

################## Adding a target to an existing journal dir ##################

run_elbencho twotargets \
    -w -t 2 -s $FILE_SIZE -b $SMALL_BLOCK \
    --journaldir "$JOURNAL_DIR" \
    "$TARGET1" "$TARGET2"
assert_ok $? "a run with an additional target succeeds"

assert_match "$(cat "$ELB_OUT")" "Resumed existing journal for target. Target: $TARGET1" \
    "the known target's journal was resumed"
assert_match "$(cat "$ELB_OUT")" "Created new journal for target. Target: $TARGET2" \
    "a new journal was created for the added target"
assert_eq "$(journal_sidecar_count "$JOURNAL_DIR")" "2" \
    "the journal dir now holds two journals"
assert_eq "$(journal_file_count "$JOURNAL_DIR")" "4" \
    "each of the two journals has a sidecar and a binary file"
assert_eq "$(journal_targets "$JOURNAL_DIR")" "$TARGET1 $TARGET2" \
    "the two journals name the two targets"

run_elbencho twotargetsread \
    -r -t 2 -s $FILE_SIZE -b $SMALL_BLOCK \
    --journaldir "$JOURNAL_DIR" \
    "$TARGET1" "$TARGET2"
assert_ok $? "both targets verify against their own journal"
assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$((2 * FILE_SIZE))" \
    "both targets were read completely"

################## Resuming with fewer targets than before ##################

run_elbencho onetarget \
    -r -t 1 -s $FILE_SIZE -b $SMALL_BLOCK \
    --journaldir "$JOURNAL_DIR" \
    "$TARGET1"
assert_ok $? "a run with only one of the two targets succeeds"
assert_eq "$(journal_sidecar_count "$JOURNAL_DIR")" "2" \
    "the unused target's journal is left in place"

################## Pointing a journal at a renamed target ##################

# This is what the sidecar exists for: a device that comes back under a
# different name, or a file that was moved, keeps its journal.
RENAMED="$DATA_DIR/file2-renamed"
mv "$TARGET2" "$RENAMED"

journal_remap_target "$JOURNAL_DIR" "$TARGET2" "$RENAMED"
assert_ok $? "the sidecar of the moved target can be pointed at its new path"

run_elbencho remapread \
    -r -t 1 -s $FILE_SIZE -b $SMALL_BLOCK \
    --journaldir "$JOURNAL_DIR" \
    "$RENAMED"
assert_ok $? "the moved target verifies against its re-mapped journal"
assert_match "$(cat "$ELB_OUT")" 'Resumed existing journal for target' \
    "the re-mapped journal was resumed rather than replaced"
assert_eq "$(journal_sidecar_count "$JOURNAL_DIR")" "2" \
    "re-mapping did not create an additional journal"

################## The resumed journal reports how much of it is usable ##################

# The percentage in the resume note counts the journal blocks that hold a
# content generation, so it is exactly predictable: create the journal over the
# whole range without writing anything, then fill exactly half of it, then all
# of it. With a 4 KiB journal block and an 8 MiB range that is 2048 blocks, so
# half of them is an exact 50.0%.
PCT_TARGET="$DATA_DIR/pctfile"
PCT_JOURNAL="$TEST_DIR/journal-percent"
PCT_SIZE=$((8 * 1024 * 1024))
mkdir -p "$PCT_JOURNAL"

# Create the target and its journal without journaling any content: a read
# phase creates the journal for the whole range and skips every block.
run_elbencho pctprefill -w -t 1 -s $PCT_SIZE -b $SMALL_BLOCK "$PCT_TARGET"
assert_ok $? "the target for the percentage checks is created"

run_elbencho pctcreate \
    -r -t 1 -s $PCT_SIZE -b $SMALL_BLOCK --journaldir "$PCT_JOURNAL" "$PCT_TARGET"
assert_ok $? "a read phase creates the journal for the whole range"

run_elbencho pctempty \
    -r -t 1 -s $PCT_SIZE -b $SMALL_BLOCK --journaldir "$PCT_JOURNAL" "$PCT_TARGET"
assert_eq "$(journal_resumed_percent "$ELB_OUT")" "0.0" \
    "a journal that was never written reports 0.0% verifiable"

run_elbencho pcthalfwrite \
    -w -t 1 -s $((PCT_SIZE / 2)) -b $SMALL_BLOCK --offset 0 \
    --journaldir "$PCT_JOURNAL" "$PCT_TARGET"
assert_ok $? "writing exactly half of the journaled range succeeds"

run_elbencho pcthalf \
    -r -t 1 -s $PCT_SIZE -b $SMALL_BLOCK --journaldir "$PCT_JOURNAL" "$PCT_TARGET"
assert_eq "$(journal_resumed_percent "$ELB_OUT")" "50.0" \
    "a half written journal reports 50.0% verifiable"

run_elbencho pctfullwrite \
    -w -t 1 -s $PCT_SIZE -b $SMALL_BLOCK --journaldir "$PCT_JOURNAL" "$PCT_TARGET"
assert_ok $? "writing the whole journaled range succeeds"

run_elbencho pctfull \
    -r -t 1 -s $PCT_SIZE -b $SMALL_BLOCK --journaldir "$PCT_JOURNAL" "$PCT_TARGET"
assert_eq "$(journal_resumed_percent "$ELB_OUT")" "100.0" \
    "a fully written journal reports 100.0% verifiable"

# A newly created journal gets no percentage, because it is 0.0% by definition.
assert_eq "$(journal_resumed_percent "$TEST_DIR/pctcreate.out")" "" \
    "the note for a newly created journal carries no percentage"

################## An interrupted write leaves its blocks unwritten ##################

# A write that is cut off mid-flight must leave the blocks it had claimed marked
# as never written, rather than claiming content that was never fully stored.
# The clean read afterwards is the real assertion here: it shows an interrupted
# write produces skipped blocks and not false verification failures.
# The write is rate limited rather than merely large, because an unthrottled
# local write finishes far more than a second's worth of work in the first
# second and would leave nothing unwritten to observe.
INTR_TARGET="$DATA_DIR/intrfile"
INTR_JOURNAL="$TEST_DIR/journal-interrupted"
INTR_SIZE=$((64 * 1024 * 1024))
INTR_RATE=4m
mkdir -p "$INTR_JOURNAL"

# Create the target at its full size first, without a journal: elbencho rejects
# a read phase whose offset plus size exceeds the target, and the interrupted
# write below stops long before it has extended the file that far.
run_elbencho intrprefill -w -t 2 -s $INTR_SIZE -b $SMALL_BLOCK "$INTR_TARGET"
assert_ok $? "the target for the interrupted write is created at full size"

run_elbencho intrwrite \
    -w -t 2 -s $INTR_SIZE -b $SMALL_BLOCK --timelimit 1 --limitwrite $INTR_RATE \
    --journaldir "$INTR_JOURNAL" \
    "$INTR_TARGET"
assert_ok $? "a journaled write cut off by the time limit ends without an error"

# Nothing reads the unwritten tail: those journal blocks were never initialized,
# so they are skipped rather than read, which is also why this does not run into
# the end of the short file.
run_elbencho intrread \
    -r -t 2 -s $INTR_SIZE -b $SMALL_BLOCK --journaldir "$INTR_JOURNAL" "$INTR_TARGET"
assert_ok $? "reading the interrupted range back reports no verification failure"

INTR_PCT="$(journal_resumed_percent "$ELB_OUT")"
assert_ne "$INTR_PCT" "0.0" \
    "the interrupted write did journal some of its blocks"
assert_ne "$INTR_PCT" "100.0" \
    "but not all of them, so the interruption really left blocks unwritten"

################## Error cases, each in its own journal dir ##################

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

# A different journal block size on resume. All journals of a run have to share
# it, so this cannot be silently adopted.
check_rejected "a resume with a different journal block size" \
    "Existing journal's block size does not match" \
    -r -t 1 -s $FILE_SIZE -b $((2 * JOURNAL_BLOCK)) --journalblock $((2 * JOURNAL_BLOCK)) \
    --journaldir "$JOURNAL_DIR" "$TARGET1"

# Two sidecars claiming the same target. Picking either one would be a guess.
AMBIG_DIR="$TEST_DIR/journal-ambiguous"
mkdir -p "$AMBIG_DIR"
cp "$JOURNAL_DIR"/*.journal "$AMBIG_DIR/" 2>/dev/null
cp "$(journal_sidecar_for "$JOURNAL_DIR" "$TARGET1")" "$AMBIG_DIR/a.journal.json"
cp "$(journal_sidecar_for "$JOURNAL_DIR" "$TARGET1")" "$AMBIG_DIR/b.journal.json"

check_rejected "two journals claiming the same target" \
    'Ambiguous journal sidecars found for target' \
    -r -t 1 -s $FILE_SIZE -b $SMALL_BLOCK --journaldir "$AMBIG_DIR" "$TARGET1"

check_rejected "a journal dir that does not exist" \
    'Journal directory \("--journaldir"\) does not exist or is not a directory' \
    -r -t 1 -s $FILE_SIZE -b $SMALL_BLOCK --journaldir "$TEST_DIR/nosuchdir" "$TARGET1"

# A sidecar that is not valid json at all. Note this is rejected even though it
# belongs to no target of this run: every sidecar in the dir gets parsed, so a
# half written one from a killed run cannot be silently ignored.
BROKEN_DIR="$TEST_DIR/journal-broken"
mkdir -p "$BROKEN_DIR"
echo 'this is not json {' > "$BROKEN_DIR/broken.journal.json"

check_rejected "an unparseable sidecar in the journal dir" \
    'Unable to parse journal sidecar file' \
    -w -t 1 -s $FILE_SIZE -b $SMALL_BLOCK \
    --journaldir "$BROKEN_DIR" "$DATA_DIR/otherfile"

INCOMPLETE_DIR="$TEST_DIR/journal-incomplete"
mkdir -p "$INCOMPLETE_DIR"
cp "$(journal_sidecar_for "$JOURNAL_DIR" "$TARGET1")" "$INCOMPLETE_DIR/x.journal.json"
jq 'del(.journalSeed)' "$INCOMPLETE_DIR/x.journal.json" > "$INCOMPLETE_DIR/x.new" \
    && mv "$INCOMPLETE_DIR/x.new" "$INCOMPLETE_DIR/x.journal.json"

check_rejected "a sidecar with a missing field" \
    'Journal sidecar file is missing a required field' \
    -r -t 1 -s $FILE_SIZE -b $SMALL_BLOCK --journaldir "$INCOMPLETE_DIR" "$TARGET1"

# A sidecar whose binary file is gone, which is what a run killed between the
# two creation steps would leave behind.
ORPHAN_DIR="$TEST_DIR/journal-orphan"
mkdir -p "$ORPHAN_DIR"
cp "$(journal_sidecar_for "$JOURNAL_DIR" "$TARGET1")" "$ORPHAN_DIR/x.journal.json"

check_rejected "a sidecar whose binary journal is missing" \
    'Unable to open existing journal binary file' \
    -r -t 1 -s $FILE_SIZE -b $SMALL_BLOCK --journaldir "$ORPHAN_DIR" "$TARGET1"
