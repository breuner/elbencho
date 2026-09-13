#!/bin/bash
#
# Allocated versus apparent file size in file mode.
#
# Covers "--preallocfile", which reserves the disk space of a file up front via
# posix_fallocate(), and the two truncate options "--trunctosize" and "--trunc".
# A partial random write leaves a sparse file behind, which is what makes the
# effect of "--preallocfile" visible: same apparent size, different amount of
# allocated disk space. elbencho itself notices a sparse file when it reads one,
# which is used here as a second, independent check.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1

FILE_SIZE=$((1024 * 1024))
BLOCK_SIZE=$((4 * 1024))
WRITE_AMOUNT=$((64 * 1024))          # only a 16th of the file gets written
FILE_SIZE_KB=$((FILE_SIZE / 1024))

# The pre-existing file for the truncate cases is twice the benchmark file size,
# so that a truncation to the benchmark file size is visible.
LARGE_SIZE=$((2 * FILE_SIZE))

SPARSE_NOTE='File seems sparse or compressed'

test_init
tap_plan 21

DATA_DIR="$TEST_DIR/data"

mkdir -p "$DATA_DIR"

################## Partial random write leaves a sparse file ##################

run_elbencho sparse \
    -w \
    -t 1 -s $FILE_SIZE -b $BLOCK_SIZE --rand --randamount $WRITE_AMOUNT \
    "$DATA_DIR/sparse"
assert_ok $? "partial random write without --preallocfile succeeds"

assert_eq "$(file_size "$DATA_DIR/sparse")" "$FILE_SIZE" \
    "the file has an apparent size of $FILE_SIZE bytes"
# Deliberately relational instead of an exact value, because the exact amount of
# allocated space depends on the file system.
assert_lt "$(allocated_kb "$DATA_DIR/sparse")" "$FILE_SIZE_KB" \
    "less than $FILE_SIZE_KB KiB of disk space are allocated, i.e. the file is sparse"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$WRITE_AMOUNT" \
    "write phase reports the $WRITE_AMOUNT bytes of the given random amount"

run_elbencho sparseread \
    -r \
    -t 1 -s $FILE_SIZE -b $BLOCK_SIZE \
    "$DATA_DIR/sparse"
assert_ok $? "reading the sparse file succeeds"

assert_match "$(cat "$ELB_OUT")" "$SPARSE_NOTE" \
    "elbencho notices that the file is sparse when it reads it"

################## The same write with --preallocfile ##################

run_elbencho prealloc \
    -w \
    -t 1 -s $FILE_SIZE -b $BLOCK_SIZE --rand --randamount $WRITE_AMOUNT \
    --preallocfile \
    "$DATA_DIR/prealloc"
assert_ok $? "partial random write with --preallocfile succeeds"

assert_eq "$(file_size "$DATA_DIR/prealloc")" "$FILE_SIZE" \
    "the preallocated file has the same apparent size of $FILE_SIZE bytes"
assert_ge "$(allocated_kb "$DATA_DIR/prealloc")" "$FILE_SIZE_KB" \
    "at least $FILE_SIZE_KB KiB of disk space are allocated for the whole file"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$WRITE_AMOUNT" \
    "--preallocfile does not change the number of written bytes"

run_elbencho preallocread \
    -r \
    -t 1 -s $FILE_SIZE -b $BLOCK_SIZE \
    "$DATA_DIR/prealloc"
assert_ok $? "reading the preallocated file succeeds"

assert_eq "$(grep -c "$SPARSE_NOTE" "$ELB_OUT")" "0" \
    "the preallocated file is not reported as sparse"

################## Truncation of an existing larger file ##################

truncate -s $LARGE_SIZE "$DATA_DIR/trunctosize"
truncate -s $LARGE_SIZE "$DATA_DIR/trunc"
truncate -s $LARGE_SIZE "$DATA_DIR/keepsize"

run_elbencho trunctosize \
    -w \
    -t 1 -s $FILE_SIZE -b $FILE_SIZE --trunctosize \
    "$DATA_DIR/trunctosize"
assert_ok $? "write phase with --trunctosize succeeds"

assert_eq "$(file_size "$DATA_DIR/trunctosize")" "$FILE_SIZE" \
    "--trunctosize shrank the existing file to the given file size"
# Only "--trunc" is reported, so this is what tells the two options apart.
assert_eq "$(json_config "$ELB_JSON" WRITE truncate_files)" "false" \
    "--trunctosize is not reported as truncate_files in the json result file"

run_elbencho trunc \
    -w \
    -t 1 -s $FILE_SIZE -b $FILE_SIZE --trunc \
    "$DATA_DIR/trunc"
assert_ok $? "write phase with --trunc succeeds"

assert_eq "$(file_size "$DATA_DIR/trunc")" "$FILE_SIZE" \
    "--trunc left the file at the given file size"
assert_eq "$(json_config "$ELB_JSON" WRITE truncate_files)" "true" \
    "--trunc is reported as truncate_files in the json result file"

run_elbencho keepsize \
    -w \
    -t 1 -s $FILE_SIZE -b $FILE_SIZE \
    "$DATA_DIR/keepsize"
assert_ok $? "write phase without a truncate option succeeds"

assert_eq "$(file_size "$DATA_DIR/keepsize")" "$LARGE_SIZE" \
    "without a truncate option the existing file keeps its larger size"
assert_eq "$(json_config "$ELB_JSON" WRITE truncate_files)" "false" \
    "no truncation is reported in the json result file"
