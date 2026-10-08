#!/bin/bash
#
# S3-over-RDMA from and to GPU memory via an s3rdma plugin.
#
# Uploads objects from GPU buffers and downloads them into GPU buffers, with a
# data integrity check (elbencho fills and verifies the GPU buffers through host
# copies). With the s3rdma_minio plugin the RDMA transfers move the data directly
# between the GPU memory and the server ("--gpuids"), as single-part objects and
# as multipart upload with ranged download. With the s3rdma_cloudian plugin two
# modes are covered: GPU data staged through host buffers, which are
# what the RDMA transfers use ("--gpuids"), and GPU-direct through GPUDirect
# Storage buffers ("--gds --gpuids"), each as multipart upload and ranged
# download. All of this needs NVIDIA's cuObject library (the client shim handles
# host memory only), a GPU, and for the GPU-direct modes a working GPUDirect RDMA
# path between the GPU and the NIC. The buffers are small (a few MiB per thread),
# so that the GPU can be in use by other programs at the same time.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/minio.sh" || exit 1
source "$ELBENCHO_TEST_LIB/plugin_s3rdma.sh" || exit 1

NUM_THREADS=2
NUM_OBJECTS=2   # per thread

EXPECTED_OBJECTS=$((NUM_THREADS * NUM_OBJECTS))

require_s3rdma_gpu

case "$S3RDMA_PLUGIN" in
    s3rdma_minio) NUM_ROUNDTRIPS=2 ;;
    s3rdma_cloudian) NUM_ROUNDTRIPS=2 ;;
    *) tap_skip_all "no GPU memory test known for plugin \"$S3RDMA_PLUGIN\"" ;;
esac

test_init
tap_plan $((NUM_ROUNDTRIPS * 8))

BUCKET="$(bucket_name)"

start_s3rdma_server
if [ $? -ne 0 ]; then
    tap_bail "Unable to start an S3-over-RDMA test server instance."
fi

# rdma_ops_check PUT|GET EXPECTED DESC
#
# Checks the number of RDMA transfers that the private server logged since the
# last call for that direction, or skips the check against an external server.
RDMA_OPS_SEEN_PUT=0
RDMA_OPS_SEEN_GET=0
rdma_ops_check()
{
    local seen_var="RDMA_OPS_SEEN_$1"
    local total

    if ! s3rdma_server_is_private; then
        tap_skip "$3"
        return 0
    fi

    total="$(s3rdma_server_rdma_ops "$1")"
    assert_eq "$((total - ${!seen_var}))" "$2" "$3"
    eval "$seen_var=$total"
}

# gpu_roundtrip MODE OBJ_SIZE BLOCK_SIZE ARGS...
#
# Uploads the objects from GPU memory with the given extra arguments, downloads
# them into GPU memory with a data integrity check and deletes them again. Every
# block is one RDMA transfer, which the server log has to confirm.
gpu_roundtrip()
{
    local mode="$1"
    local obj_size="$2"
    local block_size="$3"
    shift 3

    local expected_bytes=$((EXPECTED_OBJECTS * obj_size))
    local expected_transfers=$((EXPECTED_OBJECTS * obj_size / block_size))

    run_elbencho "write-$mode" \
        "${S3_OPTS[@]}" "$@" --gpuids "$S3RDMA_GPU_ID" \
        -d -w \
        -t $NUM_THREADS -n 0 -N $NUM_OBJECTS -s $obj_size -b $block_size \
        --verify 1 --blockvarpct 0 \
        "s3://$BUCKET"
    assert_ok $? "$mode: upload of $EXPECTED_OBJECTS objects from GPU memory over RDMA succeeds"

    assert_eq "$(json_value "$ELB_JSON" WRITE last_done entries)" "$EXPECTED_OBJECTS" \
        "$mode: write phase reports $EXPECTED_OBJECTS uploaded objects"
    assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$expected_bytes" \
        "$mode: write phase reports $expected_bytes uploaded bytes"
    rdma_ops_check PUT "$expected_transfers" \
        "$mode: the server log shows $expected_transfers RDMA transfers for the upload"

    run_elbencho "read-$mode" \
        "${S3_OPTS[@]}" "$@" --gpuids "$S3RDMA_GPU_ID" \
        -r \
        -t $NUM_THREADS -n 0 -N $NUM_OBJECTS -s $obj_size -b $block_size \
        --verify 1 \
        "s3://$BUCKET"
    assert_ok $? "$mode: download into GPU memory over RDMA with data integrity check succeeds"

    assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$expected_bytes" \
        "$mode: read phase reports $expected_bytes downloaded bytes"
    rdma_ops_check GET "$expected_transfers" \
        "$mode: the server log shows $expected_transfers RDMA transfers for the download"

    run_elbencho "delete-$mode" \
        "${S3_OPTS[@]}" \
        -F -D --nodelerr \
        -t $NUM_THREADS -n 0 -N $NUM_OBJECTS \
        "s3://$BUCKET"
    assert_ok $? "$mode: delete phase of the objects and the bucket succeeds"
}

case "$S3RDMA_PLUGIN" in
    s3rdma_minio)
        # directly from and to the GPU memory, single-part and multipart
        gpu_roundtrip gpudirect $((4 * 1024 * 1024)) $((4 * 1024 * 1024))
        gpu_roundtrip gpudirect-mpu $((8 * 1024 * 1024)) $((4 * 1024 * 1024))
        ;;
    s3rdma_cloudian)
        gpu_roundtrip hoststaged $((8 * 1024 * 1024)) $((4 * 1024 * 1024))
        gpu_roundtrip gpudirect $((8 * 1024 * 1024)) $((4 * 1024 * 1024)) --gds
        ;;
esac
