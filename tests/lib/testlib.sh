#!/bin/bash
#
# Shared helper library for the elbencho black-box test suite.
#
# Source this at the top of every test script:
#   source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
#
# Provides TAP output, per-test temporary dirs, watchdog timeouts, free TCP port
# selection and helpers to inspect elbencho result files. See tests/README.md.

########################### Environment defaults ############################
#
# All of these are normally set by tests/run-tests.sh. The defaults below make a
# test script work when it is executed directly, without the wrapper.

TESTLIB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TESTS_DIR="$(cd "$TESTLIB_DIR/.." && pwd)"
REPO_DIR="$(cd "$TESTS_DIR/.." && pwd)"

: ${ELBENCHO_TEST_BIN:="$REPO_DIR/bin/elbencho"}
: ${ELBENCHO_TEST_TMP:="$TESTS_DIR/tmp"}
: ${ELBENCHO_TEST_KEEP:="0"}
: ${ELBENCHO_TEST_CMD_TIMEOUT:="60"}
: ${ELBENCHO_TEST_SCRIPT_TIMEOUT:="300"}
: ${ELBENCHO_TEST_LIB:="$TESTLIB_DIR"}
: ${ELBENCHO_TEST_MINIO:="$ELBENCHO_TEST_TMP/minio"}
: ${ELBENCHO_TEST_TRACE:="0"}
: ${ELBENCHO_TEST_TRACE_DIR:="$ELBENCHO_TEST_TMP/commands"}

# Exact header line written by Statistics::prepLiveCSVFile(). Note the trailing
# comma after the last column, which the C++ code really does emit.
LIVECSV_HEADER='ISO Date,Label,Phase,RuntimeMS,Rank,MixType,Done%,DoneBytes,MiB/s,IOPS,Entries,Entries/s,Lat Ent us,Lat IO us,Active,CPU,Service,'

TAP_COUNT=0
TAP_FAILED=0
REGISTERED_PIDS=""
WATCHDOG_PID=""
TEST_NAME=""
TEST_DIR=""

################################ TAP output #################################

tap_diag()
{
    echo "# $*"
}

# Emit the contents of a file as TAP diagnostics (max 40 lines).
tap_diag_file()
{
    if [ -s "$1" ]; then
        head -40 "$1" | sed -e 's/^/#   /'
    fi
}

tap_plan()
{
    echo "1..$1"
}

tap_ok()
{
    TAP_COUNT=$((TAP_COUNT+1))
    echo "ok $TAP_COUNT - $*"
}

tap_fail()
{
    TAP_COUNT=$((TAP_COUNT+1))
    TAP_FAILED=$((TAP_FAILED+1))
    echo "not ok $TAP_COUNT - $*"
}

tap_skip()
{
    TAP_COUNT=$((TAP_COUNT+1))
    echo "ok $TAP_COUNT - $* # SKIP"
}

# Skip the whole test file. Valid before or after test_init().
tap_skip_all()
{
    echo "1..0 # SKIP $*"
    exit 0
}

tap_bail()
{
    echo "Bail out!  $*"
    TAP_FAILED=$((TAP_FAILED+1))
    exit 1
}

############################## Command tracing ##############################
#
# "run-tests.sh -c" turns this on. It writes every command that a test executes
# together with its console output to one transcript file per test script below
# $ELBENCHO_TEST_TRACE_DIR. That is what makes it possible to confirm that a
# test really does what it claims, also when it does not report a failure: the
# ".out" files below $TEST_DIR hold the same output, but that dir is deleted
# when the test ends and it never holds the command lines themselves.
#
# The transcript is a file and not a TAP diagnostic on purpose. Several of the
# traced helpers run inside a command substitution of their callers, e.g.
# "count=$(aws_s3api list-objects-v2 ...)" and "spdk_rpc ... | jq", so a line
# written to stdout there would end up in the caller's variable or in jq's
# input instead of in the output of the test. Writing it to a dup of stdout
# instead would escape the command substitution, but such a file descriptor is
# inherited by every background server, which would keep prove waiting for end
# of file on the test's stdout.
#
# Set TRACE_QUIET=1 with "local" in a function that polls, so that it does not
# fill the transcript with hundreds of identical lines.

TRACE_FILE=""       # empty means no transcript, e.g. when tracing is disabled
TRACE_QUIET=0
TRACE_LOGS=()       # log files of background processes, appended on test exit

