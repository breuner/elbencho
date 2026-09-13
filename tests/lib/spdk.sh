#!/bin/bash
#
# Helper library to run a private SPDK NVMe-oF target for a single test script.
# Source this after tests/lib/testlib.sh and after calling test_init().
#
# Every test gets its own nvmf_tgt process with its own RPC socket, its own
# random TCP port and its own backing files below the test's temporary dir, so
# the SPDK tests stay independent and can run in parallel - also alongside an
# unrelated nvmf_tgt that happens to use the SPDK defaults.

: ${ELBENCHO_TEST_NVMF_TGT:="$REPO_DIR/external/spdk/build/bin/nvmf_tgt"}
: ${ELBENCHO_TEST_SPDK_RPC:="$REPO_DIR/external/spdk/scripts/rpc.py"}

# Memory pool sizes in MiB. The floor for both is around 320, because spdk
# reserves about 196 MiB for its iobuf pools at startup. elbencho's own I/O
# buffers come on top of that and need
# "numThreads * iodepth * largest block size" bytes.
SPDK_TGT_MEM_MB=512
SPDK_ELB_MEM_MB=512

# Interrupt mode makes the target wait on epoll instead of busy-polling a CPU
# core. Set this to 0 to fall back to polled mode.
: ${SPDK_TGT_INTERRUPT_MODE:="1"}

# Fixed TCP port for the target's listeners. Empty means "pick a random free
# port and retry on collision", which is what keeps the test suite parallel
# safe. A fixed port turns a collision into a hard error instead, because
# retrying would just hit the same conflict again.
: ${SPDK_TGT_FIXED_PORT:=""}

# Address the target's listeners bind to. The test suite stays on loopback;
# "0.0.0.0" makes the target reachable from other hosts.
: ${SPDK_TGT_LISTEN_ADDR:="127.0.0.1"}

# Address that the generated elbencho config connects to. This has to be
# separate from the listen address, because a wildcard listen address is not a
# usable destination. Empty means "same as the listen address".
: ${SPDK_TGT_CONNECT_ADDR:=""}

# 1 = keep existing backing files, so that the dataset in them survives a
# restart of the target.
: ${SPDK_TGT_PRESERVE_DATA:="0"}

# 1 = add a "log_file" entry to the generated elbencho config. Turn this off for
# a config that gets used in distributed mode: the config content is transferred
# to the service instances, where that path does not necessarily exist.
: ${SPDK_CONF_WITH_LOG_FILE:="1"}

SPDK_NQN1="nqn.2016-06.io.spdk:cnode1"
SPDK_NQN2="nqn.2016-06.io.spdk:cnode2"
SPDK_DISCOVERY_NQN="nqn.2014-08.org.nvmexpress.discovery"

# Model name given to both subsystems, so that the model column of elbencho's
# namespace discovery output is predictable.
SPDK_NS_MODEL="elbencho test ctrl"

# The namespaces this library provisions, indexed 0-3.
#
# Sizes are deliberately >= 128 MiB: elbencho prints namespace sizes with a
# maximum length of 6 characters, so 64 MiB would be shown as "65536K" instead
# of a clean "64M", which makes the expected discovery output awkward.
#
# Namespace UUIDs and target side namespace IDs are pinned, because elbencho
# assigns its own flat numeric namespace IDs in attach order. Tests therefore
# address namespaces by UUID, which is stable, and never by numeric ID.
SPDK_NS_SIZES_MIB=(128 192 256 320)
SPDK_NS_UUIDS=(
    "a0000000-0000-4000-8000-000000000001"
    "a0000000-0000-4000-8000-000000000002"
    "a0000000-0000-4000-8000-000000000003"
    "a0000000-0000-4000-8000-000000000004"
)
# Human-friendly names as built by elbencho from the config below:
# "<subsystem name>:<controller name><N>:ns<target nsid>", where N is a
# per-subsystem counter starting at 0.
SPDK_NS_NAMES=("sysa:ctrl0:ns1" "sysa:ctrl0:ns2" "sysb:ctrl0:ns1" "sysb:ctrl0:ns2")
SPDK_NS_NQNS=("$SPDK_NQN1" "$SPDK_NQN1" "$SPDK_NQN2" "$SPDK_NQN2")
SPDK_NS_NSIDS=(1 2 1 2)

SPDK_SECTOR_SIZE=4096

SPDK_TGT_PID=""
SPDK_TGT_PORT=""
SPDK_CONF=""

