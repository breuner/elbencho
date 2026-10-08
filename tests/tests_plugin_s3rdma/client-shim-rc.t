#!/bin/bash
#
# The cuObject-style S3-over-RDMA protocol over RC: test client with the cuObject client shim.
#
# tools/s3rdma/client speaks exactly what elbencho's s3rdma plugins speak (one
# body-less request per object with the x-amz-rdma-token header), through the
# cuObject client API. Here it is linked against tools/s3rdma/cuobjclient-shim, which
# implements that API over Reliable Connections instead of cuObject's DC
# transport, so the whole path runs on a RoCE NIC without DC support and without
# a GPU. The server's RC transport chunks the objects through its staging
# buffers, which the small buffer size below forces for every object. That the
# data moved via RDMA is confirmed by the stored files, the server log and the
# NIC's counters of RDMA read (upload) and write (download) requests.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/minio.sh" || exit 1
source "$ELBENCHO_TEST_LIB/plugin_s3rdma.sh" || exit 1

NUM_OBJECTS=3
OBJ_SIZE=$((3 * 1024 * 1024 + 4096))   # not a multiple of the staging buffer size
RANGE_SIZE=$((64 * 1024))
RANGE_OFFSET=$((1024 * 1024 + 512))

require_s3rdma_client_rc

test_init
tap_plan 16

BUCKET="$(bucket_name)"

# small staging buffers, so that every object needs several RDMA chunks
S3RDMA_SERVER_BUFSIZE="1m"

start_s3rdma_server
if [ $? -ne 0 ]; then
    tap_bail "Unable to start an S3-over-RDMA test server instance."
fi

RDMA_DEV="$(rdma_device_for_ipv4 "$S3RDMA_ADDR")"

# counter_check COUNTER BEFORE DESC
#
# Checks that the given NIC counter grew beyond BEFORE, or skips the check if the
# counter is not available on this NIC.
counter_check()
{
    local after="$(rdma_hw_counter "$RDMA_DEV" "$1")"

    if [ -z "$2" ] || [ -z "$after" ]; then
        tap_skip "$3"
    else
        assert_gt "$after" "$2" "$3"
    fi
}

################## Upload over RDMA ##################

READS_BEFORE="$(rdma_hw_counter "$RDMA_DEV" rx_read_requests)"

s3rdma_client --bucket "$BUCKET" --mkbucket --put --size $OBJ_SIZE --count $NUM_OBJECTS
assert_ok $? "bucket creation and upload of $NUM_OBJECTS objects over RDMA succeed"

assert_eq "$(count_files "$S3RDMA_DATA_DIR/$BUCKET")" "$NUM_OBJECTS" \
    "the server stored $NUM_OBJECTS object files"
assert_eq "$(unique_file_sizes "$S3RDMA_DATA_DIR/$BUCKET")" "$OBJ_SIZE" \
    "all stored object files have a size of $OBJ_SIZE bytes"
assert_eq "$(s3_object_size "$BUCKET" obj1)" "$OBJ_SIZE" \
    "a head request via the aws cli reports a size of $OBJ_SIZE bytes"
assert_eq "$(s3rdma_server_rdma_ops PUT)" "$NUM_OBJECTS" \
    "the server log shows $NUM_OBJECTS RDMA transfers for the uploads"

counter_check rx_read_requests "$READS_BEFORE" \
    "the NIC counted RDMA read requests during the upload"

################## Download over RDMA ##################

WRITES_BEFORE="$(rdma_hw_counter "$RDMA_DEV" rx_write_requests)"

s3rdma_client --bucket "$BUCKET" --get --verify --size $OBJ_SIZE --count $NUM_OBJECTS
assert_ok $? "download of $NUM_OBJECTS objects over RDMA with data verification succeeds"

assert_eq "$(s3rdma_server_rdma_ops GET)" "$NUM_OBJECTS" \
    "the server log shows $NUM_OBJECTS RDMA transfers for the downloads"

counter_check rx_write_requests "$WRITES_BEFORE" \
    "the NIC counted RDMA write requests during the download"

################## Ranged download over RDMA ##################

s3rdma_client --bucket "$BUCKET" --key obj --get --verify --size $RANGE_SIZE \
    --offset $RANGE_OFFSET --count $NUM_OBJECTS
assert_ok $? "ranged download of $RANGE_SIZE bytes at offset $RANGE_OFFSET verifies"

assert_eq "$(grep -c "RDMA GET .* (range $RANGE_OFFSET-$((RANGE_OFFSET + RANGE_SIZE - 1)))" \
    "$S3RDMA_LOG")" "$NUM_OBJECTS" \
    "the server log shows $NUM_OBJECTS RDMA transfers for exactly that range"

################## Connections are cleaned up ##################

assert_eq "$(grep -c '^RC connect:' "$S3RDMA_LOG")" "$((3 * NUM_OBJECTS))" \
    "the server log shows one RC connection per transfer"
assert_eq "$(grep -c '^RC disconnect:' "$S3RDMA_LOG")" "$((3 * NUM_OBJECTS))" \
    "the server log shows that every connection was released again"

################## Delete the objects and the bucket ##################

s3rdma_client --bucket "$BUCKET" --delete --rmbucket --count $NUM_OBJECTS
assert_ok $? "deletion of the objects and the bucket succeeds"

assert_eq "$(count_files "$S3RDMA_DATA_DIR/$BUCKET")" "0" \
    "no object files are left on the server"

if s3_bucket_exists "$BUCKET"; then
    tap_fail "the aws cli tool confirms that the bucket is gone"
else
    tap_ok "the aws cli tool confirms that the bucket is gone"
fi
