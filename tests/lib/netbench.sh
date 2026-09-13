#!/bin/bash
#
# Helper library for the netbench mode tests.
#
# Source this after tests/lib/testlib.sh and after calling test_init().
#
# In netbench mode both sides of the benchmark are elbencho service instances:
# the coordinator is given a list of servers and a list of clients, and each
# service derives its own role from its position in that combined list. Every
# server listens on its service port for the control channel and additionally on
# "service port + 1000" for the actual data transfer, which is why ports have to
# be allocated in pairs here.

# Offset of the data port from the service port. Not configurable in elbencho,
# this mirrors its NETBENCH_PORT_OFFSET compile time constant.
NETBENCH_PORT_OFFSET=1000

# Service ports are drawn from [BASE, BASE+SPAN), so the matching data ports land
# in [BASE+OFFSET, BASE+OFFSET+SPAN). Two properties make that collision free:
#
# * SPAN is smaller than the offset, so a data port can never be equal to a
#   service port of this or of any other concurrently running netbench test, no
#   matter which ports were drawn.
# * Both ranges stay below the start of the local port range that the kernel
#   uses for outgoing connections (see /proc/sys/net/ipv4/ip_local_port_range,
#   usually 32768). That matters because a data port is not bound while the
#   service is idle, but only for the duration of each run, so it has to stay
#   reservable in between.
NETBENCH_PORT_BASE=20000
NETBENCH_PORT_SPAN=900

# Service ports handed out so far, so that no port is returned twice. The ports
# are not bound yet at allocation time, so this list is the only thing that
# prevents a duplicate.
NETBENCH_TAKEN_PORTS=""

# Service ports and log files of the started services, in start order.
NETBENCH_PORTS=()
NETBENCH_LOGS=()

################################ Port pairs #################################

# netbench_port_is_taken PORT
netbench_port_is_taken()
{
    local port="$1"
    local taken

    for taken in $NETBENCH_TAKEN_PORTS; do
        [ "$port" = "$taken" ] && return 0
    done

    port_in_use "$port" && return 0
    port_in_use "$(( port + NETBENCH_PORT_OFFSET ))" && return 0

    return 1
}

# Echo a service port whose service port and data port are both free, and
# remember it. A port can still be taken between this check and the actual bind,
# so callers retry the whole start sequence on failure.
netbench_alloc_port()
{
    local port
    local tries=0

    while [ $tries -lt 100 ]; do
        tries=$((tries+1))
        port=$(( NETBENCH_PORT_BASE + (RANDOM % NETBENCH_PORT_SPAN) ))

        if ! netbench_port_is_taken "$port"; then
            NETBENCH_TAKEN_PORTS="$NETBENCH_TAKEN_PORTS $port"
            echo "$port"
            return 0
        fi
    done

    return 1
}

# netbench_data_port IDX - the data port of the service at the given index
netbench_data_port()
{
    echo "$(( ${NETBENCH_PORTS[$1]} + NETBENCH_PORT_OFFSET ))"
}

# Check that no other process took one of the data ports in the meantime. This
# is an environment problem rather than an elbencho problem, so callers should
# skip instead of failing.
netbench_data_ports_free()
{
    local port

    for port in "${NETBENCH_PORTS[@]}"; do
        if port_in_use "$(( port + NETBENCH_PORT_OFFSET ))"; then
            tap_diag "Netbench data port $(( port + NETBENCH_PORT_OFFSET )) is used by another process."
            return 1
        fi
    done

    return 0
}

############################ Service instances ##############################

