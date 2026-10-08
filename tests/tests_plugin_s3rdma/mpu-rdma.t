#!/bin/bash
#
# Multipart upload and ranged download over RDMA via an s3rdma_* plugin.
#
# The object size exceeds the block size, so elbencho uploads each object as a
# multipart upload and downloads it in several ranged requests. Both plugins
# move the parts and the ranges over RDMA while the create, complete and abort
# requests stay on HTTP, so the server log has to show one RDMA transfer per
# part and per range. The second half does the same with "--sharesize", where
# several threads upload parts of the same object.

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
EXPECTED_RANGES=$((EXPECTED_OBJECTS * OBJ_SIZE / READ_BLOCK_SIZE))

# shared uploads: the object keys are given directly, so each object is one
# multipart upload with parts from several threads (more threads than objects,
# otherwise elbencho gives each thread a whole object)
NUM_SHARED_THREADS=4
NUM_SHARED_OBJECTS=2
EXPECTED_SHARED_PARTS=$((NUM_SHARED_OBJECTS * OBJ_SIZE / PART_SIZE))
EXPECTED_SHARED_BYTES=$((NUM_SHARED_OBJECTS * OBJ_SIZE))

require_s3rdma

test_init
tap_plan 25

BUCKET="$(bucket_name)"
WRITE_OPSLOG="$TEST_DIR/write.opslog"
READ_OPSLOG="$TEST_DIR/read.opslog"
SHARED_OPSLOG="$TEST_DIR/shared.opslog"
SHARED_PATHS="s3://$BUCKET/shared[1-2]"

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
assert_ok $? "bucket creation and multipart upload of $EXPECTED_OBJECTS objects over RDMA succeed"

assert_eq "$(json_value "$ELB_JSON" WRITE last_done entries)" "$EXPECTED_OBJECTS" \
    "write phase reports $EXPECTED_OBJECTS uploaded objects"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$EXPECTED_BYTES" \
    "write phase reports $EXPECTED_BYTES uploaded bytes"

assert_eq "$(s3_object_count "$BUCKET")" "$EXPECTED_OBJECTS" \
    "the object listing contains $EXPECTED_OBJECTS objects"
assert_eq "$(s3_unique_object_sizes "$BUCKET")" "$OBJ_SIZE" \
    "all listed objects have a size of $OBJ_SIZE bytes"

assert_eq "$(opslog_count "$WRITE_OPSLOG" S3UploadPart)" "$EXPECTED_PARTS" \
    "operations log contains $EXPECTED_PARTS uploaded parts"
assert_eq "$(opslog_error_count "$WRITE_OPSLOG")" "0" \
    "operations log of the upload contains no failed operation"

if s3rdma_server_is_private; then
    assert_eq "$(file_size "$(s3rdma_object_file "$BUCKET" r1-f1)")" "$OBJ_SIZE" \
        "the server assembled the parts into an object file of $OBJ_SIZE bytes"
    assert_eq "$(s3rdma_server_rdma_ops PUT)" "$EXPECTED_PARTS" \
        "the server log shows $EXPECTED_PARTS RDMA transfers for the uploaded parts"
else
    tap_skip "the server assembled the parts into an object file of $OBJ_SIZE bytes"
    tap_skip "the server log shows $EXPECTED_PARTS RDMA transfers for the uploaded parts"
fi

################## Download the objects with ranged reads ##################

run_elbencho read \
    "${S3_OPTS[@]}" \
    -r \
    -t $NUM_THREADS -n 0 -N $NUM_OBJECTS -s $OBJ_SIZE -b $READ_BLOCK_SIZE \
    --verify 1 \
    --opslog "$READ_OPSLOG" \
    "s3://$BUCKET"
assert_ok $? "ranged download over RDMA with data integrity check succeeds"

assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$EXPECTED_BYTES" \
    "read phase reports $EXPECTED_BYTES downloaded bytes"
assert_eq "$(opslog_error_count "$READ_OPSLOG")" "0" \
    "operations log of the download contains no failed operation"

if s3rdma_server_is_private; then
    assert_eq "$(s3rdma_server_rdma_ops GET)" "$EXPECTED_RANGES" \
        "the server log shows $EXPECTED_RANGES RDMA transfers for the downloaded ranges"
else
    tap_skip "the server log shows $EXPECTED_RANGES RDMA transfers for the downloaded ranges"
fi

################## Shared multipart upload by several threads ##################

run_elbencho sharedwrite \
    "${S3_OPTS[@]}" \
    -w \
    -t $NUM_SHARED_THREADS -s $OBJ_SIZE -b $PART_SIZE --sharesize $PART_SIZE \
    --verify 1 --blockvarpct 0 \
    --opslog "$SHARED_OPSLOG" \
    "$SHARED_PATHS"
assert_ok $? "shared multipart upload of $NUM_SHARED_OBJECTS objects by $NUM_SHARED_THREADS threads over RDMA succeeds"

assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$EXPECTED_SHARED_BYTES" \
    "shared write phase reports $EXPECTED_SHARED_BYTES uploaded bytes"
assert_eq "$(opslog_count "$SHARED_OPSLOG" S3UploadPart)" "$EXPECTED_SHARED_PARTS" \
    "operations log contains $EXPECTED_SHARED_PARTS uploaded parts of the shared uploads"
assert_ge "$(opslog_distinct_ranks "$SHARED_OPSLOG" /shared1)" "2" \
    "operations log confirms that an object was uploaded by multiple threads"
assert_eq "$(s3_object_size "$BUCKET" shared1)" "$OBJ_SIZE" \
    "object \"shared1\" has a size of $OBJ_SIZE bytes"

if s3rdma_server_is_private; then
    assert_eq "$(s3rdma_server_rdma_ops PUT)" "$((EXPECTED_PARTS + EXPECTED_SHARED_PARTS))" \
        "the server log shows $EXPECTED_SHARED_PARTS more RDMA transfers for the shared parts"
else
    tap_skip "the server log shows $EXPECTED_SHARED_PARTS more RDMA transfers for the shared parts"
fi

run_elbencho sharedread \
    "${S3_OPTS[@]}" \
    -r \
    -t $NUM_SHARED_THREADS -s $OBJ_SIZE -b $PART_SIZE --sharesize $PART_SIZE \
    --verify 1 \
    "$SHARED_PATHS"
assert_ok $? "shared ranged download over RDMA with data integrity check succeeds"

if s3rdma_server_is_private; then
    assert_eq "$(s3rdma_server_rdma_ops GET)" "$((EXPECTED_RANGES + EXPECTED_SHARED_PARTS))" \
        "the server log shows $EXPECTED_SHARED_PARTS more RDMA transfers for the shared ranges"
else
    tap_skip "the server log shows $EXPECTED_SHARED_PARTS more RDMA transfers for the shared ranges"
fi

run_elbencho shareddelete \
    "${S3_OPTS[@]}" \
    -F --nodelerr \
    -t $NUM_SHARED_THREADS -s $OBJ_SIZE -b $PART_SIZE --sharesize $PART_SIZE \
    "$SHARED_PATHS"
assert_ok $? "delete phase of the shared objects succeeds"

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
