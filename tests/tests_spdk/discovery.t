#!/bin/bash
#
# SPDK namespace discovery, namespace selectors and SPDK mode argument checks.
#
# Starts a private NVMe-oF target with two subsystems of two namespaces each and
# compares what elbencho discovers against what the target itself reports. Also
# covers the four ways to select a namespace and the SPDK specific argument
# validations, which are cheap to check because they fail before any I/O.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/spdk.sh" || exit 1

require_spdk

test_init
tap_plan 38

start_nvmf_tgt
if [ $? -ne 0 ]; then
    tap_bail "Unable to start an SPDK NVMe-oF target (see nvmf_tgt.log)."
fi

NS0_NAME="$(spdk_ns_name 0)"
NS0_UUID="$(spdk_ns_uuid 0)"
SMALLEST_NS_SIZE="$(spdk_ns_size 0)"

################## What the target itself reports ##################

assert_eq "$(spdk_tgt_subsystem_nqns)" "$SPDK_NQN1 $SPDK_NQN2" \
    "the target reports exactly the two provisioned subsystems"

EXPECTED_TRIPLES="$(printf '%s\n' \
    "$SPDK_NQN1|1|$(spdk_ns_uuid 0)|$(spdk_bdev_name 0)" \
    "$SPDK_NQN1|2|$(spdk_ns_uuid 1)|$(spdk_bdev_name 1)" \
    "$SPDK_NQN2|1|$(spdk_ns_uuid 2)|$(spdk_bdev_name 2)" \
    "$SPDK_NQN2|2|$(spdk_ns_uuid 3)|$(spdk_bdev_name 3)" | sort | tr '\n' ' ' | sed -e 's/ $//')"
assert_eq "$(spdk_tgt_ns_triples)" "$EXPECTED_TRIPLES" \
    "the target reports the four namespaces with the expected ids and uuids"

assert_eq "$(spdk_tgt_ns_count "$SPDK_NQN1")" "2" \
    "the first subsystem has two namespaces"
assert_eq "$(spdk_tgt_ns_count "$SPDK_NQN2")" "2" \
    "the second subsystem has two namespaces"

EXPECTED_BDEVS="$(printf '%s\n' \
    "$(spdk_bdev_name 0)|$SPDK_SECTOR_SIZE|$(( $(spdk_ns_size 0) / SPDK_SECTOR_SIZE ))" \
    "$(spdk_bdev_name 1)|$SPDK_SECTOR_SIZE|$(( $(spdk_ns_size 1) / SPDK_SECTOR_SIZE ))" \
    "$(spdk_bdev_name 2)|$SPDK_SECTOR_SIZE|$(( $(spdk_ns_size 2) / SPDK_SECTOR_SIZE ))" \
    "$(spdk_bdev_name 3)|$SPDK_SECTOR_SIZE|$(( $(spdk_ns_size 3) / SPDK_SECTOR_SIZE ))" | \
    sort | tr '\n' ' ' | sed -e 's/ $//')"
assert_eq "$(spdk_tgt_bdev_info)" "$EXPECTED_BDEVS" \
    "the backing bdevs have the expected block size and block count"

################## What elbencho discovers ##################

# Without a benchmark path, elbencho lists the available namespaces and then
# exits with an error, so the exit code is non-zero by design.
run_elbencho list "${SPDK_OPTS[@]}"
assert_nok $? "namespace listing mode exits non-zero by design"

assert_match "$(cat "$ELB_OUT")" 'No namespaces given, exiting after listing available namespaces' \
    "listing mode explains why it exited"
assert_eq "$(grep -cF "$SPDK_DISCOVERY_HEADER" "$ELB_OUT")" "1" \
    "the discovery result header is printed as expected"

assert_eq "$(spdk_listed_count "$ELB_OUT")" "4" \
    "elbencho discovered all four namespaces"
assert_eq "$(spdk_listed_names "$ELB_OUT")" \
    "$(spdk_ns_name 0) $(spdk_ns_name 1) $(spdk_ns_name 2) $(spdk_ns_name 3)" \
    "the discovered namespace names are as expected"
assert_eq "$(spdk_listed_nsids "$ELB_OUT")" "0 1 2 3" \
    "elbencho assigned the numeric namespace ids 0 to 3"
assert_eq "$(spdk_listed_sizes "$ELB_OUT")" "128M 192M 256M 320M" \
    "the discovered namespace sizes match the backing file sizes"

for idx in 0 1 2 3; do
    assert_eq "$(spdk_listed_field "$ELB_OUT" "$(spdk_ns_name "$idx")" 6)" "$(spdk_ns_uuid "$idx")" \
        "namespace \"$(spdk_ns_name "$idx")\" is listed with its expected uuid"
done

assert_eq "$(spdk_listed_field "$ELB_OUT" "$NS0_NAME" 4)" "${SPDK_SECTOR_SIZE}B" \
    "the listed sector size is $SPDK_SECTOR_SIZE bytes"