# Number of lines per background process log that go into the transcript. These
# logs are written for as long as the process runs and service instances are
# started with "--log 1", so the end of the log is the interesting part.
TRACE_LOG_MAX_LINES=200

# Prepare the transcript of this test script. Called at the end of this file,
# not from test_init(), because require_build_feature(), require_minio() and
# require_spdk() already run commands before test_init() is reached.
#
# The file is truncated instead of appended to, so that running a single test
# script directly ("ELBENCHO_TEST_TRACE=1 tests/tests_posix/smallfiles-dirmode.t")
# does not accumulate the output of all previous runs. run-tests.sh empties the
# whole dir, which is what removes the transcripts of the test scripts that are
# not part of the current run.
trace_init()
{
    local name

    [ "$ELBENCHO_TEST_TRACE" = "1" ] || return 0

    # Only test scripts get a transcript. The interactive servers in
    # tests/tools/ source this library as well, but they print their own
    # command lines and are not part of a prove run.
    case "$(basename "$0")" in
        *.t) ;;
        *)   return 0 ;;
    esac

    name="$(basename "$0" .t)"

    mkdir -p "$ELBENCHO_TEST_TRACE_DIR" 2>/dev/null
    : > "$ELBENCHO_TEST_TRACE_DIR/$name.log" 2>/dev/null
    if [ $? -ne 0 ]; then
        tap_diag "WARNING: unable to write a command transcript in $ELBENCHO_TEST_TRACE_DIR"
        return 0
    fi

    TRACE_FILE="$ELBENCHO_TEST_TRACE_DIR/$name.log"

    # run-tests.sh empties this dir, so the transcripts always belong to the
    # most recent run. The date makes that visible in the file itself.
    {
        echo "=== test script: $0"
        echo "=== elbencho binary: $ELBENCHO_TEST_BIN"
        echo "=== started: $(date '+%F %T')"
    } >> "$TRACE_FILE"

    return 0
}

# Whether the next command should be traced.
trace_on()
{
    [ -n "$TRACE_FILE" ] && [ "$TRACE_QUIET" -eq 0 ]
}

# Current time in milliseconds, for the runtime of a traced command.
#
# "date +%s%3N" must not be used: the uutils reimplementation of coreutils,
# which some distributions now ship as /usr/bin/date, ignores the field width
# and returns nanoseconds, which would report a runtime a million times too
# long. bash's own EPOCHREALTIME avoids the fork and any date implementation at
# all. It uses the decimal separator of the locale, hence the [.,] pattern. The
# fallback is for bash 4, where "%N" is zero padded to 9 digits in both date
# implementations and 19 digits still fit into bash's 64 bit arithmetic.
trace_now_ms()
{
    local stamp

    if [ -n "$EPOCHREALTIME" ]; then
        stamp="${EPOCHREALTIME/[.,]/}"
        echo $(( stamp / 1000 ))
    else
        stamp="$(date +%s%N)"
        echo $(( stamp / 1000000 ))
    fi
}

