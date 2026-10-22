#!/bin/bash
#
# Helper library for the netbench mode tests.
#
# Source this after tests/lib/testlib.sh and after calling test_init().
#
# In netbench mode both sides of the benchmark are elbencho service instances:
# the coordinator is given a list of servers and a list of clients, and each
# service gets its role from its position in that combined list. The data
# endpoints of the servers use ports chosen by the OS, which the clients learn
# through the service HTTP API, so only the service ports are allocated here.

# Service ports and log files of the started services, in start order.
NETBENCH_PORTS=()
NETBENCH_LOGS=()

# Phase name of a netbench run in the results: "NET/" plus the engine name. Tests
# that run another engine than the default set this before inspecting a result.
NET_PHASE="NET/tcp"

# netbench_start_service [SERVICE_ARGS...]
#
# Start one more elbencho service instance on a free port and append it to
# NETBENCH_PORTS / NETBENCH_LOGS. Extra arguments go to the service, e.g.
# "--netdevs" or "--netport".
#
# The services run with verbose logging, because their log is the only place
# that shows which client thread connected to which server endpoint.
netbench_start_service()
{
    local idx=${#NETBENCH_PORTS[@]}
    local tries=0
    local port
    local pid
    local log

    while [ $tries -lt 5 ]; do
        tries=$((tries+1))

        port="$(find_free_port)" || return 1
        log="$TEST_DIR/service$idx.log"

        # "--foreground" keeps the service from daemonizing, so that there
        # is a pid to clean up. Output has to go to a file, because prove
        # reads the test's stdout until end of file.
        "$ELBENCHO_TEST_BIN" --service --foreground --port "$port" --log 1 "$@" \
            > "$log" 2>&1 &

        pid=$!
        register_pid "$pid"

        trace_cmd "netbench service $idx start (attempt $tries)" \
            "$ELBENCHO_TEST_BIN" --service --foreground --port "$port" --log 1 "$@"
        trace_add_log "$log"

        if wait_for_port 127.0.0.1 "$port" 10; then
            NETBENCH_PORTS+=("$port")
            NETBENCH_LOGS+=("$log")
            return 0
        fi

        tap_diag "Service $idx did not come up on port $port, retrying..."
        kill -KILL "$pid" > /dev/null 2>&1
        wait "$pid" 2>/dev/null
    done

    tap_diag "Unable to start service instance $idx."
    return 1
}

# netbench_start_services NUM
#
# Start NUM service instances without extra arguments.
netbench_start_services()
{
    local num="$1"
    local idx

    NETBENCH_PORTS=()
    NETBENCH_LOGS=()

    for idx in $(seq 1 "$num"); do
        netbench_start_service || return 1
    done

    return 0
}

# netbench_hosts FIRST_IDX COUNT
#
# Comma separated "localhost:PORT" list of COUNT services, starting at the given
# index of the start order.
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

################## Service log inspection ##################

# Service logs are appended to across runs and are written by a process that
# keeps them open, so they must not be truncated between runs - that would leave
# padding that makes grep treat them as binary. Instead a mark is taken before a
# run and only the lines after it are inspected.

# netbench_log_mark LOGFILE - line count to pass to the netbench_log_*_since functions
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
# Sorted, space separated list of the server service addresses ("host:port")
# that this service connected to as a netbench client, taken from its
# "Established connection to: host:port; ..." messages.
netbench_log_conn_targets_since()
{
    tail -n +$(( $2 + 1 )) "$1" 2>/dev/null | \
        grep -ao 'Established connection to: [^;]*' | \
        sed -e 's/.*to: //' | sort -u | tr '\n' ' ' | sed -e 's/ $//'
}

# netbench_log_endpoints_since LOGFILE MARK
#
# Sorted, space separated list of the server data endpoint addresses that this
# client connected to. The "Established connection" message describes the
# endpoint as "endpoint: PROVIDER PROTOCOL: ADDRESS (DOMAIN)", where the
# provider name may itself contain a semicolon ("tcp;ofi_rxm").
netbench_log_endpoints_since()
{
    tail -n +$(( $2 + 1 )) "$1" 2>/dev/null | \
        sed -n -e 's/.*; endpoint: \(.*\); via: .*/\1/p' | \
        sed -e 's/.*: //' -e 's/ (.*//' | sort -u | tr '\n' ' ' | sed -e 's/ $//'
}

# netbench_log_endpoint_ports_since LOGFILE MARK
#
# Like netbench_log_endpoints_since, but only the port numbers, sorted
# numerically.
netbench_log_endpoint_ports_since()
{
    local addr

    for addr in $(netbench_log_endpoints_since "$1" "$2"); do
        echo "${addr##*:}"
    done | sort -n | tr '\n' ' ' | sed -e 's/ $//'
}

# netbench_log_sources_since LOGFILE MARK
#
# Like netbench_log_endpoints_since, but the client's own endpoint addresses
# ("via: PROVIDER PROTOCOL: ADDRESS (DOMAIN)"), i.e. the interfaces it used.
netbench_log_sources_since()
{
    tail -n +$(( $2 + 1 )) "$1" 2>/dev/null | \
        sed -n -e 's/.*; via: \(.*\); WorkerRank: .*/\1/p' | \
        sed -e 's/.*: //' -e 's/ (.*//' | sort -u | tr '\n' ' ' | sed -e 's/ $//'
}

# netbench_log_conn_pairs_since LOGFILE MARK
#
# One line per established connection of this client, "ENDPOINT_IP OWN_IP"
# (the server endpoint address and the client's own address, without ports),
# sorted. Shows which interface pairs the streams used.
netbench_log_conn_pairs_since()
{
    local line endpoint via

    tail -n +$(( $2 + 1 )) "$1" 2>/dev/null | grep -a "; endpoint: " | while read -r line; do
        endpoint="${line#*; endpoint: }"; endpoint="${endpoint%%; via: *}"
        endpoint="${endpoint##*: }"; endpoint="${endpoint%% (*}"
        via="${line#*; via: }"; via="${via%%; WorkerRank: *}"
        via="${via##*: }"; via="${via%% (*}"
        echo "$(netbench_ip_part "$endpoint") $(netbench_ip_part "$via")"
    done | sort
}

# netbench_ip_part ADDRESS
#
# The IP of an "ip:port" or "[v6]:port" endpoint address.
netbench_ip_part()
{
    echo "$1" | sed -e 's/:[0-9]*$//' -e 's/^\[//' -e 's/\]$//'
}

################## Expected transfer sizes ##################

# netbench_expected_send_bytes NUM_CLIENTS THREADS SIZE SENDSIZE RECVSIZE
# netbench_expected_recv_bytes NUM_CLIENTS THREADS SIZE SENDSIZE RECVSIZE
#
# Bytes that the clients of a netbench run send ("bytes" in the json result)
# or receive ("rwmix_read" "bytes"). The coordinator counts the clients only,
# because the servers see the same bytes from the other side:
#
#   clients * threads * rounds * sendsize      (send)
#   clients * threads * rounds * recvsize      (recv)
#
# where rounds is the size divided by the send size, or by the receive size for
# a receive-only stream (send size 0). elbencho divides as integers, so keep the
# size a multiple of the round size.
netbench_expected_send_bytes()
{
    netbench_expected_bytes_impl "$1" "$2" "$3" "$4" "$5" "$4"
}

netbench_expected_recv_bytes()
{
    netbench_expected_bytes_impl "$1" "$2" "$3" "$4" "$5" "$5"
}

netbench_expected_bytes_impl()
{
    local num_clients="$1"
    local threads="$2"
    local size="$3"
    local sendsize="$4"
    local recvsize="$5"
    local direction_size="$6"
    local unit="$sendsize"

    [ "$unit" -eq 0 ] && unit="$recvsize"

    echo "$(( num_clients * threads * (size / unit) * direction_size ))"
}
