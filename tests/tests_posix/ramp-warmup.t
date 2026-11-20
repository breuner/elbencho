#!/bin/bash
#
# Warm-up period of a benchmark phase ("--ramp", alias "--warmup").
#
# Covers the argument guards and the effect of the warm-up period on the
# results: the counters, the latency histogram and the elapsed time of a phase
# have to refer to the steady-state remainder of the phase only, and the live
# csv file has to hold the intervals of that remainder only.
#
# The warm-up time is part of the phase time limit, so a run with a warm-up
# period takes just as long as the same run without one, but reports a measured
# window that is shorter by the warm-up time. That difference to an otherwise
# identical reference run is what the assertions below are built on. The same
# holds for a fixed, count-based workload without a time limit, which is timed
# by a bandwidth limit here to make its duration predictable.
#
# A workload that is through before the warm-up time has no measured window at
# all, which is a fatal error, so that is covered as well.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1

NUM_THREADS=2
NUM_FILES=8
FILE_SIZE=$((1024 * 1024))
BLOCK_SIZE=$((256 * 1024))

TIME_LIMIT=5
RAMP_SECS=2
MEASURED_SECS=$(( TIME_LIMIT - RAMP_SECS ))

# Live stats interval, which is also the interval in which the time limit and
# thus the end of the phase is detected.
LIVE_INTERVAL_MS=500

# Tolerance for the reported elapsed time. A phase ends at the first live stats
# tick after the time limit, and the workers drop their warm-up stats at the
# first interruption check after the warm-up time, so the measured window can
# only be a bit longer than requested, never shorter.
TOLERANCE_MS=2500

test_init
tap_plan 28

DATA_DIR="$TEST_DIR/data"
mkdir -p "$DATA_DIR"

REF_LIVECSV="$TEST_DIR/reference-live.csv"
RAMP_LIVECSV="$TEST_DIR/ramp-live.csv"
ALIAS_LIVECSV="$TEST_DIR/alias-live.csv"

################## Argument guards ##################

run_elbencho ramp-equals-timelimit \
    -w -t 1 -N 1 -s $FILE_SIZE --timelimit $TIME_LIMIT --ramp $TIME_LIMIT "$DATA_DIR"
assert_nok $? "a warm-up time that is not shorter than the time limit is rejected"

run_elbencho ramp-and-warmup \
    -w -t 1 -N 1 -s $FILE_SIZE --timelimit $TIME_LIMIT \
    --ramp $RAMP_SECS --warmup $RAMP_SECS "$DATA_DIR"
assert_nok $? "--ramp and its alias --warmup cannot be given at the same time"

################## Dataset for the timed runs ##################

# The dirs are created in their own run, because the timed runs below loop
# infinitely and would spend their whole time limit in the mkdirs phase.
run_elbencho prepare-dirs -d -t $NUM_THREADS -n 1 "$DATA_DIR"
assert_ok $? "creating the benchmark dirs succeeds"

################## Reference run without a warm-up period ##################

run_elbencho reference \
    -w -t $NUM_THREADS -n 1 -N $NUM_FILES -s $FILE_SIZE -b $BLOCK_SIZE \
    --infloop --timelimit $TIME_LIMIT \
    --liveint $LIVE_INTERVAL_MS --livecsv "$REF_LIVECSV" \
    "$DATA_DIR"
assert_ok $? "the reference run without a warm-up period succeeds"

REF_ELAPSED_MS="$(json_value "$ELB_JSON" WRITE last_done elapsed_time_ms)"

assert_ge "$REF_ELAPSED_MS" $(( TIME_LIMIT * 1000 )) \
    "the reference run measures the full time limit of ${TIME_LIMIT}s"
assert_le "$REF_ELAPSED_MS" $(( TIME_LIMIT * 1000 + TOLERANCE_MS )) \
    "the reference run does not measure much more than its time limit"

REF_LIVECSV_MAX_MS="$(livecsv_max_runtime_ms "$REF_LIVECSV")"

assert_ge "$REF_LIVECSV_MAX_MS" $(( (TIME_LIMIT - 1) * 1000 )) \
    "the live csv of the reference run covers the whole phase"

