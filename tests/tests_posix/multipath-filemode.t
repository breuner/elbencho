#!/bin/bash
#
# Several benchmark paths in file mode.
#
# All given files form one block space that the worker threads divide among
# themselves, so the number of paths, the number of threads and the block size
# together decide how much data is transferred. Covers paths given as separate
# arguments, paths given as a number range in brackets, and the fact that
# random IO drops the blocks that do not divide evenly among the threads while
# sequential IO does not.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1

PAIR_SIZE=$((2 * 1024 * 1024))
PAIR_BLOCK=$((256 * 1024))
PAIR_BYTES=$((2 * PAIR_SIZE))

RANGE_FILES=13
RANGE_SIZE=$((1024 * 1024))
RANGE_BLOCK=$((8 * 1024))
RANGE_BYTES=$((RANGE_FILES * RANGE_SIZE))
RANGE_BLOCKS=$((RANGE_BYTES / RANGE_BLOCK))

# Random IO assigns each thread a whole number of blocks and ignores the
# remainder, so the transferred amount depends on the number of threads.
rand_bytes()
{
    echo $(( (RANGE_BLOCKS / $1) * $1 * RANGE_BLOCK ))
}

test_init
tap_plan 24

PAIR_DIR="$TEST_DIR/pair"
RANGE_DIR="$TEST_DIR/range"
RANGE_PATHS="$RANGE_DIR/testfile[1-$RANGE_FILES]"

mkdir -p "$PAIR_DIR" "$RANGE_DIR"

################## Two paths as separate arguments ##################

run_elbencho pairwrite \
    -w \
    -t 2 -s $PAIR_SIZE -b $PAIR_BLOCK \
    --verify 1 --blockvarpct 0 \
    "$PAIR_DIR/a" "$PAIR_DIR/b"
assert_ok $? "write phase with two benchmark paths as separate arguments succeeds"

assert_eq "$(json_config "$ELB_JSON" WRITE paths)" "2" \
    "write phase result confirms two benchmark paths"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$PAIR_BYTES" \
    "write phase reports $PAIR_BYTES bytes, i.e. the file size for each of the paths"
assert_eq "$(file_size "$PAIR_DIR/a")" "$PAIR_SIZE" \
    "the first path has a size of $PAIR_SIZE bytes"
assert_eq "$(file_size "$PAIR_DIR/b")" "$PAIR_SIZE" \
    "the second path has a size of $PAIR_SIZE bytes"
assert_eq "$(count_files "$PAIR_DIR")" "2" \
    "no other file was created"

run_elbencho pairread \
    -r \
    -t 2 -s $PAIR_SIZE -b $((4 * PAIR_BLOCK)) \
    --verify 1 --blockvarpct 0 \
    "$PAIR_DIR/a" "$PAIR_DIR/b"
assert_ok $? "read phase across both paths with a larger block size succeeds"

assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$PAIR_BYTES" \
    "read phase reports $PAIR_BYTES read bytes"

################## A number range of paths ##################

run_elbencho rangewrite \
    -w \
    -t 4 -s $RANGE_SIZE -b $RANGE_BLOCK \
    "$RANGE_PATHS"
assert_ok $? "write phase with a number range of $RANGE_FILES paths succeeds"

assert_eq "$(json_config "$ELB_JSON" WRITE paths)" "$RANGE_FILES" \
    "write phase result confirms $RANGE_FILES benchmark paths"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$RANGE_BYTES" \
    "write phase reports $RANGE_BYTES written bytes"
assert_eq "$(count_files "$RANGE_DIR")" "$RANGE_FILES" \
    "the number range created $RANGE_FILES files"
assert_eq "$(unique_file_sizes "$RANGE_DIR")" "$RANGE_SIZE" \
    "all created files have a size of $RANGE_SIZE bytes"

################## Threads that do not divide the blocks evenly ##################

# 13 files of 1MiB in 8KiB blocks are 1664 blocks, which is divisible by 4 but
# not by 3, 5 or 7.
run_elbencho seqread \
    -r \
    -t 3 -s $RANGE_SIZE -b $RANGE_BLOCK \
    "$RANGE_PATHS"
assert_ok $? "sequential read with 3 threads succeeds"

assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$RANGE_BYTES" \
    "sequential read transfers all $RANGE_BYTES bytes despite the uneven split"

for numthreads in 3 5 7; do
    expected_bytes="$(rand_bytes $numthreads)"

    run_elbencho "randread$numthreads" \
        -r --rand \
        -t $numthreads -s $RANGE_SIZE -b $RANGE_BLOCK \
        "$RANGE_PATHS"
    assert_ok $? "random read with $numthreads threads succeeds"

    assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$expected_bytes" \
        "random read with $numthreads threads transfers $expected_bytes bytes"
done

assert_gt "$(json_value "$ELB_JSON" READ last_done iops)" 0 \
    "read phase reports a non-zero IOPS value"

################## Delete with more threads than paths ##################

run_elbencho delete \
    -F --nodelerr \
    -t 6 \
    "$RANGE_PATHS"
assert_ok $? "delete phase with more threads than paths succeeds"

assert_eq "$(count_files "$RANGE_DIR")" "0" \
    "all $RANGE_FILES files were deleted"