assert_eq "$(spdk_listed_field "$ELB_OUT" "$NS0_NAME" 5)" "$SPDK_NS_MODEL" \
    "the listed model name is the one the target was given"

################## The listing is also shown at any verbose log level ##################

# The discovery result is printed for every log level of at least 1, so both of
# these must show it while still running the benchmark itself.
for loglevel in 1 2; do
    run_elbencho "verbose$loglevel" "${SPDK_OPTS[@]}" \
        --log $loglevel -r -t 1 -b 4k -s 4k "$NS0_UUID"
    assert_ok $? "benchmark run with \"--log $loglevel\" succeeds"
    assert_eq "$(grep -cF "$SPDK_DISCOVERY_HEADER" "$ELB_OUT")" "1" \
        "\"--log $loglevel\" also prints the discovery result"
done

################## Namespace selectors ##################

NS0_NUMERIC="$(spdk_listed_nsid "$TEST_DIR/list.out" "$NS0_NAME")"

run_elbencho byname "${SPDK_OPTS[@]}" -r -t 1 -b 4k -s 4k "$NS0_NAME"
assert_ok $? "a namespace can be selected by its human-friendly name"
BYNAME_BYTES="$(json_value "$ELB_JSON" READ last_done bytes)"

run_elbencho byuuid "${SPDK_OPTS[@]}" -r -t 1 -b 4k -s 4k "$NS0_UUID"
assert_ok $? "a namespace can be selected by its uuid"
assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$BYNAME_BYTES" \
    "selecting by uuid reads the same amount as selecting by name"

run_elbencho byid "${SPDK_OPTS[@]}" -r -t 1 -b 4k -s 4k "$NS0_NUMERIC"
assert_ok $? "a namespace can be selected by its numeric id"
assert_eq "$(json_value "$ELB_JSON" READ last_done bytes)" "$BYNAME_BYTES" \
    "selecting by numeric id reads the same amount as selecting by name"

# A regex is matched against the human-friendly names, so this selects both
# namespaces of the first subsystem.
run_elbencho byregex "${SPDK_OPTS[@]}" -r -t 2 -b 4k -s 4k \
    --opslog "$TEST_DIR/regex.opslog" 'sysa:.*'
assert_ok $? "a regex selects all matching namespaces"
assert_eq "$(opslog_entry_names "$TEST_DIR/regex.opslog")" \
    "$(spdk_ns_name 0) $(spdk_ns_name 1)" \
    "the regex selected exactly the two namespaces of the first subsystem"

################## Argument checks ##################

# Each of these must be rejected before any I/O happens.
check_rejected()
{
    local tag="$1"
    local expected="$2"
    shift 2

    run_elbencho "$tag" "${SPDK_OPTS[@]}" "$@" > /dev/null 2>&1
    local rc=$?

    if [ $rc -eq 0 ]; then
        tap_fail "$tag is rejected"
        tap_diag "  the run unexpectedly succeeded"
        return
    fi

    assert_match "$(cat "$ELB_OUT")" "$expected" "$tag is rejected as expected"
}

check_rejected "an invalid namespace name" \
    'Given namespace name/UUID/NGUID is invalid: nosuchnamespace' \
    -r -t 1 -b 4k -s 4k nosuchnamespace
check_rejected "an invalid numeric namespace id" \
    'Given numeric namespace ID is invalid: 99' \
    -r -t 1 -b 4k -s 4k 99
check_rejected "a block size that is not a multiple of the sector size" \
    'Block size must be an even multiple of namespace sector size' \
    -r -t 1 -b 1000 -s 4k "$NS0_UUID"
check_rejected "a size that is not a multiple of the sector size" \
    'File size and offset must be an even multiple of namespace sector size: 4096 bytes' \
    -r -t 1 -b 4k -s 4100 "$NS0_UUID"
check_rejected "an offset beyond the smallest namespace" \
    "Given offset is not smaller than size of smallest namespace: $SMALLEST_NS_SIZE bytes" \
    -r -t 1 -b 4k --offset "$SMALLEST_NS_SIZE" "$NS0_UUID"
check_rejected "an offset plus size beyond the smallest namespace" \
    "Given offset plus size exceeds size of smallest namespace: $SMALLEST_NS_SIZE bytes" \
    -r -t 1 -b 4k --offset 4096 -s "$SMALLEST_NS_SIZE" "$NS0_UUID"
check_rejected "a delete phase" \
    'Delete not supported in SPDK mode' \
    -F -t 1 "$NS0_UUID"
check_rejected "memory mapped IO" \
    'mmap not supported in SPDK mode' \
    -r -t 1 -b 4k -s 4k --mmap "$NS0_UUID"
check_rejected "unaligned random IO" \
    'Unaligned IO not supported in SPDK mode' \
    -r -t 1 -b 4k -s 4k --rand --norandalign "$NS0_UUID"
