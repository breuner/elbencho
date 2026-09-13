#!/bin/bash
#
# Lots of small S3 objects in a bucket.
#
# Creates a bucket and uploads 80 small objects with generated names, queries
# their attributes, downloads them with a data integrity check and deletes
# everything again. Also covers "--s3objprefix". Results are cross-checked
# against the object listing of the aws cli tool.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/minio.sh" || exit 1

NUM_THREADS=4
NUM_DIRS=2      # per thread
NUM_FILES=10    # per thread and dir
OBJ_SIZE=4096

OBJ_PREFIX="pfx/"
PREFIX_THREADS=2
PREFIX_DIRS=1
PREFIX_FILES=3

EXPECTED_OBJECTS=$((NUM_THREADS * NUM_DIRS * NUM_FILES))
EXPECTED_BYTES=$((EXPECTED_OBJECTS * OBJ_SIZE))
EXPECTED_PREFIXED=$((PREFIX_THREADS * PREFIX_DIRS * PREFIX_FILES))
EXPECTED_TOTAL=$((EXPECTED_OBJECTS + EXPECTED_PREFIXED))

require_build_feature s3
require_cmd aws
require_minio

test_init
tap_plan 28

BUCKET="$(bucket_name)"
WRITE_OPSLOG="$TEST_DIR/write.opslog"
READ_OPSLOG="$TEST_DIR/read.opslog"

start_minio
if [ $? -ne 0 ]; then
    tap_bail "Unable to start a minio S3 server instance."
fi

################## Create the bucket and upload the objects ##################

run_elbencho write \
    "${S3_OPTS[@]}" \
    -d -w \
    -t $NUM_THREADS -n $NUM_DIRS -N $NUM_FILES -s $OBJ_SIZE -b $OBJ_SIZE \
    --verify 1 --blockvarpct 0 \
    --opslog "$WRITE_OPSLOG" \
    "s3://$BUCKET"
assert_ok $? "bucket creation and upload of $EXPECTED_OBJECTS small objects succeed"

assert_eq "$(json_value "$ELB_JSON" MKBUCKETS last_done entries)" "1" \
    "mkbuckets phase reports one created bucket"
if s3_bucket_exists "$BUCKET"; then
    tap_ok "the aws cli tool confirms that the bucket exists"
else
    tap_fail "the aws cli tool confirms that the bucket exists"
fi

assert_eq "$(s3_object_count "$BUCKET")" "$EXPECTED_OBJECTS" \
    "the object listing contains $EXPECTED_OBJECTS objects"
assert_eq "$(s3_unique_object_sizes "$BUCKET")" "$OBJ_SIZE" \
    "all listed objects have a size of $OBJ_SIZE bytes"

assert_eq "$(json_value "$ELB_JSON" WRITE last_done entries)" "$EXPECTED_OBJECTS" \
    "write phase reports $EXPECTED_OBJECTS uploaded objects"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$EXPECTED_BYTES" \
    "write phase reports $EXPECTED_BYTES uploaded bytes"
assert_gt "$(json_value "$ELB_JSON" WRITE last_done 'bytes/s')" 0 \
    "write phase reports a non-zero throughput"
assert_eq "$(json_config "$ELB_JSON" WRITE path_type)" "bucket" \
    "write phase result reports a bucket as benchmark path type"

assert_eq "$(opslog_count "$WRITE_OPSLOG" S3CreateBucket)" "1" \
    "operations log contains one completed S3CreateBucket operation"
assert_eq "$(opslog_count "$WRITE_OPSLOG" S3PutObject)" "$EXPECTED_OBJECTS" \
    "operations log contains $EXPECTED_OBJECTS completed S3PutObject operations"
assert_eq "$(opslog_error_count "$WRITE_OPSLOG")" "0" \
    "operations log of the upload contains no failed operation"

################## Query attributes and download the objects ##################

