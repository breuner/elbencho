#!/bin/bash
#
# Large S3 objects uploaded as multipart uploads.
#
# The object size exceeds the block size given via "-b", so elbencho splits each
# upload into multiple parts. The operations log is used to verify that the
# expected number of multipart uploads and parts was really submitted. The block
# size must not be below 5 MiB here, because that is the minimum part size that
# the minio S3 server accepts for all but the last part of a multipart upload.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/minio.sh" || exit 1

NUM_THREADS=2
NUM_OBJECTS=2   # per thread
OBJ_SIZE=$((20 * 1024 * 1024))
PART_SIZE=$((5 * 1024 * 1024))
READ_BLOCK_SIZE=$((4 * 1024 * 1024))

EXPECTED_OBJECTS=$((NUM_THREADS * NUM_OBJECTS))
EXPECTED_BYTES=$((EXPECTED_OBJECTS * OBJ_SIZE))
EXPECTED_PARTS_PER_OBJ=$((OBJ_SIZE / PART_SIZE))
EXPECTED_PARTS=$((EXPECTED_OBJECTS * EXPECTED_PARTS_PER_OBJ))

require_build_feature s3
require_cmd aws
require_minio

test_init
tap_plan 18

BUCKET="$(bucket_name)"
WRITE_OPSLOG="$TEST_DIR/write.opslog"

start_minio
if [ $? -ne 0 ]; then
    tap_bail "Unable to start a minio S3 server instance."
fi

################## Upload the objects as multipart uploads ##################

# "-n 0" means no name prefix dirs, so the object keys are "r<rank>-f<num>".
run_elbencho write \
    "${S3_OPTS[@]}" \
    -d -w \
    -t $NUM_THREADS -n 0 -N $NUM_OBJECTS -s $OBJ_SIZE -b $PART_SIZE \
    --verify 1 --blockvarpct 0 \
    --opslog "$WRITE_OPSLOG" \
    "s3://$BUCKET"
assert_ok $? "bucket creation and multipart upload of $EXPECTED_OBJECTS large objects succeed"

assert_eq "$(json_value "$ELB_JSON" WRITE last_done entries)" "$EXPECTED_OBJECTS" \
    "write phase reports $EXPECTED_OBJECTS uploaded objects"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$EXPECTED_BYTES" \
    "write phase reports $EXPECTED_BYTES uploaded bytes"
assert_gt "$(json_value "$ELB_JSON" WRITE last_done 'bytes/s')" 0 \
    "write phase reports a non-zero throughput"

assert_eq "$(s3_object_count "$BUCKET")" "$EXPECTED_OBJECTS" \
    "the object listing contains $EXPECTED_OBJECTS objects"
assert_eq "$(s3_unique_object_sizes "$BUCKET")" "$OBJ_SIZE" \
    "all listed objects have a size of $OBJ_SIZE bytes"
assert_eq "$(s3_object_size "$BUCKET" r0-f0)" "$OBJ_SIZE" \
    "a head request for a single object reports a size of $OBJ_SIZE bytes"

assert_eq "$(opslog_count "$WRITE_OPSLOG" S3CreateMultipartUpload)" "$EXPECTED_OBJECTS" \
    "operations log contains $EXPECTED_OBJECTS started multipart uploads"
assert_eq "$(opslog_count "$WRITE_OPSLOG" S3UploadPart)" "$EXPECTED_PARTS" \
    "operations log contains $EXPECTED_PARTS uploaded parts"
assert_eq "$(opslog_count "$WRITE_OPSLOG" S3CompleteMultipartUpload)" "$EXPECTED_OBJECTS" \
    "operations log contains $EXPECTED_OBJECTS completed multipart uploads"
assert_eq "$(opslog_count "$WRITE_OPSLOG" S3PutObject)" "0" \
    "operations log contains no single part upload"
assert_eq "$(opslog_error_count "$WRITE_OPSLOG")" "0" \
    "operations log of the upload contains no failed operation"

################## Download the objects with ranged reads ##################

run_elbencho read \
    "${S3_OPTS[@]}" \
    -r \
    -t $NUM_THREADS -n 0 -N $NUM_OBJECTS -s $OBJ_SIZE -b $READ_BLOCK_SIZE \
    --verify 1 \
    "s3://$BUCKET"
assert_ok $? "ranged download with data integrity check succeeds"

assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$EXPECTED_BYTES" \
    "read phase reports $EXPECTED_BYTES downloaded bytes"
assert_gt "$(json_value "$ELB_JSON" READ last_done 'bytes/s')" 0 \
    "read phase reports a non-zero throughput"

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