# trace_cmd LABEL ARGS...
#
# The command line of one traced invocation. "printf %q" quotes the arguments
# so that the line can be pasted into a shell as it is, which matters because
# several of them contain spaces or brackets. The "$#" check is needed because
# printf repeats its format once with an empty argument list, which would
# append a stray ''.
trace_cmd()
{
    local label="$1"
    local args=""
    shift

    trace_on || return 0

    [ $# -gt 0 ] && args="$(printf ' %q' "$@")"

    {
        echo
        echo "=== $label"
        echo "\$$args"
    } >> "$TRACE_FILE"

    return 0
}

# trace_result LABEL EXITCODE ELAPSED_MS [OUTFILE]
#
# Exit code, runtime and console output of a traced invocation. An empty
# ELAPSED_MS omits the runtime, for the launchers that have nothing to measure.
# The output is never truncated: being the complete record is the reason for
# the transcript to exist.
trace_result()
{
    local label="$1"
    local rc="$2"
    local ms="$3"
    local outfile="$4"

    trace_on || return 0

    if [ -n "$ms" ]; then
        echo "--- $label: exit code $rc after ${ms}ms" >> "$TRACE_FILE"
    else
        echo "--- $label: exit code $rc" >> "$TRACE_FILE"
    fi

    if [ -n "$outfile" ] && [ -s "$outfile" ]; then
        sed -e 's/^/  /' "$outfile" >> "$TRACE_FILE"
    fi

    return 0
}

# trace_add_log FILE
#
# Register the log file of a background process. There is nothing useful to
# show when such a process starts, because it keeps writing its log for as long
# as it runs, so the content is collected by trace_dump_logs() when the test
# ends. An array is used instead of a space separated string, because these are
# paths and "-T" may well point to a dir whose name contains a space.
trace_add_log()
{
    local known

    trace_on || return 0

    # The start retry loops register the same file again on every attempt.
    for known in "${TRACE_LOGS[@]}"; do
        [ "$known" = "$1" ] && return 0
    done

    TRACE_LOGS+=("$1")

    return 0
}

# Append the registered background process logs to the transcript. Called from
# on_exit() after kill_registered(), so that the processes have flushed their
# output, and before $TEST_DIR gets removed, which is where these logs live.
trace_dump_logs()
{
    local log

    [ -n "$TRACE_FILE" ] || return 0

    for log in "${TRACE_LOGS[@]}"; do
        [ -s "$log" ] || continue

        {
            echo
            echo "=== last $TRACE_LOG_MAX_LINES lines of $log"
            tail -"$TRACE_LOG_MAX_LINES" "$log" | sed -e 's/^/  /'
        } >> "$TRACE_FILE"
    done

    return 0
}

############################### Assertions ##################################

is_number()
{
    case "$1" in
        ''|*[!0-9]*) return 1 ;;
        *)           return 0 ;;
    esac
}

assert_eq()
{
    if [ "$1" = "$2" ]; then
        tap_ok "$3"
    else
        tap_fail "$3"
        tap_diag "  got:      '$1'"
        tap_diag "  expected: '$2'"
    fi
}

assert_ne()
{
    if [ "$1" != "$2" ]; then
        tap_ok "$3"
    else
        tap_fail "$3"
        tap_diag "  got '$1', expected anything else"
    fi
}

# assert_gt VALUE MINIMUM DESC (numeric, exclusive)
assert_gt()
{
    if is_number "$1" && [ "$1" -gt "$2" ]; then
        tap_ok "$3"
    else
        tap_fail "$3"
        tap_diag "  got: '$1', expected a number greater than $2"
    fi
}

# assert_ge VALUE MINIMUM DESC (numeric, inclusive)
assert_ge()
{
    if is_number "$1" && [ "$1" -ge "$2" ]; then
        tap_ok "$3"
    else
        tap_fail "$3"
        tap_diag "  got: '$1', expected a number of at least $2"
    fi
}

# assert_lt VALUE LIMIT DESC (numeric, exclusive)
assert_lt()
{
    if is_number "$1" && [ "$1" -lt "$2" ]; then
        tap_ok "$3"
    else
        tap_fail "$3"
        tap_diag "  got: '$1', expected a number less than $2"
    fi
}

# assert_le VALUE MAXIMUM DESC (numeric, inclusive)
assert_le()
{
    if is_number "$1" && [ "$1" -le "$2" ]; then
        tap_ok "$3"
    else
        tap_fail "$3"
        tap_diag "  got: '$1', expected a number of at most $2"
    fi
}

assert_match()
{
    if echo "$1" | grep -qE "$2"; then
        tap_ok "$3"
    else
        tap_fail "$3"
        tap_diag "  got:     '$1'"
        tap_diag "  no match for regex: '$2'"
    fi
}

# assert_ok EXITCODE DESC
assert_ok()
{
    if [ "$1" -eq 0 ]; then
        tap_ok "$2"
    else
        tap_fail "$2"
        tap_diag "  exit code: $1"
    fi
}

# assert_nok EXITCODE DESC (command was expected to fail)
assert_nok()
{
    if [ "$1" -ne 0 ]; then
        tap_ok "$2"
    else
        tap_fail "$2"
        tap_diag "  command unexpectedly succeeded"
    fi
}

######################## Test setup, cleanup, watchdog ######################

