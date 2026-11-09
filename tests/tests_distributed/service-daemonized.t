#!/bin/bash
#
# An elbencho service that daemonizes itself.
#
# Without "--foreground" the service forks into the background and the starting
# process returns immediately, which is how the examples in
# tools/test-examples.sh start their services. The service then writes its own
# log file into $TMP, so this test points $TMP at its temporary dir to keep the
# log file there and to get hold of the pid of the daemonized process.
#
# The service gets "--port 0", so the OS picks a free port, and "--hostsfile",
# so it registers its "host:port" in that file once it listens. The port is
# taken from there, as a coordinator would, and the delete phase is run with
# the hosts file itself.
#
# Also covers that a second service cannot take an already used port, and that
# "--quit" reports success even when nothing is listening anymore, which is why
# the pid and not the exit code is what proves that the service really ended.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1

NUM_THREADS=2
NUM_DIRS=2      # per thread
NUM_FILES=4     # per thread and dir
FILE_SIZE=16384

EXPECTED_FILES=$((NUM_THREADS * NUM_DIRS * NUM_FILES))
EXPECTED_BYTES=$((EXPECTED_FILES * FILE_SIZE))

SERVICE_PORT=""
SERVICE_LOG=""
SERVICE_PID=""

test_init
tap_plan 18

DATA_DIR="$TEST_DIR/data"
HOSTS_FILE="$TEST_DIR/hosts"

mkdir -p "$DATA_DIR"

################## Start the daemonized service ##################

# The service log file lands in $TMP, which is pointed at the temporary dir of
# this test, so that it gets cleaned up with everything else.
trace_cmd "daemonized service start" \
    env TMP="$TEST_DIR" "$ELBENCHO_TEST_BIN" --service --port 0 --hostsfile "$HOSTS_FILE"

TMP="$TEST_DIR" "$ELBENCHO_TEST_BIN" --service --port 0 --hostsfile "$HOSTS_FILE" \
    > "$TEST_DIR/start.out" 2>&1
START_RC=$?

# Saved before trace_result, which replaces $?.
trace_result "daemonized service start" "$START_RC" "" "$TEST_DIR/start.out"

if [ $START_RC -ne 0 ]; then
    tap_diag "Output of the failed service start:"
    tap_diag_file "$TEST_DIR/start.out"
    tap_bail "Unable to start a daemonized elbencho service instance."
fi

tap_ok "starting a service without --foreground returns immediately"

# The starting process prints the path of the log file before it forks, so the
# path is taken from there instead of being reconstructed from user and pid.
assert_match "$(cat "$TEST_DIR/start.out")" "Daemonizing into background.*Logfile: $TEST_DIR/" \
    "the service log file was created below the temporary dir given as \$TMP"

SERVICE_LOG=$(sed -ne 's/^.*Logfile: //p' "$TEST_DIR/start.out" | head -1)

# The pid line is written by the forked child, i.e. after the starting process
# has already returned, so it needs to be waited for.
wait_for_file_line "$SERVICE_LOG" 'Running in background\. PID: [0-9]+' 10
if [ $? -eq 0 ]; then
    tap_ok "the service log file holds the pid of the background process"
else
    tap_fail "the service log file holds the pid of the background process"
    tap_diag_file "$SERVICE_LOG"
fi

SERVICE_PID=$(sed -ne 's/^.*Running in background\. PID: //p' "$SERVICE_LOG" | head -1)

trace_add_log "$SERVICE_LOG"

if [ -n "$SERVICE_PID" ]; then
    register_pid "$SERVICE_PID"
fi

kill -0 "$SERVICE_PID" >/dev/null 2>&1
assert_ok $? "the background process with pid $SERVICE_PID is running"

################## The service registers in the hosts file ##################

# The hosts file is written once the service listens, so this is also the
# point where the OS-chosen port becomes known.
wait_for_file_line "$HOSTS_FILE" '^[^:]+:[0-9]+$' 10
if [ $? -eq 0 ]; then
    tap_ok "the service registered a \"host:port\" line in the hosts file"
else
    tap_fail "the service registered a \"host:port\" line in the hosts file"
    tap_diag_file "$SERVICE_LOG"
fi

SERVICE_PORT=$(sed -ne 's/^[^:]*://p' "$HOSTS_FILE" 2>/dev/null | head -1)
if [ -z "$SERVICE_PORT" ]; then
    tap_bail "Unable to learn the service port from the hosts file."
fi

wait_for_port 127.0.0.1 "$SERVICE_PORT" 10
assert_ok $? "the daemonized service is listening on port $SERVICE_PORT from the hosts file"

################## A second service cannot take the same port ##################

trace_cmd "second daemonized service start on the same port" \
    env TMP="$TEST_DIR" "$ELBENCHO_TEST_BIN" --service --port "$SERVICE_PORT"

TMP="$TEST_DIR" "$ELBENCHO_TEST_BIN" --service --port "$SERVICE_PORT" \
    > "$TEST_DIR/start2.out" 2>&1
SECOND_START_RC=$?

# Saved before trace_result, which replaces $?.
trace_result "second daemonized service start" "$SECOND_START_RC" "" "$TEST_DIR/start2.out"

assert_nok $SECOND_START_RC "a second service on the same port does not start"

assert_match "$(cat "$TEST_DIR/start2.out")" \
    "Unable to bind to desired port\. Service already running\? Port: $SERVICE_PORT" \
    "the second service reports that the port is already in use"

################## Run a benchmark through the service ##################

run_elbencho bench \
    --hosts "localhost:$SERVICE_PORT" \
    -d -w \
    -t $NUM_THREADS -n $NUM_DIRS -N $NUM_FILES -s $FILE_SIZE -b $FILE_SIZE \
    --verify 1 --blockvarpct 0 \
    "$DATA_DIR"
assert_ok $? "a benchmark through the daemonized service succeeds"

assert_eq "$(json_config "$ELB_JSON" WRITE hosts)" "1" \
    "write phase result confirms a single service host"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done entries)" "$EXPECTED_FILES" \
    "write phase reports $EXPECTED_FILES written files"
assert_eq "$(json_value "$ELB_JSON" WRITE last_done bytes)" "$EXPECTED_BYTES" \
    "write phase reports $EXPECTED_BYTES written bytes"
assert_eq "$(count_files "$DATA_DIR")" "$EXPECTED_FILES" \
    "$EXPECTED_FILES files exist on the file system"

# The hosts file written by the service, used as the coordinator's hosts file.
run_elbencho delete \
    --hostsfile "$HOSTS_FILE" \
    -F -D --nodelerr \
    -t $NUM_THREADS -n $NUM_DIRS -N $NUM_FILES \
    "$DATA_DIR"
assert_ok $? "the delete phase through the hosts file written by the service succeeds"

################## Terminate the service ##################

run_elbencho quit --hosts "localhost:$SERVICE_PORT" --quit
assert_ok $? "--quit terminates the daemonized service"

wait_for_port_gone 127.0.0.1 "$SERVICE_PORT" 10
assert_ok $? "the service port is not listening anymore"

wait_for_pid_gone "$SERVICE_PID" 10
assert_ok $? "the background process ended"

# An unreachable service is not an error for "--quit", which is why the pid
# check above is what really confirms the termination.
run_elbencho quitagain --hosts "localhost:$SERVICE_PORT" --quit
assert_ok $? "--quit for a port where nothing listens is still reported as success"