# Indexes of backing files that shall be prefilled with 0xFF instead of being
# left sparse, e.g. SPDK_PREFILL_FF_IDXS=(0). Set this before start_nvmf_tgt,
# which applies it whenever it (re-)creates the backing files.
SPDK_PREFILL_FF_IDXS=()

# elbencho arguments to talk to this test's target. Set by write_spdk_conf.
SPDK_OPTS=()

############################ Capability checks ##############################

# Skip the whole test file unless this build can run SPDK tests at all.
require_spdk()
{
    require_build_feature spdk
    require_cmd python3

    if [ ! -x "$ELBENCHO_TEST_NVMF_TGT" ]; then
        tap_skip_all "SPDK target not found at $ELBENCHO_TEST_NVMF_TGT (build elbencho with \"SPDK_SUPPORT=1\")"
    fi

    if [ ! -f "$ELBENCHO_TEST_SPDK_RPC" ]; then
        tap_skip_all "SPDK rpc.py not found at $ELBENCHO_TEST_SPDK_RPC"
    fi
}

# DPDK creates a runtime dir even when it does not use shared memory config.
# Point it into the test dir, because otherwise it lands in /tmp or in the
# user's home. Must be called before the target and before elbencho.
spdk_contain_runtime_dir()
{
    mkdir -p "$TEST_DIR/dpdk-run"

    # RUNTIME_DIRECTORY takes precedence over XDG_RUNTIME_DIR in dpdk, and dpdk
    # creates only the "dpdk" subdir below it, not the parent.
    export RUNTIME_DIRECTORY="$TEST_DIR/dpdk-run"
    export XDG_RUNTIME_DIR="$TEST_DIR/dpdk-run"
}

################################ Naming #####################################

spdk_ns_size_mib() { echo "${SPDK_NS_SIZES_MIB[$1]}"; }
spdk_ns_size()     { echo "$(( ${SPDK_NS_SIZES_MIB[$1]} * 1024 * 1024 ))"; }
spdk_ns_uuid()     { echo "${SPDK_NS_UUIDS[$1]}"; }
spdk_ns_name()     { echo "${SPDK_NS_NAMES[$1]}"; }
spdk_ns_nqn()      { echo "${SPDK_NS_NQNS[$1]}"; }
spdk_ns_nsid()     { echo "${SPDK_NS_NSIDS[$1]}"; }
spdk_bdev_name()   { echo "aio$1"; }
spdk_bdev_file()   { echo "$TEST_DIR/bdev/aio$1.img"; }

############################## Backing files ################################

# Create the sparse backing files for all namespaces.
make_backing_files()
{
    local idx
    local file
    local wanted_bytes
    local actual_bytes

    mkdir -p "$TEST_DIR/bdev"

    for idx in "${!SPDK_NS_SIZES_MIB[@]}"; do
        file="$(spdk_bdev_file "$idx")"

        # When the dataset is to be preserved, an existing file of exactly the
        # right size is left completely alone. A "truncate" to the same size
        # would be a no-op, but a size mismatch would silently cut the file
        # short or extend it with a hole, which would only show up much later as
        # a data verification failure. So say so and start over instead.
        if [ "$SPDK_TGT_PRESERVE_DATA" = "1" ] && [ -f "$file" ]; then
            wanted_bytes=$(( ${SPDK_NS_SIZES_MIB[$idx]} * 1024 * 1024 ))
            actual_bytes="$(stat -c %s "$file" 2>/dev/null)"

            if [ "$actual_bytes" = "$wanted_bytes" ]; then
                continue
            fi

            tap_diag "Backing file has an unexpected size and gets recreated, so its data is lost:"
            tap_diag "  $file ($actual_bytes bytes instead of $wanted_bytes)"
            rm -f "$file"
        fi

        truncate -s "${SPDK_NS_SIZES_MIB[$idx]}M" "$file"
        if [ $? -ne 0 ]; then
            tap_diag "Unable to create backing file: $file"
            return 1
        fi
    done

    return 0
}

# Fill a backing file with 0xFF bytes instead of leaving it sparse/zeroed, so
# that a test can afterwards tell written from untouched regions byte-exactly.
# Must be called before the target is started. The counterpart for reading such
# a region back is region_is_not_ff in lib/testlib.sh.
fill_backing_file_ff()
{
    local idx="$1"

    fill_file_ff "$(spdk_bdev_file "$idx")" "${SPDK_NS_SIZES_MIB[$idx]}"

    return $?
}

############################### Target RPC ##################################