# Derive the test name from the script name, create its temporary dir and arm
# the cleanup trap plus the script watchdog.
test_init()
{
    TEST_NAME="$(basename "$0")"
    TEST_NAME="${TEST_NAME%.t}"
    TEST_DIR="$ELBENCHO_TEST_TMP/$TEST_NAME"

    if [ ! -x "$ELBENCHO_TEST_BIN" ]; then
        tap_bail "elbencho binary not found or not executable: $ELBENCHO_TEST_BIN"
    fi

    mkdir -p "$ELBENCHO_TEST_TMP"
    rm -rf "$TEST_DIR"
    mkdir -p "$TEST_DIR"
    if [ $? -ne 0 ]; then
        tap_bail "Unable to create temporary dir for test: $TEST_DIR"
    fi

    trap on_exit EXIT
    trap on_signal INT TERM

    start_watchdog
}

on_signal()
{
    tap_diag "Test terminated by signal (watchdog timeout or interrupt)."
    TAP_FAILED=$((TAP_FAILED+1))
    exit 1
}

on_exit()
{
    local exitcode=$?

    trap - EXIT INT TERM

    stop_watchdog
    kill_registered

    # After kill_registered, so that the background processes have written
    # everything, and before the temporary dir is removed, because that is
    # where their logs are.
    trace_dump_logs

    if [ "$TAP_FAILED" -gt 0 ] && [ "$ELBENCHO_TEST_KEEP" = "1" ]; then
        tap_diag "Keeping temporary dir of failed test: $TEST_DIR"
    else
        rm -rf "$TEST_DIR"
    fi

    if [ -n "$TRACE_FILE" ]; then
        tap_diag "Command transcript: $TRACE_FILE"
    fi

    if [ "$TAP_FAILED" -gt 0 ] && [ "$exitcode" -eq 0 ]; then
        exitcode=1
    fi

    exit $exitcode
}

# Kill this script if it hangs anywhere outside of a "timeout"-wrapped command,
# e.g. in a background helper like minio. Output is redirected so that the
# background subshell does not hold the test's stdout open, which would keep
# "prove" waiting for EOF.
#
# The wait is a loop of short sleeps instead of a single long one, because
# killing the subshell would leave a long running sleep behind as an orphan.
# "$$" is the pid of the test script itself, also inside the subshell.
start_watchdog()
{
    ( waited=0
      while [ $waited -lt "$ELBENCHO_TEST_SCRIPT_TIMEOUT" ]; do
          sleep 1
          waited=$((waited+1))
      done
      kill -TERM $$ ) >/dev/null 2>&1 &

    WATCHDOG_PID=$!
}

stop_watchdog()
{
    if [ -n "$WATCHDOG_PID" ]; then
        kill -KILL "$WATCHDOG_PID" >/dev/null 2>&1
        wait "$WATCHDOG_PID" 2>/dev/null
        WATCHDOG_PID=""
    fi
}

# Register a background process for automatic termination on test exit.
register_pid()
{
    REGISTERED_PIDS="$REGISTERED_PIDS $1"
}

kill_registered()
{
    local pid
    local alive
    local waited

    for pid in $REGISTERED_PIDS; do
        kill -TERM "$pid" >/dev/null 2>&1
    done

    waited=0
    while [ $waited -lt 50 ]; do
        alive=0
        for pid in $REGISTERED_PIDS; do
            kill -0 "$pid" >/dev/null 2>&1 && alive=1
        done
        [ $alive -eq 0 ] && break
        sleep 0.1
        waited=$((waited+1))
    done

    for pid in $REGISTERED_PIDS; do
        kill -KILL "$pid" >/dev/null 2>&1
        wait "$pid" 2>/dev/null
    done

    REGISTERED_PIDS=""
}

############################ elbencho invocation ############################

