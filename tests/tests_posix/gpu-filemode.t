#!/bin/bash
#
# File writes and reads through GPU memory, with data verification.
#
# Two rounds over shared files in file mode, each a write with "--verify" and a
# read back with the integrity check: first with the GPU buffers only
# ("--gpuids", elbencho copies between its host buffers and the GPU around each
# file I/O), then with GPU-direct storage access ("--gpudirect", the GPU-direct
# file API cuFile or hipFile moves the data between the file and the GPU memory
# without a host copy). The operations log of the read phase confirms which I/O
# call was used. Skips without a GPU build or a GPU; the GPU-direct round skips
# without the file API in the build or without direct IO on the test file system.
# The vendor's management tool lists the GPU (nvidia-smi for CUDA, amd-smi for
# HIP); ELBENCHO_TEST_GPU_ID names it where that tool is missing. The buffers
# are small (a few MiB per thread), so that the GPU can be in use by other
# programs at the same time.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1

NUM_THREADS=2
FILE_SIZE=$((4 * 1024 * 1024))
BLOCK_SIZE=$((1024 * 1024))

EXPECTED_FILES=2
EXPECTED_BYTES=$((EXPECTED_FILES * FILE_SIZE))
EXPECTED_BLOCKS=$((EXPECTED_BYTES / BLOCK_SIZE))

if has_build_feature cuda; then
    GPUFILE_FEATURE="cufile/gds"
    GPUFILE_READ_OP="cuFileRead"
    GPU_ID="${ELBENCHO_TEST_GPU_ID:-$(nvidia-smi --query-gpu=index --format=csv,noheader 2>/dev/null | head -n1)}"
    GPU_TOOL="nvidia-smi"
elif has_build_feature hip; then
    GPUFILE_FEATURE="hipfile"
    GPUFILE_READ_OP="hipFileRead"
    GPU_ID="${ELBENCHO_TEST_GPU_ID:-$(amd-smi list --csv 2>/dev/null | awk -F, 'NR == 2 { print $1 }')}"
    GPU_TOOL="amd-smi"
else
    tap_skip_all "elbencho was built without GPU support (CUDA/HIP)"
fi

if [ -z "$GPU_ID" ]; then
    tap_skip_all "no GPU found on this host ($GPU_TOOL; set ELBENCHO_TEST_GPU_ID)"
fi

test_init
tap_plan 10

DATA_DIR="$TEST_DIR/data"

mkdir -p "$DATA_DIR"

# Quoted, so that elbencho itself expands the number range in the brackets.
BENCH_PATHS="$DATA_DIR/file[1-2]"

# gpu_roundtrip MODE READ_OP ARGS...
#
# Writes the files from GPU memory with the given extra arguments and reads them
# back into GPU memory with a data integrity check. The read phase has to use the
# given I/O call for every block.
gpu_roundtrip()
{
    local mode="$1"
    local read_op="$2"
    shift 2

    local opslog="$TEST_DIR/read-$mode.opslog"

    run_elbencho "write-$mode" \
        "$@" --gpuids "$GPU_ID" \
        -w \
        -t $NUM_THREADS -s $FILE_SIZE -b $BLOCK_SIZE \
        --verify 1 --blockvarpct 0 \
        "$BENCH_PATHS"
    assert_ok $? "$mode: write phase of $EXPECTED_FILES files from GPU memory succeeds"

    assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$EXPECTED_BYTES" \
        "$mode: write phase reports $EXPECTED_BYTES written bytes"

    run_elbencho "read-$mode" \
        "$@" --gpuids "$GPU_ID" \
        -r \
        -t $NUM_THREADS -s $FILE_SIZE -b $BLOCK_SIZE \
        --verify 1 \
        --opslog "$opslog" \
        "$BENCH_PATHS"
    assert_ok $? "$mode: read phase into GPU memory with data integrity check succeeds"

    assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$EXPECTED_BYTES" \
        "$mode: read phase reports $EXPECTED_BYTES read bytes"
    assert_eq "$(opslog_count "$opslog" "$read_op")" "$EXPECTED_BLOCKS" \
        "$mode: operations log shows $EXPECTED_BLOCKS completed $read_op calls"
}

################## GPU buffers with host copies ##################

gpu_roundtrip gpucopy pread

################## GPU-direct storage access ##################

if ! has_build_feature "$GPUFILE_FEATURE"; then
    gpudirect_skip="elbencho was built without \"$GPUFILE_FEATURE\" support"
elif ! fs_supports_directio "$DATA_DIR"; then
    gpudirect_skip="the test file system does not support direct IO"
fi

if [ -n "$gpudirect_skip" ]; then
    for i in 1 2 3 4 5; do
        tap_skip "gpudirect: $gpudirect_skip"
    done
else
    gpu_roundtrip gpudirect "$GPUFILE_READ_OP" --gpudirect
fi
