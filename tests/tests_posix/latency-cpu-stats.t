#!/bin/bash
#
# Latency and CPU utilization statistics.
#
# Covers the reporting options that add rows to the results and keys to the json
# result file: "--lat", "--lathisto", "--lathistogrpd", "--latpercent",
# "--latpercent9s" and "--cpu". A run without them serves as the negative
# control, because what makes these options testable is that their output is
# absent by default.
#
# This also covers all phases of a dir mode benchmark in a single invocation.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1

NUM_THREADS=2
NUM_DIRS=2
NUM_FILES=4
FILE_SIZE=$((64 * 1024))
BLOCK_SIZE=$((16 * 1024))

# Single thread with 8 files for the histogram run, so that the expected number
# of operations is easy to follow: 8 entries and 8 * 64KiB / 16KiB = 32 IOs.
HISTO_FILES=8
HISTO_ENTRY_OPS=$HISTO_FILES
HISTO_IO_OPS=$(( HISTO_FILES * FILE_SIZE / BLOCK_SIZE ))

test_init
tap_plan 36

DATA_DIR="$TEST_DIR/data"
NOLAT_DIR="$TEST_DIR/nolat"
HISTO_DIR="$TEST_DIR/histo"
PERCENT_DIR="$TEST_DIR/percent"

mkdir -p "$DATA_DIR" "$NOLAT_DIR" "$HISTO_DIR" "$PERCENT_DIR"

################## All phases in one run, with --lat and --cpu ##################

run_elbencho lat \
    -d -w -r -F -D \
    -t $NUM_THREADS -n $NUM_DIRS -N $NUM_FILES -s $FILE_SIZE -b $BLOCK_SIZE \
    --lat --cpu \
    "$DATA_DIR"
assert_ok $? "all phases in a single invocation succeed"

assert_eq "$(json_phases "$ELB_JSON")" "MKDIRS WRITE READ RMFILES RMDIRS" \
    "the single invocation ran all five phases in the expected order"

# An absent value is an empty string, which is not a number, so asserting a
# number of at least zero also asserts that the value is there at all.
assert_ge "$(json_latency "$ELB_JSON" MKDIRS entries avg_us)" 0 \
    "mkdirs phase reports an entry latency"
assert_eq "$(json_latency "$ELB_JSON" MKDIRS IO avg_us)" "" \
    "mkdirs phase reports no IO latency, because it transfers no data"

assert_ge "$(json_latency "$ELB_JSON" WRITE entries avg_us)" 0 \
    "write phase reports an entry latency"
assert_ge "$(json_latency "$ELB_JSON" WRITE IO min_us)" 0 \
    "write phase reports an IO latency"

LAT_MIN="$(json_latency "$ELB_JSON" WRITE IO min_us)"
LAT_AVG="$(json_latency "$ELB_JSON" WRITE IO avg_us)"
LAT_MAX="$(json_latency "$ELB_JSON" WRITE IO max_us)"

assert_ge "$LAT_AVG" "$LAT_MIN" "write phase IO latency average is not below its minimum"
assert_ge "$LAT_MAX" "$LAT_AVG" "write phase IO latency maximum is not below its average"

assert_ge "$(json_latency "$ELB_JSON" READ entries avg_us)" 0 \
    "read phase reports an entry latency"
assert_ge "$(json_latency "$ELB_JSON" READ IO avg_us)" 0 \
    "read phase reports an IO latency"

# Entry latency is reported as dirs latency in the dir phases and as files
# latency in the file phases, IO latency only where data is transferred.
assert_eq "$(res_row_count "$ELB_RES" 'Dirs latency')" 2 \
    "the result file has a dirs latency row for the mkdirs and rmdirs phase"
assert_eq "$(res_row_count "$ELB_RES" 'Files latency')" 3 \
    "the result file has a files latency row for the write, read and rmfiles phase"
assert_eq "$(res_row_count "$ELB_RES" 'IO latency')" 2 \
    "the result file has an IO latency row for the write and read phase"

assert_eq "$(res_row_count "$ELB_RES" 'CPU util %')" 5 \
    "--cpu adds a CPU utilization row for each of the five phases"

assert_ge "$(json_value "$ELB_JSON" WRITE last_done 'cpu%')" 0 \
    "the reported CPU utilization is not negative"
