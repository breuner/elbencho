#!/bin/bash
#
# Mixed file sizes in custom tree mode, including a file shared by multiple
# threads.
#
# Custom tree mode takes the dirs, file names and file sizes from a treefile.
# Files of at least "--sharesize" bytes are not exclusively assigned to a single
# thread, but get split into block ranges that multiple threads work on. This
# test verifies the resulting tree, the custom tree counters of the json result
# file and, through the operations log, that the large file really was accessed
# by more than one thread.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1

NUM_THREADS=4
BLOCK_SIZE=65536
SHARE_SIZE=$((1024 * 1024))

# Sum of all file sizes in the treefile below.
EXPECTED_BYTES=$((1024 + 4096 + 65536 + 1048576 + 16777216))
EXPECTED_FILES=5
EXPECTED_DIRS=2
# Files of at least SHARE_SIZE bytes: sub2/big1 and sub2/huge1
EXPECTED_SHARED=2
EXPECTED_NOT_SHARED=3
# The write phase file counter only counts files that have not been split across
# multiple workers. That is the 3 non-shared files plus sub2/big1, whose 16
# blocks all end up in the block range of the first worker.
EXPECTED_FILE_ENTRIES=4

test_init
tap_plan 19

DATA_DIR="$TEST_DIR/data"
TREEFILE="$TEST_DIR/tree.txt"
OPSLOG="$TEST_DIR/write.opslog"

mkdir -p "$DATA_DIR"

# Treefile format: "d <relative path>" for dirs, "f <size> <relative path>" for
# files. All paths are relative to the benchmark dir.
cat > "$TREEFILE" <<TREEFILE_EOF
d sub1
d sub2
f 1024 sub1/tiny1
f 4096 sub1/small1
f 65536 sub2/medium1
f 1048576 sub2/big1
f 16777216 sub2/huge1
TREEFILE_EOF

EXPECTED_LISTING="sub1/small1 4096
sub1/tiny1 1024
sub2/big1 1048576
sub2/huge1 16777216
sub2/medium1 65536"

################## Create dirs and write files ##################

run_elbencho write \
    -d -w \
    -t $NUM_THREADS -b $BLOCK_SIZE --sharesize $SHARE_SIZE --treefile "$TREEFILE" \
    --verify 1 --blockvarpct 0 \
    --opslog "$OPSLOG" \
    "$DATA_DIR"
assert_ok $? "write phase of $EXPECTED_FILES files with mixed sizes succeeds"

assert_eq "$(json_value "$ELB_JSON" MKDIRS last_done entries)" "$EXPECTED_DIRS" \
    "mkdirs phase reports $EXPECTED_DIRS created dirs"
assert_eq "$(json_config "$ELB_JSON" WRITE custom_tree_dirs)" "$EXPECTED_DIRS" \
    "write phase result reports $EXPECTED_DIRS custom tree dirs"
assert_eq "$(json_config "$ELB_JSON" WRITE custom_tree_files_shared)" "$EXPECTED_SHARED" \
    "write phase result reports $EXPECTED_SHARED files above the given share size"
assert_eq "$(json_config "$ELB_JSON" WRITE files_not_shared)" "$EXPECTED_NOT_SHARED" \
    "write phase result reports $EXPECTED_NOT_SHARED files below the given share size"

assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$EXPECTED_BYTES" \
    "write phase reports $EXPECTED_BYTES written bytes"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done entries)" "$EXPECTED_FILE_ENTRIES" \
    "write phase counts the $EXPECTED_FILE_ENTRIES files that were not split across workers"

assert_eq "$(file_listing "$DATA_DIR")" "$EXPECTED_LISTING" \
    "all files from the treefile exist with exactly the given sizes"
assert_eq "$(count_dirs "$DATA_DIR")" "$EXPECTED_DIRS" \
    "$EXPECTED_DIRS dirs from the treefile exist on the file system"

assert_eq "$(opslog_error_count "$OPSLOG")" "0" \
    "operations log contains no failed operation"
assert_ge "$(opslog_distinct_ranks "$OPSLOG" sub2/huge1)" "2" \
    "operations log confirms that the largest file was shared by multiple threads"
assert_eq "$(opslog_distinct_ranks "$OPSLOG" sub1/tiny1)" "1" \
    "operations log confirms that a small file was handled by a single thread"

################## Read files back ##################

run_elbencho read \
    -r \
    -t $NUM_THREADS -b $BLOCK_SIZE --sharesize $SHARE_SIZE --treefile "$TREEFILE" \
    --verify 1 \
    "$DATA_DIR"
assert_ok $? "read phase with data integrity check succeeds"

assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$EXPECTED_BYTES" \
    "read phase reports $EXPECTED_BYTES read bytes"

################## Read again with round-robin block assignment ##################

run_elbencho readroundrob \
    -r \
    -t $NUM_THREADS -b $BLOCK_SIZE --sharesize $SHARE_SIZE --treefile "$TREEFILE" \
    --treeroundrob --verify 1 \
    "$DATA_DIR"
assert_ok $? "read phase with \"--treeroundrob\" and data integrity check succeeds"

assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$EXPECTED_BYTES" \
    "round-robin read phase also reports $EXPECTED_BYTES read bytes"

################## Delete files and dirs ##################

run_elbencho delete \
    -F -D --nodelerr \
    -t $NUM_THREADS -b $BLOCK_SIZE --sharesize $SHARE_SIZE --treefile "$TREEFILE" \
    "$DATA_DIR"
assert_ok $? "delete phase of files and dirs succeeds"

assert_eq "$(count_files "$DATA_DIR")" "0" \
    "no files are left after the delete phase"
assert_eq "$(count_dirs "$DATA_DIR")" "0" \
    "no dirs are left after the delete phase"
