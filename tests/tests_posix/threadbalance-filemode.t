#!/bin/bash
#
# Balanced block ranges of the worker threads.
#
# In file mode and for shared files in custom tree mode, the blocks of all files
# form one block space that is split into one contiguous range per thread. When
# the block count is not a multiple of the thread count, the extra blocks go to
# the first threads one by one, so no thread transfers more than one block more
# than any other (instead of the last thread getting the whole remainder). This
# test verifies the per-thread balance through the operations log, that the full
# dataset is still transferred, and that it reads back intact.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1

NUM_THREADS=4
BLOCK_SIZE=65536
# 19 blocks over 4 threads: 5, 5, 5 and 4 blocks per thread.
FILE_SIZE=$((19 * BLOCK_SIZE))
# The same with a partial last block: 20 blocks, the last one 4 KiB, so every
# thread gets 5 blocks and the last thread ends with the partial block.
PARTIAL_FILE_SIZE=$((FILE_SIZE + 4096))
# Custom tree mode: two files above the share size with 11 + 8 = 19 blocks.
SHARE_SIZE=$((2 * BLOCK_SIZE))
TREE_FILE1_SIZE=$((11 * BLOCK_SIZE))
TREE_FILE2_SIZE=$((8 * BLOCK_SIZE))
TREE_BYTES=$((TREE_FILE1_SIZE + TREE_FILE2_SIZE))

test_init
tap_plan 26

DATA_DIR="$TEST_DIR/data"
TREE_DIR="$TEST_DIR/tree"
TREEFILE="$TEST_DIR/tree.txt"
OPSLOG="$TEST_DIR/write.opslog"
PARTIAL_OPSLOG="$TEST_DIR/write-partial.opslog"
TREE_OPSLOG="$TEST_DIR/write-tree.opslog"

mkdir -p "$DATA_DIR" "$TREE_DIR"

################## Shared file with a block remainder ##################

run_elbencho write \
    -w \
    -t $NUM_THREADS -s $FILE_SIZE -b $BLOCK_SIZE \
    --verify 1 --blockvarpct 0 \
    --opslog "$OPSLOG" \
    "$DATA_DIR/file"
assert_ok $? "write phase of a shared file with a block remainder succeeds"

assert_eq "$(file_size "$DATA_DIR/file")" "$FILE_SIZE" \
    "the file has a size of $FILE_SIZE bytes"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$FILE_SIZE" \
    "write phase reports $FILE_SIZE written bytes"

assert_eq "$(opslog_error_count "$OPSLOG")" "0" \
    "operations log contains no failed operation"
assert_eq "$(opslog_num_ranks "$OPSLOG" pwrite)" "$NUM_THREADS" \
    "operations log confirms that all $NUM_THREADS threads wrote a part of the file"
assert_le "$(opslog_rank_bytes_spread "$OPSLOG" pwrite)" "$BLOCK_SIZE" \
    "no thread wrote more than one block more than any other thread"

run_elbencho read \
    -r \
    -t $NUM_THREADS -s $FILE_SIZE -b $BLOCK_SIZE \
    --verify 1 \
    "$DATA_DIR/file"
assert_ok $? "read phase with data integrity check succeeds"

assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$FILE_SIZE" \
    "read phase reports $FILE_SIZE read bytes"

################## Shared file with a partial last block ##################

run_elbencho writepartial \
    -w \
    -t $NUM_THREADS -s $PARTIAL_FILE_SIZE -b $BLOCK_SIZE \
    --verify 1 --blockvarpct 0 \
    --opslog "$PARTIAL_OPSLOG" \
    "$DATA_DIR/partial"
assert_ok $? "write phase of a shared file with a partial last block succeeds"

assert_eq "$(file_size "$DATA_DIR/partial")" "$PARTIAL_FILE_SIZE" \
    "the file has a size of $PARTIAL_FILE_SIZE bytes"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$PARTIAL_FILE_SIZE" \
    "write phase reports $PARTIAL_FILE_SIZE written bytes"

assert_eq "$(opslog_error_count "$PARTIAL_OPSLOG")" "0" \
    "operations log contains no failed operation"
assert_eq "$(opslog_num_ranks "$PARTIAL_OPSLOG" pwrite)" "$NUM_THREADS" \
    "operations log confirms that all $NUM_THREADS threads wrote a part of the file"
assert_le "$(opslog_rank_bytes_spread "$PARTIAL_OPSLOG" pwrite)" "$BLOCK_SIZE" \
    "no thread wrote more than one block more than any other thread"

run_elbencho readpartial \
    -r \
    -t $NUM_THREADS -s $PARTIAL_FILE_SIZE -b $BLOCK_SIZE \
    --verify 1 \
    "$DATA_DIR/partial"
assert_ok $? "read phase with data integrity check succeeds"

assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$PARTIAL_FILE_SIZE" \
    "read phase reports $PARTIAL_FILE_SIZE read bytes"

################## Shared files in custom tree mode ##################

# Treefile format: "d <relative path>" for dirs, "f <size> <relative path>" for
# files. Both files are at least SHARE_SIZE bytes, so both get split between
# the threads.
cat > "$TREEFILE" <<TREEFILE_EOF
d sub
f $TREE_FILE1_SIZE sub/file1
f $TREE_FILE2_SIZE sub/file2
TREEFILE_EOF

run_elbencho treewrite \
    -d -w \
    -t $NUM_THREADS -b $BLOCK_SIZE --sharesize $SHARE_SIZE --treefile "$TREEFILE" \
    --verify 1 --blockvarpct 0 \
    --opslog "$TREE_OPSLOG" \
    "$TREE_DIR"
assert_ok $? "write phase of 2 shared custom tree files succeeds"

assert_eq "$(json_config "$ELB_JSON" WRITE custom_tree_files_shared)" "2" \
    "write phase result reports 2 files above the given share size"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$TREE_BYTES" \
    "write phase reports $TREE_BYTES written bytes"
assert_eq "$(file_size "$TREE_DIR/sub/file1")" "$TREE_FILE1_SIZE" \
    "sub/file1 has a size of $TREE_FILE1_SIZE bytes"
assert_eq "$(file_size "$TREE_DIR/sub/file2")" "$TREE_FILE2_SIZE" \
    "sub/file2 has a size of $TREE_FILE2_SIZE bytes"

assert_eq "$(opslog_error_count "$TREE_OPSLOG")" "0" \
    "operations log contains no failed operation"
assert_eq "$(opslog_num_ranks "$TREE_OPSLOG" pwrite)" "$NUM_THREADS" \
    "operations log confirms that all $NUM_THREADS threads wrote parts of the files"
assert_le "$(opslog_rank_bytes_spread "$TREE_OPSLOG" pwrite)" "$BLOCK_SIZE" \
    "no thread wrote more than one block more than any other thread"

run_elbencho treeread \
    -r \
    -t $NUM_THREADS -b $BLOCK_SIZE --sharesize $SHARE_SIZE --treefile "$TREEFILE" \
    --verify 1 \
    "$TREE_DIR"
assert_ok $? "read phase with data integrity check succeeds"

assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$TREE_BYTES" \
    "read phase reports $TREE_BYTES read bytes"