run_elbencho read \
    "${S3_OPTS[@]}" \
    --stat -r \
    -t $NUM_THREADS -n $NUM_DIRS -N $NUM_FILES -s $OBJ_SIZE -b $OBJ_SIZE \
    --verify 1 \
    --opslog "$READ_OPSLOG" \
    "s3://$BUCKET"
assert_ok $? "head and download phases with data integrity check succeed"

assert_eq "$(json_value "$ELB_JSON" HEADOBJ last_done entries)" "$EXPECTED_OBJECTS" \
    "headobj phase reports $EXPECTED_OBJECTS queried objects"
assert_eq "$(json_value "$ELB_JSON" READ last_done entries)" "$EXPECTED_OBJECTS" \
    "read phase reports $EXPECTED_OBJECTS downloaded objects"
assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$EXPECTED_BYTES" \
    "read phase reports $EXPECTED_BYTES downloaded bytes"

assert_eq "$(opslog_count "$READ_OPSLOG" S3HeadObject)" "$EXPECTED_OBJECTS" \
    "operations log contains $EXPECTED_OBJECTS completed S3HeadObject operations"
assert_eq "$(opslog_count "$READ_OPSLOG" S3GetObject)" "$EXPECTED_OBJECTS" \
    "operations log contains $EXPECTED_OBJECTS completed S3GetObject operations"
assert_eq "$(opslog_error_count "$READ_OPSLOG")" "0" \
    "operations log of the download contains no failed operation"

# A different salt means different expected contents, so this has to fail. That
# way the data integrity check above is proven to be effective.
run_elbencho badverify \
    "${S3_OPTS[@]}" \
    -r \
    -t $NUM_THREADS -n $NUM_DIRS -N $NUM_FILES -s $OBJ_SIZE -b $OBJ_SIZE \
    --verify 99 \
    "s3://$BUCKET" > /dev/null 2>&1
assert_nok $? "download with a wrong data integrity salt fails as expected"

################## Upload another dataset with an object prefix ##################

run_elbencho prefixwrite \
    "${S3_OPTS[@]}" \
    -w \
    -t $PREFIX_THREADS -n $PREFIX_DIRS -N $PREFIX_FILES -s $OBJ_SIZE -b $OBJ_SIZE \
    --s3objprefix "$OBJ_PREFIX" \
    "s3://$BUCKET"
assert_ok $? "upload with object prefix \"$OBJ_PREFIX\" succeeds"

assert_eq "$(s3_object_count "$BUCKET" "$OBJ_PREFIX")" "$EXPECTED_PREFIXED" \
    "$EXPECTED_PREFIXED objects were created below the given object prefix"
assert_eq "$(s3_object_count "$BUCKET")" "$EXPECTED_TOTAL" \
    "the bucket now contains $EXPECTED_TOTAL objects in total"

################## Delete the objects and the bucket ##################

run_elbencho prefixdelete \
    "${S3_OPTS[@]}" \
    -F --nodelerr \
    -t $PREFIX_THREADS -n $PREFIX_DIRS -N $PREFIX_FILES \
    --s3objprefix "$OBJ_PREFIX" \
    "s3://$BUCKET"
assert_ok $? "delete phase of the prefixed objects succeeds"

run_elbencho delete \
    "${S3_OPTS[@]}" \
    -F -D --nodelerr \
    -t $NUM_THREADS -n $NUM_DIRS -N $NUM_FILES \
    "s3://$BUCKET"
assert_ok $? "delete phase of the remaining objects and the bucket succeeds"

assert_eq "$(json_value "$ELB_JSON" RMOBJECTS last_done entries)" "$EXPECTED_OBJECTS" \
    "rmobjects phase reports $EXPECTED_OBJECTS deleted objects"
assert_eq "$(json_value "$ELB_JSON" RMBUCKETS last_done entries)" "1" \
    "rmbuckets phase reports one deleted bucket"

if s3_bucket_exists "$BUCKET"; then
    tap_fail "the aws cli tool confirms that the bucket is gone"
else
    tap_ok "the aws cli tool confirms that the bucket is gone"
fi
