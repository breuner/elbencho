#!/bin/bash
#
# Self-test of the S3-over-RDMA test server over plain HTTP.
#
# The s3rdma_minio plugin only does single-part transfers, so the multipart
# upload, ranged download and listing paths of tools/s3rdma/server would otherwise
# stay unexercised. This runs elbencho without a plugin against the server, the
# same way as the minio tests do, and also checks that the server declines a
# request that carries an RDMA token while its RDMA side is disabled. It needs
# neither an s3rdma plugin nor RDMA hardware, only the server executable.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/minio.sh" || exit 1
source "$ELBENCHO_TEST_LIB/plugin_s3rdma.sh" || exit 1

NUM_THREADS=2
NUM_OBJECTS=2   # per thread
OBJ_SIZE=$((20 * 1024 * 1024))
PART_SIZE=$((5 * 1024 * 1024))
READ_BLOCK_SIZE=$((4 * 1024 * 1024))

EXPECTED_OBJECTS=$((NUM_THREADS * NUM_OBJECTS))
EXPECTED_BYTES=$((EXPECTED_OBJECTS * OBJ_SIZE))
EXPECTED_PARTS=$((EXPECTED_OBJECTS * OBJ_SIZE / PART_SIZE))

require_s3rdma_server
require_cmd curl

test_init
tap_plan 19

BUCKET="$(bucket_name)"
WRITE_OPSLOG="$TEST_DIR/write.opslog"

# no plugin selected, so this is a plain HTTP server instance
start_s3rdma_server
if [ $? -ne 0 ]; then
    tap_bail "Unable to start an S3-over-RDMA test server instance."
fi

################## Upload the objects as multipart uploads ##################

run_elbencho write \
    "${S3_OPTS[@]}" \
    -d -w \
    -t $NUM_THREADS -n 0 -N $NUM_OBJECTS -s $OBJ_SIZE -b $PART_SIZE \
    --verify 1 --blockvarpct 0 \
    --opslog "$WRITE_OPSLOG" \
    "s3://$BUCKET"
assert_ok $? "bucket creation and multipart upload of $EXPECTED_OBJECTS objects succeed"

assert_eq "$(json_value "$ELB_JSON" WRITE last_done entries)" "$EXPECTED_OBJECTS" \
    "write phase reports $EXPECTED_OBJECTS uploaded objects"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$EXPECTED_BYTES" \
    "write phase reports $EXPECTED_BYTES uploaded bytes"

assert_eq "$(s3_object_count "$BUCKET")" "$EXPECTED_OBJECTS" \
    "the object listing contains $EXPECTED_OBJECTS objects"
assert_eq "$(s3_unique_object_sizes "$BUCKET")" "$OBJ_SIZE" \
    "all listed objects have a size of $OBJ_SIZE bytes"

assert_eq "$(opslog_count "$WRITE_OPSLOG" S3CreateMultipartUpload)" "$EXPECTED_OBJECTS" \
    "operations log contains $EXPECTED_OBJECTS started multipart uploads"
assert_eq "$(opslog_count "$WRITE_OPSLOG" S3UploadPart)" "$EXPECTED_PARTS" \
    "operations log contains $EXPECTED_PARTS uploaded parts"
assert_eq "$(opslog_count "$WRITE_OPSLOG" S3CompleteMultipartUpload)" "$EXPECTED_OBJECTS" \
    "operations log contains $EXPECTED_OBJECTS completed multipart uploads"
assert_eq "$(opslog_error_count "$WRITE_OPSLOG")" "0" \
    "operations log of the upload contains no failed operation"

if s3rdma_server_is_private; then
    assert_eq "$(file_size "$(s3rdma_object_file "$BUCKET" r1-f1)")" "$OBJ_SIZE" \
        "the server assembled the parts into an object file of $OBJ_SIZE bytes"
    assert_eq "$(count_dirs "$S3RDMA_DATA_DIR/$BUCKET/.mpu")" "0" \
        "no multipart upload is left in progress on the server"
else
    tap_skip "the server assembled the parts into an object file of $OBJ_SIZE bytes"
    tap_skip "no multipart upload is left in progress on the server"
fi

################## Head and ranged download ##################

run_elbencho read \
    "${S3_OPTS[@]}" \
    --stat -r \
    -t $NUM_THREADS -n 0 -N $NUM_OBJECTS -s $OBJ_SIZE -b $READ_BLOCK_SIZE \
    --verify 1 \
    "s3://$BUCKET"
assert_ok $? "head and ranged download phases with data integrity check succeed"

assert_eq "$(json_value "$ELB_JSON" HEADOBJ last_done entries)" "$EXPECTED_OBJECTS" \
    "headobj phase reports $EXPECTED_OBJECTS objects"
assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$EXPECTED_BYTES" \
    "read phase reports $EXPECTED_BYTES downloaded bytes"

################## RDMA requests get declined ##################

# A fake but well-formed cuObject token. The private server has RDMA disabled
# here, so it must decline with the protocol's 501 reply instead of trying a
# transfer. (An external server may support RDMA, and the unsigned request would
# not pass its authentication anyway.)
FAKE_TOKEN="0000000000001000:00100000:00000001:0000:000001:1:00000000000000000000ffffc0a80001"

if s3rdma_server_is_private; then
    trace_cmd "curl" curl -s -o /dev/null -D - -X PUT -H "x-amz-rdma-token: $FAKE_TOKEN" \
        "$S3_ENDPOINT/$BUCKET/rdma-declined"
    DECLINE_HEADERS="$(curl -s -o /dev/null -D - -X PUT -H "x-amz-rdma-token: $FAKE_TOKEN" \
        "$S3_ENDPOINT/$BUCKET/rdma-declined")"

    assert_match "$DECLINE_HEADERS" "^HTTP/1.1 501" \
        "a PUT with an RDMA token is answered with HTTP 501 while RDMA is disabled"
    assert_match "$DECLINE_HEADERS" "^x-amz-rdma-reply: 501" \
        "the declined PUT carries the x-amz-rdma-reply 501 header"
else
    tap_skip "a PUT with an RDMA token is answered with HTTP 501 while RDMA is disabled"
    tap_skip "the declined PUT carries the x-amz-rdma-reply 501 header"
fi

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