# spdk_rpc ARGS... - one RPC against this test's own target.
# The socket path is relative to the test dir, which keeps it far below the
# 108 byte limit for unix socket paths no matter where the temp dir lives.
spdk_rpc()
{
    trace_cmd "spdk rpc" \
        python3 "$ELBENCHO_TEST_SPDK_RPC" -s ./nvmf.sock -t 30 "$@"

    ( cd "$TEST_DIR" && \
      timeout "$ELBENCHO_TEST_CMD_TIMEOUT" \
        python3 "$ELBENCHO_TEST_SPDK_RPC" -s ./nvmf.sock -t 30 "$@" ) \
        2>> "$TEST_DIR/rpc.log"
}

# Wait until the target answers RPCs and has initialized all its subsystems.
# This replaces a static startup sleep: "-r" retries the connect every 200ms
# and re-checks the socket path each time, so this returns as soon as the
# target is really ready.
spdk_wait_tgt_ready()
{
    trace_cmd "spdk wait for target init" \
        python3 "$ELBENCHO_TEST_SPDK_RPC" -s ./nvmf.sock -r 150 -t 30 framework_wait_init

    ( cd "$TEST_DIR" && \
      timeout 40 python3 "$ELBENCHO_TEST_SPDK_RPC" -s ./nvmf.sock -r 150 -t 30 \
          framework_wait_init ) > /dev/null 2>> "$TEST_DIR/rpc.log"
    if [ $? -ne 0 ]; then
        return 1
    fi

    # guard against a target that died during initialization
    kill -0 "$SPDK_TGT_PID" 2>/dev/null
}

############################# Target lifecycle ##############################

spdk_launch_tgt()
{
    local interrupt_opt=""

    [ "$SPDK_TGT_INTERRUPT_MODE" != "0" ] && interrupt_opt="--interrupt-mode"

    # Run in a subshell with the test dir as working dir, so that the RPC
    # socket path can stay relative and "$!" is still the pid of the target
    # itself (which the target loss tests need).
    #
    # Flags that matter for running multiple targets in parallel:
    # * "-r ./nvmf.sock": the default /var/tmp/spdk.sock would be shared
    # * "--disable-cpumask-locks": the default takes an exclusive lock file per
    #   cpumask core in /var/tmp, which two targets would fight over
    # * "--num-trace-entries 0": skips the trace file in /dev/shm, which would
    #   otherwise be left behind when a test kills its target
    # * no "--shm-id": the default of -1 is what makes dpdk use a per-pid file
    #   prefix and no shared config, so passing one would *reduce* isolation
    ( cd "$TEST_DIR" && exec "$ELBENCHO_TEST_NVMF_TGT" \
        -r ./nvmf.sock \
        -m 0x1 \
        -s $SPDK_TGT_MEM_MB \
        --no-huge \
        --no-pci \
        --disable-cpumask-locks \
        --num-trace-entries 0 \
        --base-virtaddr 0x1000000000 \
        $interrupt_opt \
    ) >> "$TEST_DIR/nvmf_tgt.log" 2>&1 &

    SPDK_TGT_PID=$!
    register_pid "$SPDK_TGT_PID"

    # $interrupt_opt is unquoted here just like in the launch above, because
    # quoted it would show up as a stray '' when it is empty.
    trace_cmd "SPDK target start" \
        "$ELBENCHO_TEST_NVMF_TGT" -r ./nvmf.sock -m 0x1 -s $SPDK_TGT_MEM_MB \
        --no-huge --no-pci --disable-cpumask-locks --num-trace-entries 0 \
        --base-virtaddr 0x1000000000 $interrupt_opt
    trace_add_log "$TEST_DIR/nvmf_tgt.log"
    trace_add_log "$TEST_DIR/rpc.log"
}