# run_elbencho TAG ARGS...
#
# Runs the binary under a watchdog timeout with quiet output and per-invocation
# result files under "$TEST_DIR/TAG.{txt,csv,json,out}". Explicit result file
# paths keep /var/tmp/elbencho_results_$USER clean and avoid appending to a csv
# file of a different column count. Returns the exit code of the run.
run_elbencho()
{
    local tag="$1"
    shift

    ELB_JSON="$TEST_DIR/$tag.json"
    ELB_CSV="$TEST_DIR/$tag.csv"
    ELB_RES="$TEST_DIR/$tag.txt"
    ELB_OUT="$TEST_DIR/$tag.out"

    local started="$(trace_now_ms)"

    trace_cmd "elbencho run \"$tag\"" \
        "$ELBENCHO_TEST_BIN" \
        --nolive --no0usecerr \
        --resfile "$ELB_RES" --csvfile "$ELB_CSV" --jsonfile "$ELB_JSON" \
        "$@"

    timeout --kill-after=5s --signal=TERM "$ELBENCHO_TEST_CMD_TIMEOUT" \
        "$ELBENCHO_TEST_BIN" \
        --nolive --no0usecerr \
        --resfile "$ELB_RES" --csvfile "$ELB_CSV" --jsonfile "$ELB_JSON" \
        "$@" > "$ELB_OUT" 2>&1

    local rc=$?

    trace_result "elbencho run \"$tag\"" "$rc" \
        "$(( $(trace_now_ms) - started ))" "$ELB_OUT"

    if [ $rc -eq 124 ] || [ $rc -eq 137 ]; then
        tap_diag "TIMEOUT: elbencho run \"$tag\" exceeded ${ELBENCHO_TEST_CMD_TIMEOUT}s and was killed."
    fi

    if [ $rc -ne 0 ]; then
        tap_diag "Output of failed elbencho run \"$tag\":"
        tap_diag_file "$ELB_OUT"
    fi

    return $rc
}

############################### TCP ports ###################################

port_in_use()
{
    local port="$1"

    if command -v ss >/dev/null 2>&1; then
        ss -ltn 2>/dev/null | awk '{print $4}' | grep -qE "[:.]${port}\$" && return 0
        return 1
    fi

    (exec 3<>"/dev/tcp/127.0.0.1/$port") >/dev/null 2>&1 && return 0
    return 1
}

# Port range that the netbench tests allocate their port pairs from, see
# tests/lib/netbench.sh. It is skipped here, because a netbench data port is not
# bound while its service is idle, so handing one out to another test would
# make that netbench run fail later with an "address already in use".
FIND_FREE_PORT_SKIP_LO=20000
FIND_FREE_PORT_SKIP_HI=21899

# Echo a random port that is currently not listened on. Because the port can
# still be taken between check and bind, callers should retry the whole
# start-and-wait sequence on failure.
find_free_port()
{
    local port
    local tries=0

    while [ $tries -lt 50 ]; do
        tries=$((tries+1))
        port=$(( 20000 + (RANDOM % 40000) ))

        if [ "$port" -ge "$FIND_FREE_PORT_SKIP_LO" ] && \
           [ "$port" -le "$FIND_FREE_PORT_SKIP_HI" ]; then
            continue
        fi

        if ! port_in_use "$port"; then
            echo "$port"
            return 0
        fi
    done

    return 1
}

# Echo two consecutive free ports ("P P+1"). Needed for a "host:[P1-P2]" host
# list, because that syntax is a numeric range, so the two random ports of
# find_free_port cannot be used for it.
find_free_port_pair()
{
    local port
    local tries=0

    while [ $tries -lt 50 ]; do
        tries=$((tries+1))
        port=$(( 20000 + (RANDOM % 40000) ))

        if [ "$port" -ge "$((FIND_FREE_PORT_SKIP_LO - 1))" ] && \
           [ "$port" -le "$FIND_FREE_PORT_SKIP_HI" ]; then
            continue
        fi

        if ! port_in_use "$port" && ! port_in_use "$((port + 1))"; then
            echo "$port $((port + 1))"
            return 0
        fi
    done

    return 1
}

# wait_for_port HOST PORT SECONDS
wait_for_port()
{
    local host="$1"
    local port="$2"
    local maxloops=$(( $3 * 10 ))
    local waited=0

    while [ $waited -lt $maxloops ]; do
        if (exec 3<>"/dev/tcp/$host/$port") >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
        waited=$((waited+1))
    done

    return 1
}

# wait_for_port_gone HOST PORT SECONDS
wait_for_port_gone()
{
    local host="$1"
    local port="$2"
    local maxloops=$(( $3 * 10 ))
    local waited=0

    while [ $waited -lt $maxloops ]; do
        if ! (exec 3<>"/dev/tcp/$host/$port") >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
        waited=$((waited+1))
    done

    return 1
}

# wait_for_pid_gone PID SECONDS
# A daemonized service is not a child of the test script, so "wait" cannot be
# used on it and its pid is the only reliable indicator that it really ended.
wait_for_pid_gone()
{
    local pid="$1"
    local maxloops=$(( $2 * 10 ))
    local waited=0

    while [ $waited -lt $maxloops ]; do
        if ! kill -0 "$pid" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
        waited=$((waited+1))
    done

    return 1
}

