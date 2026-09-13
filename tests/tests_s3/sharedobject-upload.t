#!/bin/bash
#
# Shared upload and download of large S3 objects by multiple threads.
#
# The object keys are given directly as benchmark paths, and their size exceeds
# "--sharesize", so each object is uploaded cooperatively by several worker
# threads, each of them submitting a part of the same multipart upload. The
# operations log is used to verify that an object really was handled by more
# than one thread. The part size must not be below 5 MiB, because that is the
# minimum part size that the minio S3 server accepts.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/minio.sh" || exit 1

NUM_THREADS=8
OBJ_SIZE=$((40 * 1024 * 1024))
PART_SIZE=$((5 * 1024 * 1024))

EXPECTED_OBJECTS=2
EXPECTED_BYTES=$((EXPECTED_OBJECTS * OBJ_SIZE))
EXPECTED_PARTS=$((EXPECTED_BYTES / PART_SIZE))

require_build_feature s3
require_cmd aws
require_minio

test_init
tap_plan 20

BUCKET="$(bucket_name)"
WRITE_OPSLOG="$TEST_DIR/write.opslog"

start_minio
if [ $? -ne 0 ]; then
    tap_bail "Unable to start a minio S3 server instance."
fi

# Quoted, so that elbencho itself expands the number range in the brackets.
BENCH_PATHS="s3://$BUCKET/shared[1-2]"

################## Create the bucket ##################

run_elbencho mkbucket \
    "${S3_OPTS[@]}" \
    -d \
    "s3://$BUCKET"
assert_ok $? "bucket creation succeeds"

assert_eq "$(json_value "$ELB_JSON" MKBUCKETS last_done entries)" "1" \
    "mkbuckets phase reports one created bucket"

################## Shared upload of the objects ##################

run_elbencho write \
    "${S3_OPTS[@]}" \
    -w \
    -t $NUM_THREADS -s $OBJ_SIZE -b $PART_SIZE --sharesize $PART_SIZE \
    --verify 1 --blockvarpct 0 \
    --opslog "$WRITE_OPSLOG" \
    "$BENCH_PATHS"
assert_ok $? "shared upload of $EXPECTED_OBJECTS objects by $NUM_THREADS threads succeeds"

assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$EXPECTED_BYTES" \
    "write phase reports $EXPECTED_BYTES uploaded bytes"
assert_gt "$(json_value "$ELB_JSON" WRITE last_done 'bytes/s')" 0 \
    "write phase reports a non-zero throughput"

assert_eq "$(s3_object_count "$BUCKET")" "$EXPECTED_OBJECTS" \
    "the object listing contains $EXPECTED_OBJECTS objects"
assert_eq "$(s3_object_size "$BUCKET" shared1)" "$OBJ_SIZE" \
    "object \"shared1\" has a size of $OBJ_SIZE bytes"
assert_eq "$(s3_object_size "$BUCKET" shared2)" "$OBJ_SIZE" \
    "object \"shared2\" has a size of $OBJ_SIZE bytes"

assert_eq "$(opslog_count "$WRITE_OPSLOG" S3CreateMultipartUpload)" "$EXPECTED_OBJECTS" \
    "operations log contains $EXPECTED_OBJECTS started multipart uploads"
assert_eq "$(opslog_count "$WRITE_OPSLOG" S3UploadPart)" "$EXPECTED_PARTS" \
    "operations log contains $EXPECTED_PARTS uploaded parts"
assert_eq "$(opslog_count "$WRITE_OPSLOG" S3CompleteMultipartUpload)" "$EXPECTED_OBJECTS" \
    "operations log contains $EXPECTED_OBJECTS completed multipart uploads"
assert_ge "$(opslog_distinct_ranks "$WRITE_OPSLOG" /shared1)" "2" \
    "operations log confirms that an object was uploaded by multiple threads"
assert_eq "$(opslog_error_count "$WRITE_OPSLOG")" "0" \
    "operations log of the upload contains no failed operation"

################## Shared download of the objects ##################

run_elbencho read \
    "${S3_OPTS[@]}" \
    -r \
    -t $NUM_THREADS -s $OBJ_SIZE -b $PART_SIZE --sharesize $PART_SIZE \
    --verify 1 \
    "$BENCH_PATHS"
assert_ok $? "shared download with data integrity check succeeds"

assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$EXPECTED_BYTES" \
    "read phase reports $EXPECTED_BYTES downloaded bytes"

################## Delete the objects and the bucket ##################

run_elbencho delete \
    "${S3_OPTS[@]}" \
    -F --nodelerr \
    -t $NUM_THREADS -s $OBJ_SIZE -b $PART_SIZE --sharesize $PART_SIZE \
    "$BENCH_PATHS"
assert_ok $? "delete phase of the shared objects succeeds"

assert_eq "$(s3_object_count "$BUCKET")" "0" \
    "no objects are left in the bucket after the delete phase"

run_elbencho rmbucket \
    "${S3_OPTS[@]}" \
    -D --nodelerr \
    "s3://$BUCKET"
assert_ok $? "bucket deletion succeeds"

assert_eq "$(json_value "$ELB_JSON" RMBUCKETS last_done entries)" "1" \
    "rmbuckets phase reports one deleted bucket"

if s3_bucket_exists "$BUCKET"; then
    tap_fail "the aws cli tool confirms that the bucket is gone"
else
    tap_ok "the aws cli tool confirms that the bucket is gone"
fi