assert_le "$(json_value "$ELB_JSON" WRITE last_done 'cpu%')" 100 \
    "the reported CPU utilization is not above 100 percent"

################## Negative control without --lat and --cpu ##################

run_elbencho nolat \
    -d -w \
    -t $NUM_THREADS -n $NUM_DIRS -N $NUM_FILES -s $FILE_SIZE -b $BLOCK_SIZE \
    "$NOLAT_DIR"
assert_ok $? "the same benchmark without --lat and --cpu succeeds"

assert_eq "$(json_has_key "$ELB_JSON" WRITE last_done latency)" "false" \
    "without --lat there is no latency subtree in the json result file"
assert_eq "$(res_row_count "$ELB_RES" 'CPU util %')" 0 \
    "without --cpu there is no CPU utilization row in the result file"

# The counter itself is written either way, i.e. "--cpu" only controls the
# results output, not what gets measured.
assert_ge "$(json_value "$ELB_JSON" WRITE last_done 'cpu%')" 0 \
    "the json result file holds a CPU utilization value even without --cpu"

################## File mode has no entry latency ##################

run_elbencho latfile \
    -w \
    -t $NUM_THREADS -s $((1024 * 1024)) -b $((256 * 1024)) \
    --lat \
    "$TEST_DIR/latfile[1-2]"
assert_ok $? "write phase in file mode with --lat succeeds"

assert_ge "$(json_latency "$ELB_JSON" WRITE IO avg_us)" 0 \
    "file mode reports an IO latency"
assert_eq "$(json_latency "$ELB_JSON" WRITE entries avg_us)" "" \
    "file mode reports no entry latency, because it does not open a file per entry"
assert_eq "$(res_row_count "$ELB_RES" 'Files latency')" 0 \
    "the result file has no files latency row in file mode"
assert_eq "$(res_row_count "$ELB_RES" 'IO latency')" 1 \
    "the result file has an IO latency row in file mode"

################## Histogram, grouped histogram and percentiles ##################

run_elbencho histo \
    -d -w \
    -t 1 -n 1 -N $HISTO_FILES -s $FILE_SIZE -b $BLOCK_SIZE \
    --lathisto --lathistogrpd --latpercent --latpercent9s 2 \
    "$HISTO_DIR"
assert_ok $? "write phase with the latency histogram options succeeds"

# Only non-empty buckets are written, so the number of buckets is not
# predictable, but every operation has to be in exactly one bucket.
assert_eq "$(json_latency_histogram_sum "$ELB_JSON" WRITE IO)" "$HISTO_IO_OPS" \
    "the IO latency histogram accounts for all $HISTO_IO_OPS IO operations"
assert_eq "$(json_latency_histogram_sum "$ELB_JSON" WRITE entries)" "$HISTO_ENTRY_OPS" \
    "the entry latency histogram accounts for all $HISTO_ENTRY_OPS entries"

assert_eq "$(json_latency "$ELB_JSON" WRITE IO min_us)" "" \
    "--lathisto alone adds no minimum, average and maximum to the json result file"

assert_eq "$(res_row_count "$ELB_RES" 'IO lat hist')" 1 \
    "--lathisto adds an IO latency histogram row"
assert_eq "$(res_row_count "$ELB_RES" 'IO lat grpd')" 1 \
    "--lathistogrpd adds a grouped IO latency histogram row"
assert_eq "$(res_row_count "$ELB_RES" 'IO lat % us')" 1 \
    "--latpercent adds an IO latency percentiles row"
assert_eq "$(res_row_field_count "$ELB_RES" 'IO lat % us' '%<=')" 6 \
    "--latpercent9s 2 extends the percentiles row to 6 values"
assert_eq "$(res_row_count "$ELB_RES" 'IO latency')" 0 \
    "the histogram options alone add no plain IO latency row"

run_elbencho percent \
    -d -w \
    -t 1 -n 1 -N $HISTO_FILES -s $FILE_SIZE -b $BLOCK_SIZE \
    --latpercent \
    "$PERCENT_DIR"
assert_ok $? "write phase with --latpercent alone succeeds"

assert_eq "$(res_row_field_count "$ELB_RES" 'IO lat % us' '%<=')" 4 \
    "--latpercent without --latpercent9s reports 4 percentile values"