# Register the backing files as aio bdevs, create the subsystems, add the
# namespaces and start listening. Returns non-zero if any step failed.
spdk_provision_tgt()
{
    local idx

    for idx in "${!SPDK_NS_SIZES_MIB[@]}"; do
        spdk_rpc bdev_aio_create "$(spdk_bdev_file "$idx")" "$(spdk_bdev_name "$idx")" \
            $SPDK_SECTOR_SIZE -u "$(spdk_ns_uuid "$idx")" > /dev/null || return 1
    done

    spdk_rpc nvmf_create_transport -t TCP > /dev/null || return 1

    spdk_rpc nvmf_create_subsystem "$SPDK_NQN1" -a -s SPDKTEST01 \
        -d "$SPDK_NS_MODEL" > /dev/null || return 1
    spdk_rpc nvmf_create_subsystem "$SPDK_NQN2" -a -s SPDKTEST02 \
        -d "$SPDK_NS_MODEL" > /dev/null || return 1

    for idx in "${!SPDK_NS_SIZES_MIB[@]}"; do
        spdk_rpc nvmf_subsystem_add_ns "$(spdk_ns_nqn "$idx")" \
            "$(spdk_bdev_name "$idx")" -n "$(spdk_ns_nsid "$idx")" > /dev/null || return 1
    done

    for nqn in "$SPDK_NQN1" "$SPDK_NQN2" "$SPDK_DISCOVERY_NQN"; do
        spdk_rpc nvmf_subsystem_add_listener "$nqn" -t TCP -a "$SPDK_TGT_LISTEN_ADDR" \
            -s "$SPDK_TGT_PORT" > /dev/null || return 1
    done

    return 0
}

# Start a private NVMe-oF target and provision it. The chosen TCP port can be
# taken between the free port check and the actual bind, in which case adding
# the listener fails loudly, so the whole sequence is retried with a fresh
# process.
start_nvmf_tgt()
{
    local tries=0
    local maxtries=5
    local prefill_idx

    spdk_contain_runtime_dir

    # Retrying only makes sense for a randomly picked port. With an explicitly
    # requested port a second attempt would run into the same conflict.
    [ -n "$SPDK_TGT_FIXED_PORT" ] && maxtries=1

    while [ $tries -lt $maxtries ]; do
        tries=$((tries+1))

        if [ -n "$SPDK_TGT_FIXED_PORT" ]; then
            SPDK_TGT_PORT="$SPDK_TGT_FIXED_PORT"

            if port_in_use "$SPDK_TGT_PORT"; then
                tap_diag "TCP port $SPDK_TGT_PORT is already in use, maybe by a leftover target."
                tap_diag "Check with:  ss -ltnp | grep ':$SPDK_TGT_PORT'"
                return 1
            fi
        else
            SPDK_TGT_PORT=$(find_free_port)
            if [ -z "$SPDK_TGT_PORT" ]; then
                tap_diag "Unable to find a free TCP port for the SPDK target."
                return 1
            fi
        fi

        # The RPC socket is always recreated, because a stale one from a killed
        # target or from before a reboot would make rpc.py talk to nothing. Note
        # that spdk itself takes a lock on the ".lock" file and unlinks a stale
        # socket, so the two files are only removed when the dataset is not to
        # be preserved anyway - keeping them lets spdk detect a second target
        # that tries to use the same backing files.
        if [ "$SPDK_TGT_PRESERVE_DATA" != "1" ]; then
            rm -rf "$TEST_DIR/bdev" "$TEST_DIR/nvmf.sock" "$TEST_DIR/nvmf.sock.lock"
        fi

        make_backing_files || return 1

        # Must happen before the target opens the files, and never when the
        # dataset is to be preserved, because it would destroy exactly the data
        # that is meant to be kept.
        if [ "$SPDK_TGT_PRESERVE_DATA" != "1" ]; then
            for prefill_idx in "${SPDK_PREFILL_FF_IDXS[@]}"; do
                fill_backing_file_ff "$prefill_idx"
                if [ $? -ne 0 ]; then
                    tap_diag "Unable to prefill backing file $prefill_idx."
                    return 1
                fi
            done
        fi

        spdk_launch_tgt

        if spdk_wait_tgt_ready && spdk_provision_tgt; then
            write_spdk_conf
            return 0
        fi

        tap_diag "SPDK target setup failed on port $SPDK_TGT_PORT, retrying..."
        kill -KILL "$SPDK_TGT_PID" > /dev/null 2>&1
        wait "$SPDK_TGT_PID" 2>/dev/null
        SPDK_TGT_PID=""
    done

    tap_diag "Giving up on starting an SPDK NVMe-oF target. Target log:"
    tap_diag_file "$TEST_DIR/nvmf_tgt.log"
    tap_diag "RPC log:"
    tap_diag_file "$TEST_DIR/rpc.log"

    return 1
}

# Terminate the target cleanly and wait for it to be gone.
stop_nvmf_tgt()
{
    local waited=0

    [ -z "$SPDK_TGT_PID" ] && return 0

    kill -TERM "$SPDK_TGT_PID" > /dev/null 2>&1

    while [ $waited -lt 100 ]; do
        kill -0 "$SPDK_TGT_PID" 2>/dev/null || break
        sleep 0.1
        waited=$((waited+1))
    done

    kill -KILL "$SPDK_TGT_PID" > /dev/null 2>&1
    wait "$SPDK_TGT_PID" 2>/dev/null

    SPDK_TGT_PID=""

    return 0
}