# wait_for_file_line FILE REGEX SECONDS
# Waits for a line matching the regex to appear in the given file, which may
# not exist yet when the wait starts.
wait_for_file_line()
{
    local file="$1"
    local regex="$2"
    local maxloops=$(( $3 * 10 ))
    local waited=0

    while [ $waited -lt $maxloops ]; do
        if [ -f "$file" ] && grep -qE "$regex" "$file" 2>/dev/null; then
            return 0
        fi
        sleep 0.1
        waited=$((waited+1))
    done

    return 1
}

########################## Result file inspection ###########################
#
# The json result file is in json-lines format with one object per benchmark
# phase, and the boost property_tree writer quotes every scalar, so all numeric
# comparisons are done on the string values as printed by jq.

# json_value JSONFILE PHASE SECTION KEY
# SECTION is "first_done" or "last_done". Missing keys yield "0", because
# elbencho omits counters whose total is zero.
json_value()
{
    jq -r --arg p "$2" --arg s "$3" --arg k "$4" \
        'select(.phase_type == $p) | .[$s][$k] // "0"' "$1" 2>/dev/null | head -1
}

# json_value_rwmix JSONFILE PHASE SECTION KEY
# Like json_value, but for the "rwmix_read" subtree that holds the read side of
# a mixed read/write phase.
json_value_rwmix()
{
    jq -r --arg p "$2" --arg s "$3" --arg k "$4" \
        'select(.phase_type == $p) | .[$s].rwmix_read[$k] // "0"' "$1" 2>/dev/null | head -1
}

# json_config JSONFILE PHASE KEY
json_config()
{
    jq -r --arg p "$2" --arg k "$3" \
        'select(.phase_type == $p) | .config[$k] // ""' "$1" 2>/dev/null | head -1
}

# json_phases JSONFILE -> space separated list of phase names in file order
json_phases()
{
    jq -r '.phase_type' "$1" 2>/dev/null | tr '\n' ' ' | sed -e 's/ $//'
}

# json_latency JSONFILE PHASE TYPE KEY
# TYPE is "entries" or "IO", KEY one of min_us, avg_us, max_us. The latency
# subtree only exists with "--lat" and only below "last_done", and there is no
# entry latency when the benchmark path is a file or block device. Missing
# values yield an empty string, so presence and absence are both assertable.
json_latency()
{
    jq -r --arg p "$2" --arg t "$3" --arg k "$4" \
        'select(.phase_type == $p) | .last_done.latency[$t][$k] // ""' "$1" 2>/dev/null | head -1
}

# json_latency_histogram_sum JSONFILE PHASE TYPE
# Total number of operations in the latency histogram of "--lathisto". Only
# non-empty buckets are written, so the number of buckets is not predictable,
# but the counts have to add up to the number of operations.
json_latency_histogram_sum()
{
    jq -r --arg p "$2" --arg t "$3" \
        'select(.phase_type == $p) |
         [ .last_done.latency[$t].histogram.buckets[].count | tonumber ] | add // 0' \
        "$1" 2>/dev/null | head -1
}

# json_has_key JSONFILE PHASE SECTION KEY -> "true" or "false"
json_has_key()
{
    jq -r --arg p "$2" --arg s "$3" --arg k "$4" \
        'select(.phase_type == $p) | .[$s] | has($k)' "$1" 2>/dev/null | head -1
}

# res_row_count RESFILE LABEL
# Number of result rows with the given label in the txt result file, which gets
# the same text as the console. The label is anchored, so that e.g. "IO latency"
# does not also match the "IO lat % us" row of "--latpercent".
res_row_count()
{
    grep -cE "^ +$2 +:" "$1" 2>/dev/null
}

# res_row_field_count RESFILE LABEL SEPARATOR
# Number of separator occurrences in the given result row, e.g. the number of
# "%<=" entries in the "IO lat % us" row of "--latpercent".
res_row_field_count()
{
    grep -E "^ +$2 +:" "$1" 2>/dev/null | head -1 | grep -o -- "$3" | wc -l | tr -d ' '
}

# Value that the helpers below report when the ops log cannot be parsed, so that
# a failed check names the reason instead of showing an empty value. A multi
# threaded run needs "--opsloglock", because concurrent appends are not
# synchronized otherwise and can result in interleaved, unparseable lines.
OPSLOG_MALFORMED="MALFORMED_OPSLOG"

