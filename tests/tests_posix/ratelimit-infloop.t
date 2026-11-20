#!/bin/bash
#
# Rate limits must hold across the passes of an infinite I/O loop.
#
# "--infloop" repeats the workload of a phase until the time limit is reached.
# A pass over only a few small files finishes in a fraction of a second, so a
# rate limiter that is restarted at the beginning of each pass hands out a fresh
# budget per pass and effectively does not limit anything. The limit is 10 KiB/s
# and a pass writes or reads only 8 KiB, so a single pass never reaches the
# limit and only the accumulation across passes can.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1

NUM_FILES=2         # per thread; 2 x 4 KiB = 8 KiB per pass
FILE_SIZE=4096
LIMIT_BPS=10240
TIMELIMIT=3
# Budget of the time limit plus the first second and a margin for the final second
MAX_BYTES=$((LIMIT_BPS * (TIMELIMIT + 2)))
# More than one pass, so the run really looped
MIN_BYTES=$((NUM_FILES * FILE_SIZE))

test_init
tap_plan 7

DATA_DIR="$TEST_DIR/data"
mkdir -p "$DATA_DIR"

################## Create the files without a limit ##################

run_elbencho prepare \
    -d -w -t 1 -n 1 -N $NUM_FILES -s $FILE_SIZE -b $FILE_SIZE "$DATA_DIR"
assert_ok $? "creating the files succeeds"

################## Limited write with infloop ##################

run_elbencho write-infloop \
    -w -t 1 -n 1 -N $NUM_FILES -s $FILE_SIZE -b $FILE_SIZE \
    --limitwrite $LIMIT_BPS --infloop --timelimit $TIMELIMIT "$DATA_DIR"
assert_ok $? "limited write with infloop ends at the time limit"

WRITTEN="$(json_value "$ELB_JSON" WRITE last_done bytes)"
assert_gt "$WRITTEN" "$MIN_BYTES" "limited write with infloop ran more than one pass"
assert_le "$WRITTEN" "$MAX_BYTES" \
    "write limit of $LIMIT_BPS B/s holds across infloop passes (wrote $WRITTEN B, max $MAX_BYTES B)"

################## Limited read with infloop ##################

run_elbencho read-infloop \
    -r -t 1 -n 1 -N $NUM_FILES -s $FILE_SIZE -b $FILE_SIZE \
    --limitread $LIMIT_BPS --infloop --timelimit $TIMELIMIT "$DATA_DIR"
assert_ok $? "limited read with infloop ends at the time limit"

READ="$(json_value "$ELB_JSON" READ last_done bytes)"
assert_gt "$READ" "$MIN_BYTES" "limited read with infloop ran more than one pass"
assert_le "$READ" "$MAX_BYTES" \
    "read limit of $LIMIT_BPS B/s holds across infloop passes (read $READ B, max $MAX_BYTES B)"