# Make the target disappear abruptly, as in a crash or a cable pull.
kill_nvmf_tgt()
{
    [ -z "$SPDK_TGT_PID" ] && return 0

    kill -KILL "$SPDK_TGT_PID" > /dev/null 2>&1
    wait "$SPDK_TGT_PID" 2>/dev/null

    SPDK_TGT_PID=""

    return 0
}

spdk_tgt_alive()
{
    [ -n "$SPDK_TGT_PID" ] && kill -0 "$SPDK_TGT_PID" 2>/dev/null
}

########################## elbencho SPDK config #############################

# Write the SPDK config for this test and set SPDK_OPTS accordingly.
#
# Both subsystems use the same controller name on purpose: elbencho numbers the
# controller per subsystem, so this yields the predictable namespace names
# "sysa:ctrl0:ns*" and "sysb:ctrl0:ns*".
#
# "interrupt_mode" is deliberately not enabled here: spdk's bdev_nvme module
# refuses to create bdevs for non-PCIe transports while it is on, and these
# tests always use TCP. "host_nqn" is left unset so that spdk's own default
# applies, and no CPU cores are pinned so that parallel tests can spread out.
write_spdk_conf()
{
    local traddr="${SPDK_TGT_CONNECT_ADDR:-$SPDK_TGT_LISTEN_ADDR}"
    local log_file_line=""

    if [ "$SPDK_CONF_WITH_LOG_FILE" = "1" ]; then
        log_file_line="    \"log_file\": \"$TEST_DIR/elbencho_spdk.log\","
    fi

    SPDK_CONF="$TEST_DIR/spdk.json"

    cat > "$SPDK_CONF" <<CONF_EOF
{
    "io_threads": 1,
    "mem_size_mb": $SPDK_ELB_MEM_MB,
$log_file_line
    "subsystems": [
        {
            "name": "sysa",
            "controller_name": "ctrl",
            "nqn": "$SPDK_NQN1",
            "traddr": "$traddr",
            "trsvcid": "$SPDK_TGT_PORT",
            "trtype": "tcp"
        },
        {
            "name": "sysb",
            "controller_name": "ctrl",
            "nqn": "$SPDK_NQN2",
            "traddr": "$traddr",
            "trsvcid": "$SPDK_TGT_PORT",
            "trtype": "tcp"
        }
    ]
}
CONF_EOF

    SPDK_OPTS=( --spdkconffile "$SPDK_CONF" )
}

######################### Target side inspection ############################
#
# These give a view of the target that is completely independent of what
# elbencho reports, which is what makes them useful as cross-checks.

# Sorted list of the NQNs of all real (non-discovery) subsystems.
spdk_tgt_subsystem_nqns()
{
    spdk_rpc nvmf_get_subsystems | \
        jq -r '.[] | select(.subtype == "NVMe") | .nqn' | sort | tr '\n' ' ' | sed -e 's/ $//'
}

# Sorted "<nqn>|<nsid>|<uuid>|<bdev>" lines for all provisioned namespaces.
spdk_tgt_ns_triples()
{
    spdk_rpc nvmf_get_subsystems | \
        jq -r '.[] | select(.subtype == "NVMe") | .nqn as $nqn |
               .namespaces[] | "\($nqn)|\(.nsid)|\(.uuid)|\(.bdev_name)"' | \
        sort | tr '\n' ' ' | sed -e 's/ $//'
}

# Number of namespaces of the given subsystem.
spdk_tgt_ns_count()
{
    spdk_rpc nvmf_get_subsystems | \
        jq -r --arg nqn "$1" '[ .[] | select(.nqn == $nqn) | .namespaces[] ] | length'
}

# Sorted "<name>|<block_size>|<num_blocks>" lines for all aio bdevs.
spdk_tgt_bdev_info()
{
    spdk_rpc bdev_get_bdevs | \
        jq -r '.[] | "\(.name)|\(.block_size)|\(.num_blocks)"' | \
        sort | tr '\n' ' ' | sed -e 's/ $//'
}

# spdk_tgt_iostat BDEV FIELD, e.g. bytes_written, num_write_ops, bytes_read.
spdk_tgt_iostat()
{
    spdk_rpc bdev_get_iostat --name "$1" | \
        jq -r --arg f "$2" '.bdevs[0][$f] // 0'
}