# opslog_count OPSLOGFILE OP_NAME -> number of completed operations of that name
opslog_count()
{
    jq -s --arg op "$2" \
        '[ .[] | select(.op_name == $op and .is_finished == true) ] | length' "$1" 2>/dev/null \
        || echo "$OPSLOG_MALFORMED"
}

# opslog_error_count OPSLOGFILE
opslog_error_count()
{
    jq -s '[ .[] | select(.is_error == true) ] | length' "$1" 2>/dev/null \
        || echo "$OPSLOG_MALFORMED"
}

# The "entry_name" field of the operations log holds the path for operations
# that work on a name (openat, mkdirat, unlinkat, all S3 operations), but the
# numeric file descriptor for read/write operations on an already open file.
# Hence the two different helpers below.

# opslog_distinct_ranks OPSLOGFILE ENTRY_SUFFIX
# Number of different worker threads that operated on the entry with the given
# name suffix. This is how "this file/object was really shared between multiple
# threads" gets verified for operations that log a path.
opslog_distinct_ranks()
{
    jq -s --arg suf "$2" \
        '[ .[] | select(.entry_name | endswith($suf)) | .worker_rank ] | unique | length' \
        "$1" 2>/dev/null || echo "$OPSLOG_MALFORMED"
}

# opslog_max_distinct_ranks OPSLOGFILE OP_NAME
# Highest number of different worker threads seen on any single entry of the
# given operation. Used to verify shared access for operations that log a file
# descriptor instead of a path.
opslog_max_distinct_ranks()
{
    jq -s --arg op "$2" \
        '[ .[] | select(.op_name == $op) ] | group_by(.entry_name) |
         map([ .[].worker_rank ] | unique | length) | max // 0' \
        "$1" 2>/dev/null || echo "$OPSLOG_MALFORMED"
}

# The three helpers below look at the accessed range rather than at operation
# counts. Every operation is logged twice, before ("is_finished": false) and
# after ("is_finished": true), so they select the "before" lines to get exactly
# one entry per operation. The optional OP_NAME narrows them to one operation
# type; without it every logged operation counts.
#
# They report the *requested* offset and length, which is what a test wants for
# checking the boundaries of "--offset"/"--size": elbencho logs the same values
# before and after an operation, so a partial completion cannot skew them.

# opslog_min_offset OPSLOGFILE [OP_NAME] - lowest accessed offset
opslog_min_offset()
{
    jq -s --arg op "${2:-}" \
        '[ .[] | select(.is_finished == false)
           | select($op == "" or .op_name == $op) | .offset ] | min // 0' "$1" 2>/dev/null
}

# opslog_max_end OPSLOGFILE [OP_NAME] - highest offset+length seen
opslog_max_end()
{
    jq -s --arg op "${2:-}" \
        '[ .[] | select(.is_finished == false)
           | select($op == "" or .op_name == $op) | (.offset + .length) ] | max // 0' "$1" 2>/dev/null
}

# opslog_bytes_sum OPSLOGFILE [OP_NAME] - total of all logged lengths
opslog_bytes_sum()
{
    jq -s --arg op "${2:-}" \
        '[ .[] | select(.is_finished == false)
           | select($op == "" or .op_name == $op) | .length ] | add // 0' "$1" 2>/dev/null
}

# livecsv_header LIVECSVFILE
livecsv_header()
{
    head -1 "$1" 2>/dev/null
}

# livecsv_bad_rows LIVECSVFILE
# Number of data rows whose field count differs from the header's. Live rows are
# only written on "--liveint" ticks, so a short run may legitimately have none;
# this only checks the shape of the rows that do exist.
livecsv_bad_rows()
{
    local expected
    expected=$(livecsv_header "$1" | awk -F, '{print NF}')
    awk -F, -v numfields="$expected" \
        'NR > 1 && NF != numfields { count++ } END { print count+0 }' "$1" 2>/dev/null
}

########################### Filesystem inspection ###########################

count_files()
{
    find "$1" -type f 2>/dev/null | wc -l | tr -d ' '
}

count_dirs()
{
    find "$1" -mindepth 1 -type d 2>/dev/null | wc -l | tr -d ' '
}

# Space separated sorted list of the distinct file sizes below the given dir.
unique_file_sizes()
{
    find "$1" -type f -printf '%s\n' 2>/dev/null | sort -un | tr '\n' ' ' | sed -e 's/ $//'
}

