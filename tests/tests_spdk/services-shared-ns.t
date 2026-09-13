#!/bin/bash
#
# Two elbencho service instances working concurrently on one SPDK namespace.
#
# Only the coordinator gets the SPDK config; the service instances receive it
# through the normal argument transfer, which is part of what this test covers.
# Each service runs its own SPDK client and does its own namespace discovery, so
# the two of them have to agree on which namespace they are working on.
#
# By default the dataset is shared between the services, i.e. each of them
# covers a part of the given range. "--nosvcshare" makes each service work on
# the full range instead, and both cases are checked to pin that difference.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/spdk.sh" || exit 1

require_spdk

test_init
tap_plan 18

NS_IDX=0
NUM_SERVICES=2
NUM_THREADS=2
LENGTH=$((16 * 1024 * 1024))

SERVICE_PORTS=()

# Start an elbencho service instance on a random free port. Output has to go to
# a file, because prove reads the test's stdout until end of file.
start_service()
{
    local idx="$1"
    local tries=0
    local port
    local pid

    while [ $tries -lt 5 ]; do
        tries=$((tries+1))

        port=$(find_free_port)
        if [ -z "$port" ]; then
            return 1
        fi

        "$ELBENCHO_TEST_BIN" --service --foreground --port "$port" \
            > "$TEST_DIR/service$idx.log" 2>&1 &

        pid=$!
        register_pid "$pid"

        trace_cmd "service $idx start on port $port (attempt $tries)" \
            "$ELBENCHO_TEST_BIN" --service --foreground --port "$port"
        trace_add_log "$TEST_DIR/service$idx.log"

        if wait_for_port 127.0.0.1 "$port" 10; then
            SERVICE_PORTS+=("$port")
            return 0
        fi

        tap_diag "Service $idx did not come up on port $port, retrying..."
        kill -KILL "$pid" > /dev/null 2>&1
        wait "$pid" 2>/dev/null
    done

    return 1
}

start_nvmf_tgt
if [ $? -ne 0 ]; then
    tap_bail "Unable to start an SPDK NVMe-oF target (see nvmf_tgt.log)."
fi

NS_UUID="$(spdk_ns_uuid $NS_IDX)"
NS_NAME="$(spdk_ns_name $NS_IDX)"
BDEV="$(spdk_bdev_name $NS_IDX)"

for idx in $(seq 0 $((NUM_SERVICES - 1)) ); do
    start_service "$idx"
    if [ $? -ne 0 ]; then
        tap_diag "Service log:"
        tap_diag_file "$TEST_DIR/service$idx.log"
        tap_bail "Unable to start elbencho service instance $idx."
    fi
done

HOSTS="localhost:${SERVICE_PORTS[0]},localhost:${SERVICE_PORTS[1]}"
tap_ok "$NUM_SERVICES elbencho service instances are listening"

################## Both services on the same namespace ##################

OPSLOG="$TEST_DIR/shared.opslog"

# The integrity check is valid here even though both services write the same
# namespace: the pattern depends only on the absolute offset and the salt, so
# any distribution of the range across the services yields the same content.
# "--opsloglock" is used because both services append to the same log file.
run_elbencho shared "${SPDK_OPTS[@]}" \
    --hosts "$HOSTS" \
    -w -r -t $NUM_THREADS --iodepth 4 -b 65536 -s $LENGTH \
    --verify 1 --blockvarpct 0 \
    --opslog "$OPSLOG" --opsloglock \
    --livecsv "$TEST_DIR/shared.livecsv" --liveint 200 \
    "$NS_UUID"
assert_ok $? "distributed write and read on one namespace through two services succeed"

assert_eq "$(json_config "$ELB_JSON" WRITE hosts)" "$NUM_SERVICES" \
    "the result confirms $NUM_SERVICES service hosts"

# The range is shared between the services, so the total must be the given size
# and not a multiple of it.
assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$LENGTH" \
    "both services together wrote exactly $LENGTH bytes"
assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$LENGTH" \
    "both services together read exactly $LENGTH bytes"
assert_gt "$(json_value "$ELB_JSON" WRITE last_done 'bytes/s')" 0 \
    "the distributed write phase reports a non-zero throughput"

assert_eq "$(opslog_entry_names "$OPSLOG")" "$NS_NAME" \
    "all operations of both services went to the same single namespace"
assert_eq "$(opslog_error_count "$OPSLOG")" "0" \
    "operations log contains no failed operation"

# Worker ranks are unique across services, so all threads of both services show
# up as separate ranks operating on that one namespace.
TOTAL_WORKERS=$((NUM_SERVICES * NUM_THREADS))
assert_eq "$(opslog_num_distinct_ranks "$OPSLOG")" "$TOTAL_WORKERS" \
    "the operations log contains $TOTAL_WORKERS distinct worker ranks from both services"
assert_eq "$(opslog_distinct_ranks "$OPSLOG" "$NS_NAME")" "$TOTAL_WORKERS" \
    "all $TOTAL_WORKERS worker threads operated on the shared namespace"

# The two services must have been active at the same time, not one after the
# other. Worker ranks below NUM_THREADS belong to the first service.
assert_eq "$(opslog_ranks_overlap_in_time "$OPSLOG" $NUM_THREADS)" "1" \
    "the two services were working on the namespace at the same time"

assert_eq "$(spdk_tgt_iostat "$BDEV" bytes_written)" "$LENGTH" \
    "the target's own statistics confirm $LENGTH written bytes in total"
# Reads are a lower bound rather than an exact value, because attaching to a
# namespace also causes a few small reads of its own that land in the same
# counter. Writes are exact, so they are asserted exactly above.
assert_ge "$(spdk_tgt_iostat "$BDEV" bytes_read)" "$LENGTH" \
    "the target's own statistics confirm at least $LENGTH read bytes in total"

# Each service discovers the namespaces on its own, so a disagreement between
# them would be reported here.
assert_eq "$(grep -c 'Namespace discovery inconsistency' "$ELB_OUT")" "0" \
    "the services agree with the coordinator about the namespace"

################## Each service on the full range ##################

run_elbencho nosvcshare "${SPDK_OPTS[@]}" \
    --hosts "$HOSTS" \
    -w -t $NUM_THREADS --iodepth 4 -b 65536 -s $LENGTH \
    --nosvcshare \
    "$NS_UUID"
assert_ok $? "a distributed write with \"--nosvcshare\" succeeds"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$((LENGTH * NUM_SERVICES))" \
    "with \"--nosvcshare\" each service writes the full range, so the total doubles"

################## Terminate the services ##################

run_elbencho quit --hosts "$HOSTS" --quit
assert_ok $? "\"--quit\" is accepted by both services"

ALL_GONE=1
for port in "${SERVICE_PORTS[@]}"; do
    wait_for_port_gone 127.0.0.1 "$port" 10 || ALL_GONE=0
done
assert_eq "$ALL_GONE" "1" "both services stopped listening after \"--quit\""
