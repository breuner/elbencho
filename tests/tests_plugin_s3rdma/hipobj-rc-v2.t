#!/bin/bash
#
# AMD hipObject's hipobj-rc-v2 protocol: PUT and GET over RDMA with RC transport.
#
# No elbencho plugin speaks this protocol yet, so this drives the test server's
# implementation of it with hipObject's own RDMA test client (built from the
# hipObject sources next to the server). The client uploads a small object with
# an RDMA write into the server's staging buffer, reads it back via the server's
# RDMA write into its buffer and verifies the payload itself. The server side is
# checked through the stored file, the server log and the NIC's RDMA counters.
# This runs on any RoCE NIC, also on ones without cuObject's DC transport.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/minio.sh" || exit 1
source "$ELBENCHO_TEST_LIB/plugin_s3rdma.sh" || exit 1

OBJ_SIZE=64   # the client's transfer buffer holds at most 4 KiB
PAYLOAD_PREFIX="dp-put-payload"

require_hipobj_rc
require_cmd curl

test_init
tap_plan 14

BUCKET="$(bucket_name)"

start_s3rdma_server
if [ $? -ne 0 ]; then
    tap_bail "Unable to start an S3-over-RDMA test server instance."
fi

RDMA_DEV="$(rdma_device_for_ipv4 "$S3RDMA_ADDR")"

# Both directions are RDMA writes by the peer, so the NIC's counter of received
# write requests grows with each transfer. Client and server share the NIC here.
WRITES_BEFORE="$(rdma_hw_counter "$RDMA_DEV" rx_write_requests)"

################## Upload over RDMA ##################

run_elbencho mkbucket \
    "${S3_OPTS[@]}" \
    -d \
    "s3://$BUCKET"
assert_ok $? "bucket creation succeeds"

hipobj_data_client PUT "/$BUCKET/rdmaobj" $OBJ_SIZE
assert_ok $? "hipObject client uploads an object of $OBJ_SIZE bytes via RDMA"

assert_eq "$(file_size "$(s3rdma_object_file "$BUCKET" rdmaobj)")" "$OBJ_SIZE" \
    "the server stored the object as a file of $OBJ_SIZE bytes"
assert_eq "$(head -c ${#PAYLOAD_PREFIX} "$(s3rdma_object_file "$BUCKET" rdmaobj)")" \
    "$PAYLOAD_PREFIX" "the stored file starts with the client's payload"
assert_eq "$(s3_object_size "$BUCKET" rdmaobj)" "$OBJ_SIZE" \
    "a head request via the aws cli reports a size of $OBJ_SIZE bytes"
assert_eq "$(s3rdma_server_rdma_ops PUT)" "1" \
    "the server log shows one RDMA transfer for the upload"

################## Download over RDMA ##################

hipobj_data_client GET "/$BUCKET/rdmaobj" $OBJ_SIZE
assert_ok $? "hipObject client downloads the object via RDMA and verifies the payload"

assert_eq "$(s3rdma_server_rdma_ops GET)" "1" \
    "the server log shows one RDMA transfer for the download"

WRITES_AFTER="$(rdma_hw_counter "$RDMA_DEV" rx_write_requests)"
if [ -n "$WRITES_BEFORE" ] && [ -n "$WRITES_AFTER" ]; then
    assert_ge "$WRITES_AFTER" "$((WRITES_BEFORE + 2))" \
        "the NIC counted at least two RDMA write requests for the two transfers"
else
    tap_skip "the NIC counted at least two RDMA write requests for the two transfers"
fi

################## Protocol corner cases ##################

CANCEL_CODE=$(curl -s -o /dev/null -w '%{http_code}' -X POST \
    -H 'x-amz-rdma-protocol: hipobj-rc-v2' \
    -H 'x-amz-rdma-session: 00000000000000000000000000000000' \
    "$S3_ENDPOINT/.hipobj-rc/cancel")
assert_eq "$CANCEL_CODE" "204" "cancelling an unknown session is answered with 204"

UNSUPPORTED_HEADERS="$(curl -s -o /dev/null -D - -X POST "$S3_ENDPOINT/.hipobj-rc/prepare")"
assert_match "$UNSUPPORTED_HEADERS" "^HTTP/1.1 501" \
    "a prepare request without the protocol header is answered with 501"
assert_match "$UNSUPPORTED_HEADERS" "^X-Amz-Rdma-Protocol-Status: unsupported" \
    "the 501 carries the protocol's unsupported marker"

################## Delete the object and the bucket ##################

run_elbencho delete \
    "${S3_OPTS[@]}" \
    -F -D --nodelerr \
    "s3://$BUCKET/rdmaobj"
assert_ok $? "delete phase of the object and the bucket succeeds"

if s3_bucket_exists "$BUCKET"; then
    tap_fail "the aws cli tool confirms that the bucket is gone"
else
    tap_ok "the aws cli tool confirms that the bucket is gone"
fi