file_size()
{
    stat -c %s "$1" 2>/dev/null
}

# Allocated disk space of a file in KiB, which is smaller than its apparent
# size for a sparse file. Uses the 512 byte block count of stat, because the
# block size of "du" depends on its environment.
allocated_kb()
{
    echo $(( $(stat -c %b "$1" 2>/dev/null) / 2 ))
}

# Sorted "relative/path size" listing of all files below the given dir.
file_listing()
{
    ( cd "$1" 2>/dev/null && find . -type f -printf '%P %s\n' 2>/dev/null | sort )
}

# A region of a file can only be told apart from an untouched one if the
# untouched state is known, which is what these two are for: prefill a file with
# a byte value that no benchmark ever writes, then count the bytes that are no
# longer that value.

# fill_file_ff FILE SIZE_MIB
# Fill the first SIZE_MIB MiB of a file with 0xFF, creating or extending it.
fill_file_ff()
{
    # "iflag=fullblock" is essential here: reads from a pipe return short, so
    # without it dd would stop long before the requested count.
    tr '\0' '\377' < /dev/zero | \
        dd of="$1" bs=1M count="$2" conv=notrunc iflag=fullblock status=none 2>/dev/null

    return $?
}

# region_is_not_ff FILE OFFSET_MIB LENGTH_MIB
# Echoes the number of bytes in that region that are NOT 0xFF, i.e. 0 means the
# region is still exactly as fill_file_ff left it.
region_is_not_ff()
{
    dd if="$1" bs=1M skip="$2" count="$3" status=none 2>/dev/null | \
        tr -d '\377' | wc -c | tr -d ' '
}

############################ Process inspection #############################
#
# The CPU core and NUMA zone binding options do not appear in any result file,
# so the effective CPU affinity of the process is the only way to see whether
# they had an effect.

# cpus_allowed_list PID -> e.g. "0" or "0-19" or "0,4-7"
cpus_allowed_list()
{
    awk '/^Cpus_allowed_list:/ { print $2 }' "/proc/$1/status" 2>/dev/null
}

# First CPU core this test script itself is allowed to run on. Used instead of
# a hardcoded core 0, so that the binding tests also work when the test runner
# is restricted to a cpuset that does not contain core 0.
first_allowed_cpu()
{
    cpus_allowed_list "$$" | sed -e 's/,.*//' -e 's/-.*//'
}

############################ Capability checks ##############################

# Whether the binary was built with the given optional feature, as reported by
# "--version". For tests that only skip a part of themselves for a feature.
has_build_feature()
{
    local feature="$1"
    local included

    trace_cmd "build feature check" "$ELBENCHO_TEST_BIN" --version

    included=$("$ELBENCHO_TEST_BIN" --version 2>/dev/null | \
        grep 'Included optional build features:')

    case " $included " in
        *" $feature "*) return 0 ;;
    esac

    return 1
}

# Skip the whole test file unless the binary was built with the given optional
# feature.
require_build_feature()
{
    if has_build_feature "$1"; then
        return 0
    fi

    tap_skip_all "elbencho was built without \"$1\" support"
}

require_cmd()
{
    command -v "$1" >/dev/null 2>&1 && return 0
    tap_skip_all "required command not found in PATH: $1"
}

# Some file systems reject O_DIRECT. Probe once so that direct IO subtests can
# be skipped instead of failing.
fs_supports_directio()
{
    local probe="$1/.directio_probe"
    # The output used to be discarded. It goes to a file now, so that the
    # command transcript can show why a direct IO subtest was skipped.
    local out="$1/.directio_probe.out"

    trace_cmd "direct IO probe" \
        "$ELBENCHO_TEST_BIN" -w -s 4k -b 4k --direct \
        --nolive --no0usecerr \
        --resfile /dev/null --csvfile /dev/null --jsonfile /dev/null \
        "$probe"

    timeout 30 "$ELBENCHO_TEST_BIN" -w -s 4k -b 4k --direct \
        --nolive --no0usecerr \
        --resfile /dev/null --csvfile /dev/null --jsonfile /dev/null \
        "$probe" > "$out" 2>&1
    local rc=$?

    rm -f "$probe"

    trace_result "direct IO probe" "$rc" "" "$out"

    rm -f "$out"

    return $rc
}

# Last, so that every function above is defined when the transcript is set up.
trace_init