# spdk_wait_for_target_io BDEV SECONDS
# Wait until the target has actually seen I/O on the given bdev. Used instead
# of a sleep to make sure that a test breaks the target while I/O is really in
# flight, and not during startup.
spdk_wait_for_target_io()
{
    local bdev="$1"
    local maxloops=$(( $2 * 5 ))
    local waited=0
    local numops
    # This polls an rpc five times per second, so tracing every single call
    # would bury everything else in the transcript.
    local TRACE_QUIET=1

    while [ $waited -lt $maxloops ]; do
        numops=$(spdk_rpc bdev_get_iostat --name "$bdev" | \
            jq -r '[ .bdevs[0].num_read_ops, .bdevs[0].num_write_ops ] | add // 0')

        if [ -n "$numops" ] && [ "$numops" -gt 0 ] 2>/dev/null; then
            return 0
        fi

        spdk_tgt_alive || return 1

        sleep 0.2
        waited=$((waited+1))
    done

    return 1
}

# spdk_remove_ns NQN NSID - remove a namespace from a running target.
spdk_remove_ns()
{
    spdk_rpc nvmf_subsystem_remove_ns "$1" "$2" > /dev/null
}

###################### Discovery output inspection ##########################
#
# elbencho prints its namespace discovery result to stdout as
#   SPDK NVMe-oF discovery result... (Namespace; Numeric ID; Size; Format; Model; UUID/NGUID)
#   * <name>; <numeric id>; <size>; <sector size>B; <model>; <uuid or nguid>

SPDK_DISCOVERY_HEADER='SPDK NVMe-oF discovery result... (Namespace; Numeric ID; Size; Format; Model; UUID/NGUID)'

# Sorted, space separated list of the namespace names in a captured output.
spdk_listed_names()
{
    grep -oE '^\* [^;]+' "$1" 2>/dev/null | sed -e 's/^\* //' | \
        sort | tr '\n' ' ' | sed -e 's/ $//'
}

# Number of listed namespaces.
spdk_listed_count()
{
    grep -cE '^\* ' "$1" 2>/dev/null | tr -d ' '
}

# spdk_listed_field FILE NAME FIELDNUM - one semicolon separated field of the
# listing line of the given namespace. Field 1 is the name (including the
# leading "* "), 2 the numeric ID, 3 the size, 4 the format, 5 the model and
# 6 the UUID/NGUID.
spdk_listed_field()
{
    awk -F'; ' -v name="* $2" -v num="$3" \
        '$1 == name { gsub(/^\* /, "", $1); print $num; exit }' "$1" 2>/dev/null
}

# Sorted, space separated list of the numeric namespace IDs in the listing.
spdk_listed_nsids()
{
    awk -F'; ' '/^\* / { print $2 }' "$1" 2>/dev/null | \
        sort -n | tr '\n' ' ' | sed -e 's/ $//'
}

# Sorted, space separated list of the reported namespace sizes.
spdk_listed_sizes()
{
    awk -F'; ' '/^\* / { print $3 }' "$1" 2>/dev/null | \
        sort | tr '\n' ' ' | sed -e 's/ $//'
}

# The numeric ID that elbencho assigned to the given namespace name.
spdk_listed_nsid()
{
    spdk_listed_field "$1" "$2" 2
}

########################### SPDK opslog helpers #############################
#
# In SPDK mode the operations log uses "spdkRead"/"spdkWrite" as operation
# names and the human-friendly namespace name as entry name. Each operation is
# logged twice, before ("is_finished": false) and after ("is_finished": true),
# so the helpers below count the "before" lines to get one entry per operation.
#
# The generic ones that only rely on that double logging - opslog_min_offset,
# opslog_max_end, opslog_bytes_sum - live in lib/testlib.sh, because the file
# mode tests need them too.

# Sorted, space separated list of the entry names in the operations log.
opslog_entry_names()
{
    jq -rs '[ .[] | .entry_name ] | unique | join(" ")' "$1" 2>/dev/null
}

# Sorted, space separated list of the distinct block sizes in the log.
opslog_distinct_lengths()
{
    jq -rs '[ .[] | select(.is_finished == false) | .length ] | unique | sort | join(" ")' \
        "$1" 2>/dev/null
}

# opslog_length_count OPSLOGFILE LENGTH - number of operations of that size
opslog_length_count()
{
    jq -s --argjson len "$2" \
        '[ .[] | select(.is_finished == false and .length == $len) ] | length' "$1" 2>/dev/null
}