################## Same run with a warm-up period ##################

run_elbencho ramp \
    -w -t $NUM_THREADS -n 1 -N $NUM_FILES -s $FILE_SIZE -b $BLOCK_SIZE \
    --infloop --timelimit $TIME_LIMIT --ramp $RAMP_SECS \
    --liveint $LIVE_INTERVAL_MS --livecsv "$RAMP_LIVECSV" --lathisto \
    "$DATA_DIR"
assert_ok $? "the same run with a ${RAMP_SECS}s warm-up period succeeds"

RAMP_ELAPSED_MS="$(json_value "$ELB_JSON" WRITE last_done elapsed_time_ms)"
RAMP_BYTES="$(json_value "$ELB_JSON" WRITE last_done bytes)"
RAMP_IO_OPS="$(json_latency_histogram_sum "$ELB_JSON" WRITE IO)"

assert_ge "$RAMP_ELAPSED_MS" $(( MEASURED_SECS * 1000 - 1000 )) \
    "the warm-up run measures the remaining ${MEASURED_SECS}s of its time limit"
assert_le "$RAMP_ELAPSED_MS" $(( MEASURED_SECS * 1000 + TOLERANCE_MS )) \
    "the warm-up run does not measure much more than the remaining ${MEASURED_SECS}s"
assert_le "$RAMP_ELAPSED_MS" $(( REF_ELAPSED_MS - RAMP_SECS * 1000 + 1500 )) \
    "the warm-up run measures about ${RAMP_SECS}s less than the reference run"

assert_gt "$RAMP_BYTES" 0 \
    "the warm-up run still reports the data of its measured window"

# Every I/O of this workload is exactly one block, so the number of values in
# the latency histogram has to match the transferred bytes. This is what shows
# that the histogram was reset together with the counters, instead of still
# holding the operations of the warm-up period.
assert_eq "$(( RAMP_IO_OPS * BLOCK_SIZE ))" "$RAMP_BYTES" \
    "the latency histogram holds exactly the I/Os of the measured window"

RAMP_LIVECSV_MAX_MS="$(livecsv_max_runtime_ms "$RAMP_LIVECSV")"

assert_eq "$(livecsv_bad_rows "$RAMP_LIVECSV")" 0 \
    "the live csv of the warm-up run is well formed"
# The last live row and the elapsed time of the workers are taken at slightly
# different instants, so they normally land on the same millisecond, but the
# row can also be one live stats interval ahead. A timeline that still held the
# warm-up period would be ahead by RAMP_SECS, which is far beyond that.
assert_le "$RAMP_LIVECSV_MAX_MS" $(( RAMP_ELAPSED_MS + LIVE_INTERVAL_MS )) \
    "the live csv timeline of the warm-up run stays within the measured window"
assert_le "$RAMP_LIVECSV_MAX_MS" $(( REF_LIVECSV_MAX_MS - RAMP_SECS * 1000 + 1500 )) \
    "the live csv of the warm-up run is shorter than the one of the reference run"

################## The --warmup alias does the same ##################

run_elbencho warmup-alias \
    -w -t $NUM_THREADS -n 1 -N $NUM_FILES -s $FILE_SIZE -b $BLOCK_SIZE \
    --infloop --timelimit $TIME_LIMIT --warmup $RAMP_SECS \
    --liveint $LIVE_INTERVAL_MS --livecsv "$ALIAS_LIVECSV" \
    "$DATA_DIR"
assert_ok $? "a run with the --warmup alias succeeds"

ALIAS_ELAPSED_MS="$(json_value "$ELB_JSON" WRITE last_done elapsed_time_ms)"

assert_ge "$ALIAS_ELAPSED_MS" $(( MEASURED_SECS * 1000 - 1000 )) \
    "the --warmup alias also measures the remaining ${MEASURED_SECS}s only"
assert_le "$ALIAS_ELAPSED_MS" $(( MEASURED_SECS * 1000 + TOLERANCE_MS )) \
    "the --warmup alias does not measure much more than the remaining ${MEASURED_SECS}s"