# netbench_start_services NUM
#
# Start NUM elbencho service instances and fill NETBENCH_PORTS / NETBENCH_LOGS.
#
# The services run with verbose logging, because their log is the only place
# that shows which client thread connected to which server, which is what the
# round-robin assignment test needs.
netbench_start_services()
{
    local num="$1"
    local idx=0
    local tries
    local port
    local pid
    local log

    NETBENCH_PORTS=()
    NETBENCH_LOGS=()

    while [ $idx -lt "$num" ]; do
        tries=0
        port=""

        while [ $tries -lt 5 ]; do
            tries=$((tries+1))

            port="$(netbench_alloc_port)"
            if [ -z "$port" ]; then
                tap_diag "No free netbench port pair available."
                return 1
            fi

            log="$TEST_DIR/service$idx.log"

            # "--foreground" keeps the service from daemonizing, so that there
            # is a pid to clean up. Output has to go to a file, because prove
            # reads the test's stdout until end of file.
            "$ELBENCHO_TEST_BIN" --service --foreground --port "$port" --log 1 \
                > "$log" 2>&1 &

            pid=$!
            register_pid "$pid"

            trace_cmd "netbench service $idx start (attempt $tries)" \
                "$ELBENCHO_TEST_BIN" --service --foreground --port "$port" --log 1
            trace_add_log "$log"

            if wait_for_port 127.0.0.1 "$port" 10; then
                break
            fi

            tap_diag "Service $idx did not come up on port $port, retrying..."
            kill -KILL "$pid" > /dev/null 2>&1
            wait "$pid" 2>/dev/null
            port=""
        done

        if [ -z "$port" ]; then
            tap_diag "Unable to start service instance $idx."
            return 1
        fi

        NETBENCH_PORTS+=("$port")
        NETBENCH_LOGS+=("$log")
        idx=$((idx+1))
    done

    return 0
}

# netbench_hosts FIRST_IDX COUNT
#
# Comma separated "localhost:PORT" list of COUNT services, starting at the given
# index of the start order.
#
# The port must always be spelled out: the coordinator forwards the given
# servers string to the services unchanged, and a service that finds no port in
# an entry falls back to elbencho's compiled-in default service port instead of
# the one the coordinator was started with.
netbench_hosts()
{
    local first="$1"
    local count="$2"
    local list=""
    local idx

    for idx in $(seq "$first" $(( first + count - 1 )) ); do
        list="$list${list:+,}localhost:${NETBENCH_PORTS[$idx]}"
    done

    echo "$list"
}

# All started services as a comma separated list.
netbench_all_hosts()
{
    netbench_hosts 0 "${#NETBENCH_PORTS[@]}"
}

# Terminate all started services through a single coordinator run.
#
# This uses "--hosts" and deliberately not "--netbench --servers/--clients",
# because it needs no netbench arguments at all and matches what the distributed
# mode test does.
netbench_quit_services()
{
    local port
    local rc

    run_elbencho quit --hosts "$(netbench_all_hosts)" --quit
    rc=$?

    for port in "${NETBENCH_PORTS[@]}"; do
        wait_for_port_gone 127.0.0.1 "$port" 10 || return 1
    done

    return $rc
}

############################### Service logs ################################
#
# Service logs are appended to across runs and are written by a process that
# keeps them open, so they must not be truncated between runs - that would leave
# padding that makes grep treat them as binary. Instead a mark is taken before a
# run and only the lines after it are inspected.

# netbench_log_mark LOGFILE - line count to pass to netbench_log_count_since
netbench_log_mark()
{
    wc -l < "$1" 2>/dev/null | tr -d ' '
}

# netbench_log_count_since LOGFILE MARK PATTERN
netbench_log_count_since()
{
    tail -n +$(( $2 + 1 )) "$1" 2>/dev/null | grep -ac "$3"
}

# netbench_log_conn_targets_since LOGFILE MARK
#
# Sorted, space separated list of the data ports that this service connected to
# as a netbench client, taken from its "Established connection to: host:port"
# messages.
netbench_log_conn_targets_since()
{
    tail -n +$(( $2 + 1 )) "$1" 2>/dev/null | \
        grep -ao 'Established connection to: [^;]*' | \
        sed -e 's/.*://' | sort -u | tr '\n' ' ' | sed -e 's/ $//'
}

########################### Expected byte totals ############################

# netbench_expected_bytes NUM_CLIENTS THREADS SIZE BLOCKSIZE RESPSIZE
#
# Total number of bytes that a netbench run puts on the wire, counting both
# directions. The coordinator sums the counters of the servers and the clients,
# and since each side counts a different direction, the send and the receive
# total both come out as this same value:
#
#   clients * threads * (size / blocksize) * (blocksize + respsize)
#
# Note that elbencho divides size by blocksize as integers, so the size should
# be an exact multiple of the block size to keep this in sync.
netbench_expected_bytes()
{
    local num_clients="$1"
    local threads="$2"
    local size="$3"
    local blocksize="$4"
    local respsize="$5"

    echo "$(( num_clients * threads * (size / blocksize) * (blocksize + respsize) ))"
}