# opslog_count_oversized OPSLOGFILE MAXLEN - operations larger than MAXLEN
opslog_count_oversized()
{
    jq -s --argjson max "$2" \
        '[ .[] | select(.is_finished == false and .length > $max) ] | length' "$1" 2>/dev/null
}

# opslog_nonaligned_count OPSLOGFILE ALIGNMENT - operations whose offset or
# length is not an even multiple of the given alignment
opslog_nonaligned_count()
{
    jq -s --argjson al "$2" \
        '[ .[] | select(.is_finished == false)
           | select( ( (.offset % $al) != 0) or ( (.length % $al) != 0) ) ] | length' \
        "$1" 2>/dev/null
}

# opslog_ranks_for_op OPSLOGFILE OP_NAME - sorted worker ranks as csv
opslog_ranks_for_op()
{
    jq -rs --arg op "$2" \
        '[ .[] | select(.op_name == $op) | .worker_rank ] | unique | sort | join(",")' \
        "$1" 2>/dev/null
}

# opslog_num_distinct_ranks OPSLOGFILE
opslog_num_distinct_ranks()
{
    jq -s '[ .[] | .worker_rank ] | unique | length' "$1" 2>/dev/null
}

# opslog_ranks_overlap_in_time OPSLOGFILE SPLIT_RANK
# Echoes 1 if the operations of the worker ranks below SPLIT_RANK and those of
# the ranks from SPLIT_RANK upwards overlap in time, i.e. both groups were
# really active at the same time. The timestamps use a fixed format and the
# same time zone in all processes, so comparing them as strings is valid.
opslog_ranks_overlap_in_time()
{
    jq -s --argjson split "$2" '
        ( [ .[] | select(.worker_rank <  $split) | .date ] ) as $a |
        ( [ .[] | select(.worker_rank >= $split) | .date ] ) as $b |
        if ($a | length) == 0 or ($b | length) == 0 then 0
        elif ( ($a | min) <= ($b | max) ) and ( ($b | min) <= ($a | max) ) then 1
        else 0 end' "$1" 2>/dev/null
}

##################### Losing the target during a run #######################
#
# These are for the tests that break the target while elbencho is running.
#
# elbencho handles SIGINT and SIGTERM cleanly, i.e. terminating it that way
# looks exactly like a graceful shutdown. So these tests must not run elbencho
# under a timeout: the only way to tell "elbencho noticed the broken target and
# exited" apart from "elbencho hung and the test gave up" is to never signal it
# and see whether it exits by itself.

# elbencho writes a backtrace to this fixed path if it hits a fault. It cannot
# be redirected into the test dir, so it is only used for diagnostics and never
# asserted on - it is shared between all users and appended to.
SPDK_FAULT_TRACE_PATH="/tmp/elbencho_fault_trace.txt"

ELB_BG_PID=""
ELB_BG_OUT=""
ELB_BG_RC=""
ELB_BG_SELF_EXITED=0
ELB_BG_ELAPSED=0
SPDK_FAULT_TRACE_SIZE_BEFORE=0

spdk_fault_trace_size()
{
    if [ -f "$SPDK_FAULT_TRACE_PATH" ]; then
        stat -c %s "$SPDK_FAULT_TRACE_PATH" 2>/dev/null
    else
        echo "0"
    fi
}

# spdk_start_background_elbencho TAG ARGS...
# Start elbencho in the background, deliberately without a timeout wrapper.
spdk_start_background_elbencho()
{
    local tag="$1"
    shift

    SPDK_FAULT_TRACE_SIZE_BEFORE="$(spdk_fault_trace_size)"

    ELB_BG_OUT="$TEST_DIR/$tag.out"
    : > "$ELB_BG_OUT"
    ELB_BG_RC=""
    ELB_BG_SELF_EXITED=0
    ELB_BG_ELAPSED=0

    "$ELBENCHO_TEST_BIN" \
        --nolive --no0usecerr \
        --resfile /dev/null --csvfile /dev/null --jsonfile /dev/null \
        "$@" > "$ELB_BG_OUT" 2>&1 &

    ELB_BG_PID=$!
    register_pid "$ELB_BG_PID"

    trace_cmd "background elbencho \"$tag\" (deliberately without a timeout)" \
        "$ELBENCHO_TEST_BIN" \
        --nolive --no0usecerr \
        --resfile /dev/null --csvfile /dev/null --jsonfile /dev/null \
        "$@"
}