############## Warm-up of a count-based workload without a time limit ##########
#
# A fixed workload ends when it is done and not at a time limit, so the write
# throttle below is what makes its duration predictable: each of the threads
# writes its half of the files at THROTTLED_RATE bytes per second, which takes
# a few seconds and thus outlasts the warm-up period. The measured window of
# the warm-up run has to be shorter than the whole run by the warm-up time.

THROTTLED_FILES=4
THROTTLED_FILE_SIZE=$((1024 * 1024))
THROTTLED_RATE=$((1024 * 1024))
THROTTLED_RAMP=2

THROTTLED_DIR="$TEST_DIR/throttled"
mkdir -p "$THROTTLED_DIR"

run_elbencho throttled-prepare-dirs -d -t $NUM_THREADS -n 1 "$THROTTLED_DIR"
assert_ok $? "creating the dirs of the throttled workload succeeds"

run_elbencho throttled-reference \
    -w -t $NUM_THREADS -n 1 -N $THROTTLED_FILES -s $THROTTLED_FILE_SIZE -b $BLOCK_SIZE \
    --limitwrite $THROTTLED_RATE \
    "$THROTTLED_DIR"
assert_ok $? "the throttled reference run without a warm-up period succeeds"

THROTTLED_REF_ELAPSED_MS="$(json_value "$ELB_JSON" WRITE last_done elapsed_time_ms)"

run_elbencho throttled-ramp \
    -w -t $NUM_THREADS -n 1 -N $THROTTLED_FILES -s $THROTTLED_FILE_SIZE -b $BLOCK_SIZE \
    --limitwrite $THROTTLED_RATE --ramp $THROTTLED_RAMP \
    "$THROTTLED_DIR"
assert_ok $? "--ramp without --timelimit works for a count-based workload"

THROTTLED_RAMP_ELAPSED_MS="$(json_value "$ELB_JSON" WRITE last_done elapsed_time_ms)"
THROTTLED_RAMP_BYTES="$(json_value "$ELB_JSON" WRITE last_done bytes)"

assert_gt "$THROTTLED_RAMP_BYTES" 0 \
    "the count-based warm-up run reports the data of its measured window"
assert_le "$THROTTLED_RAMP_ELAPSED_MS" \
    $(( THROTTLED_REF_ELAPSED_MS - THROTTLED_RAMP * 1000 + 1500 )) \
    "the count-based warm-up run measures about ${THROTTLED_RAMP}s less than its reference run"

################## Workload shorter than the warm-up period ##################
#
# There is no measured window left in that case, so the phase has to fail
# instead of reporting the warm-up data as a result.

QUICK_FILE="$TEST_DIR/quickfile"
QUICK_RAMP=5

run_elbencho quick-workload-ramp \
    -w -t 1 -s $FILE_SIZE -b $BLOCK_SIZE --ramp $QUICK_RAMP "$QUICK_FILE"
assert_nok $? "a workload that is through before the end of the warm-up period fails"
assert_match "$(cat "$ELB_OUT")" \
    'ERROR: Workload completed before the "--ramp" window expired' \
    "the failed run explains that its workload did not outlast the warm-up period"

run_elbencho quick-workload-ramp-timelimit \
    -w -t 1 -s $FILE_SIZE -b $BLOCK_SIZE \
    --timelimit $(( QUICK_RAMP + 1 )) --ramp $QUICK_RAMP "$QUICK_FILE"
assert_nok $? "the same holds when a time limit is given"

# Only the read and write phases have a warm-up period, so the dir creation and
# the deletions of this run are not expected to outlast it, even though they
# are through in no time.
ALL_PHASES_DIR="$TEST_DIR/allphases"
mkdir -p "$ALL_PHASES_DIR"

run_elbencho all-phases-ramp \
    -d -w -r -D -F -t $NUM_THREADS -n 1 -N $THROTTLED_FILES \
    -s $THROTTLED_FILE_SIZE -b $BLOCK_SIZE \
    --limitwrite $THROTTLED_RATE --limitread $THROTTLED_RATE --ramp $THROTTLED_RAMP \
    "$ALL_PHASES_DIR"
assert_ok $? "the short preparation and cleanup phases of a full run have no warm-up period"
