#!/bin/bash
#
# Single-part S3 PUT and GET over RDMA via an s3rdma_* plugin.
#
# Starts a private S3-over-RDMA test server (tools/s3rdma/server) and lets elbencho
# upload and download objects with the plugin active. The block size equals the
# object size, because the s3rdma_minio plugin only supports single-part
# transfers. That the data really moved over RDMA and not in the HTTP body is
# confirmed independently of the plugin: the server only stores what it received
# via RDMA (the control requests carry no body), its log counts the RDMA
# transfers, and the RDMA hardware counters of the NIC increase.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/minio.sh" || exit 1
source "$ELBENCHO_TEST_LIB/plugin_s3rdma.sh" || exit 1

NUM_THREADS=2
NUM_OBJECTS=3   # per thread
OBJ_SIZE=$((4 * 1024 * 1024))

EXPECTED_OBJECTS=$((NUM_THREADS * NUM_OBJECTS))
EXPECTED_BYTES=$((EXPECTED_OBJECTS * OBJ_SIZE))

require_s3rdma

test_init
tap_plan 21

BUCKET="$(bucket_name)"
WRITE_OPSLOG="$TEST_DIR/write.opslog"
READ_OPSLOG="$TEST_DIR/read.opslog"

start_s3rdma_server
if [ $? -ne 0 ]; then
    tap_bail "Unable to start an S3-over-RDMA test server instance."
fi

# The NIC counters count RDMA reads/writes by the remote side. Client and server
# share the NIC here, so either role's counter shows the transfers.
RDMA_DEV="$(rdma_device_for_ipv4 "$S3RDMA_ADDR")"

# hw_counter_check COUNTER BEFORE DESC
#
# Checks that the given NIC counter grew beyond BEFORE, or skips the check if the
# counter is not available on this NIC.
hw_counter_check()
{
    local after="$(rdma_hw_counter "$RDMA_DEV" "$1")"

    if [ -z "$2" ] || [ -z "$after" ]; then
        tap_skip "$3"
    else
        assert_gt "$after" "$2" "$3"
    fi
}

################## Upload the objects over RDMA ##################

READS_BEFORE="$(rdma_hw_counter "$RDMA_DEV" rx_read_requests)"

# "-n 0" means no name prefix dirs, so the object keys are "r<rank>-f<num>".
run_elbencho write \
    "${S3_OPTS[@]}" \
    -d -w \
    -t $NUM_THREADS -n 0 -N $NUM_OBJECTS -s $OBJ_SIZE -b $OBJ_SIZE \
    --verify 1 --blockvarpct 0 \
    --opslog "$WRITE_OPSLOG" \
    "s3://$BUCKET"
assert_ok $? "bucket creation and upload of $EXPECTED_OBJECTS objects over RDMA succeed"

assert_eq "$(json_value "$ELB_JSON" WRITE last_done entries)" "$EXPECTED_OBJECTS" \
    "write phase reports $EXPECTED_OBJECTS uploaded objects"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$EXPECTED_BYTES" \
    "write phase reports $EXPECTED_BYTES uploaded bytes"
assert_gt "$(json_value "$ELB_JSON" WRITE last_done 'bytes/s')" 0 \
    "write phase reports a non-zero throughput"

assert_eq "$(s3_object_count "$BUCKET")" "$EXPECTED_OBJECTS" \
    "the object listing contains $EXPECTED_OBJECTS objects"
assert_eq "$(s3_object_size "$BUCKET" r0-f0)" "$OBJ_SIZE" \
    "a head request for a single object reports a size of $OBJ_SIZE bytes"

assert_eq "$(opslog_count "$WRITE_OPSLOG" S3PutObject)" "$EXPECTED_OBJECTS" \
    "operations log contains $EXPECTED_OBJECTS single part uploads"
assert_eq "$(opslog_error_count "$WRITE_OPSLOG")" "0" \
    "operations log of the upload contains no failed operation"

if s3rdma_server_is_private; then
    assert_eq "$(file_size "$(s3rdma_object_file "$BUCKET" r1-f2)")" "$OBJ_SIZE" \
        "the server stored the last object as a file of $OBJ_SIZE bytes"
    assert_eq "$(s3rdma_server_rdma_ops PUT)" "$EXPECTED_OBJECTS" \
        "the server log shows $EXPECTED_OBJECTS RDMA transfers for the uploads"
else
    tap_skip "the server stored the last object as a file of $OBJ_SIZE bytes"
    tap_skip "the server log shows $EXPECTED_OBJECTS RDMA transfers for the uploads"
fi

hw_counter_check rx_read_requests "$READS_BEFORE" \
    "the NIC counted RDMA read requests during the upload"

################## Download the objects over RDMA ##################

WRITES_BEFORE="$(rdma_hw_counter "$RDMA_DEV" rx_write_requests)"

run_elbencho read \
    "${S3_OPTS[@]}" \
    -r \
    -t $NUM_THREADS -n 0 -N $NUM_OBJECTS -s $OBJ_SIZE -b $OBJ_SIZE \
    --verify 1 \
    --opslog "$READ_OPSLOG" \
    "s3://$BUCKET"
assert_ok $? "download over RDMA with data integrity check succeeds"

assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$EXPECTED_BYTES" \
    "read phase reports $EXPECTED_BYTES downloaded bytes"
assert_gt "$(json_value "$ELB_JSON" READ last_done 'bytes/s')" 0 \
    "read phase reports a non-zero throughput"

assert_eq "$(opslog_count "$READ_OPSLOG" S3GetObject)" "$EXPECTED_OBJECTS" \
    "operations log contains $EXPECTED_OBJECTS downloads"
assert_eq "$(opslog_error_count "$READ_OPSLOG")" "0" \
    "operations log of the download contains no failed operation"

if s3rdma_server_is_private; then
    assert_eq "$(s3rdma_server_rdma_ops GET)" "$EXPECTED_OBJECTS" \
        "the server log shows $EXPECTED_OBJECTS RDMA transfers for the downloads"
else
    tap_skip "the server log shows $EXPECTED_OBJECTS RDMA transfers for the downloads"
fi

hw_counter_check rx_write_requests "$WRITES_BEFORE" \
    "the NIC counted RDMA write requests during the download"

################## Delete the objects and the bucket ##################

run_elbencho delete \
    "${S3_OPTS[@]}" \
    -F -D --nodelerr \
    -t $NUM_THREADS -n 0 -N $NUM_OBJECTS \
    "s3://$BUCKET"
assert_ok $? "delete phase of the objects and the bucket succeeds"

assert_eq "$(json_value "$ELB_JSON" RMOBJECTS last_done entries)" "$EXPECTED_OBJECTS" \
    "rmobjects phase reports $EXPECTED_OBJECTS deleted objects"

if s3_bucket_exists "$BUCKET"; then
    tap_fail "the aws cli tool confirms that the bucket is gone"
else
    tap_ok "the aws cli tool confirms that the bucket is gone"
fi