# spdk_wait_for_own_exit SECONDS
# Wait for the background elbencho to terminate on its own. Sets
# ELB_BG_SELF_EXITED, ELB_BG_RC and ELB_BG_ELAPSED.
spdk_wait_for_own_exit()
{
    local maxloops=$(( $1 * 5 ))
    local waited=0
    local start=$SECONDS

    while [ $waited -lt $maxloops ]; do
        if ! kill -0 "$ELB_BG_PID" 2>/dev/null; then
            ELB_BG_SELF_EXITED=1
            break
        fi
        sleep 0.2
        waited=$((waited+1))
    done

    ELB_BG_ELAPSED=$((SECONDS - start))

    if [ "$ELB_BG_SELF_EXITED" -eq 1 ]; then
        wait "$ELB_BG_PID"
        ELB_BG_RC=$?
    fi

    trace_result "background elbencho" "${ELB_BG_RC:-still running}" \
        "$(( ELB_BG_ELAPSED * 1000 ))" "$ELB_BG_OUT"
}

# Terminate a background elbencho that did not exit by itself, after dumping
# where it is stuck. Only reached when the test already failed.
spdk_kill_background_elbencho()
{
    local waited=0

    kill -0 "$ELB_BG_PID" 2>/dev/null || return 0

    if command -v gdb > /dev/null 2>&1; then
        tap_diag "elbencho is stuck, thread backtraces:"
        gdb -p "$ELB_BG_PID" -batch -ex "thread apply all bt" 2>&1 | \
            head -60 | sed -e 's/^/#   /'
    else
        tap_diag "elbencho is stuck (install gdb to get a backtrace here)"
    fi

    kill -TERM "$ELB_BG_PID" > /dev/null 2>&1

    while [ $waited -lt 50 ]; do
        kill -0 "$ELB_BG_PID" 2>/dev/null || break
        sleep 0.1
        waited=$((waited+1))
    done

    kill -KILL "$ELB_BG_PID" > /dev/null 2>&1
    wait "$ELB_BG_PID" 2>/dev/null

    ELB_BG_PID=""
}

# spdk_assert_clean_termination WHAT MAX_SECONDS
# The full set of checks for a run whose target was broken: elbencho must have
# terminated by itself, with a failure exit code that is not a signal death, an
# error message on its output, and no backtrace or unhandled exception.
# Emits exactly 5 TAP results in every case.
spdk_assert_clean_termination()
{
    local what="$1"
    local maxsecs="$2"
    local output

    if [ "$ELB_BG_SELF_EXITED" -ne 1 ]; then
        tap_fail "elbencho terminates by itself after $what"
        tap_diag "  it was still running ${ELB_BG_ELAPSED}s after the target was broken"
        tap_diag "  output so far:"
        tap_diag_file "$ELB_BG_OUT"
        spdk_kill_background_elbencho

        tap_fail "elbencho reports a failure exit code after $what"
        tap_fail "elbencho reports an error message after $what"
        tap_fail "elbencho does not generate a backtrace after $what"
        tap_fail "elbencho terminates promptly after $what"
        return
    fi

    tap_ok "elbencho terminates by itself after $what (${ELB_BG_ELAPSED}s)"

    output="$(cat "$ELB_BG_OUT")"

    # An exit code of 1 is the regular failure exit. Anything of 128 or above
    # means the process died from a signal, i.e. it crashed.
    assert_eq "$ELB_BG_RC" "1" \
        "elbencho reports a failure exit code after $what"

    assert_match "$output" \
        '\[SPDK\] I/O failed|IO failed:|SPDK IO submission failed|File read failed|File write failed' \
        "elbencho reports an error message after $what"

    if echo "$output" | grep -qE 'Trying to generate a backtrace|Saved backtrace:|terminate called|Segmentation fault|Aborted'; then
        tap_fail "elbencho does not generate a backtrace after $what"
        tap_diag "  output:"
        tap_diag_file "$ELB_BG_OUT"
    else
        tap_ok "elbencho does not generate a backtrace after $what"
    fi

    if [ "$ELB_BG_ELAPSED" -le "$maxsecs" ]; then
        tap_ok "elbencho terminates within ${maxsecs}s after $what"
    else
        tap_fail "elbencho terminates within ${maxsecs}s after $what"
        tap_diag "  it took ${ELB_BG_ELAPSED}s"
    fi

    tap_diag "reported error: $(echo "$output" | grep -iE 'error|failed' | head -1 | cut -c1-100)"
    tap_diag "fault trace file size before/after: $SPDK_FAULT_TRACE_SIZE_BEFORE/$(spdk_fault_trace_size)"
}
