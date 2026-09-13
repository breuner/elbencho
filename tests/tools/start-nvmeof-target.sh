#!/bin/bash
#
# Start an SPDK NVMe-oF target for interactive elbencho testing and keep it
# running until ctrl+c. The data is kept, so the same dataset can be used again
# after a restart of the target or of the whole host.
#
# This reuses the same target setup as the automated SPDK tests, see
# tests/lib/spdk.sh, so it needs elbencho to be built with "SPDK_SUPPORT=1".

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/spdk.sh" || exit 1
source "$ELBENCHO_TEST_LIB/interactive.sh" || exit 1

DEFAULT_PORT=4420
DEFAULT_NS_SIZE_MIB=1024
DATA_DIR_NAME="tools-nvmeof-target"

LISTEN_ADDR=""              # user-definable via "-a"; empty means auto-detect
CONNECT_ADDR=""             # user-definable via "-c"
TGT_PORT=$DEFAULT_PORT      # user-definable via "-p"
NS_SIZES="$DEFAULT_NS_SIZE_MIB" # user-definable via "-s"
TGT_MEM_MB=""               # user-definable via "-m"

usage()
{
    echo "About:"
    echo "  Start an SPDK NVMe-oF target for interactive elbencho testing and keep"
    echo "  it running until ctrl+c. It provides 2 subsystems with 2 namespaces"
    echo "  each, backed by sparse files."
    echo
    echo "  The data is kept when the target is stopped, so the same dataset can be"
    echo "  used again the next day. Use \"-C\" to start with empty namespaces."
    echo
    echo "Usage:"
    echo "  $ $(basename "$0") [OPTIONS]"
    echo
    echo "Optional Arguments:"
    echo "  -a ADDR   Address to listen on. This has to be a concrete address, because"
    echo "            NVMe-oF only accepts clients that connect to an address which is"
    echo "            registered as a listener of the subsystem, so a wildcard address"
    echo "            would reject everyone. Use \"-a 127.0.0.1\" to keep the target"
    echo "            local. (Default: the auto-detected address of this host, so that"
    echo "            it is reachable from other hosts)"
    echo "  -c ADDR   Address that clients connect to, as written into the generated"
    echo "            elbencho config, for the case that clients reach this host under"
    echo "            a different address or name. (Default: same as \"-a\")"
    echo "  -p PORT   TCP port to listen on. (Default: $DEFAULT_PORT)"
    echo "  -s LIST   Namespace sizes in MiB. Either a single value for all of them"
    echo "            or a comma-separated list of 4. (Default: $DEFAULT_NS_SIZE_MIB)"
    echo "  -m MB     Memory pool size of the target in MiB. (Default: $SPDK_TGT_MEM_MB)"
    echo "  -n NAME   Name of the data dir under the temporary dir."
    echo "            (Default: $DATA_DIR_NAME)"
    echo "  -T DIR    Parent dir for the data dir. (Default: $ELBENCHO_TEST_TMP)"
    echo "  -e PATH   elbencho binary to use in the printed example commands."
    echo "            (Default: $ELBENCHO_TEST_BIN)"
    echo "  -C        Start clean, i.e. delete an existing dataset first."
    echo "  -h        Print this help."
    echo
    echo "  Long forms are also accepted: --clean, --listen-addr, --connect-addr,"
    echo "  --port, --name, --tmp-dir, --help."
    echo
    echo "Examples:"
    echo "  Start a target with the default settings:"
    echo "    $ $(basename "$0")"
    echo
    echo "  Local use only, with 4 namespaces of 8 GiB each:"
    echo "    $ $(basename "$0") -a 127.0.0.1 -s 8192"
    echo
    echo "  Start over with empty namespaces on a different port:"
    echo "    $ $(basename "$0") --clean -p 4430"

    exit 1
}

parse_args()
{
    local OPTIND # local to prevent effects from other subscripts

    while getopts ":a:c:Ce:hm:n:p:s:T:" opt; do
        case "${opt}" in
        a)
            LISTEN_ADDR="${OPTARG}"
            ;;
        c)
            CONNECT_ADDR="${OPTARG}"
            ;;
        C)
            INTERACTIVE_CLEAN=1
            ;;
        e)
            ELBENCHO_TEST_BIN="${OPTARG}"
            ;;
        m)
            TGT_MEM_MB="${OPTARG}"
            ;;
        n)
            DATA_DIR_NAME="${OPTARG}"
            ;;
        p)
            TGT_PORT="${OPTARG}"
            ;;
        s)
            NS_SIZES="${OPTARG}"
            ;;
        T)
            interactive_set_tmp_dir "${OPTARG}"
            ;;
        h)
            usage
            ;;
        *) # Other option arguments are invalid
            usage
            ;;
        esac
    done

    shift $((OPTIND-1))

    if [ $# -ne 0 ]; then
        echo "ERROR: Unexpected argument: $1" >&2
        usage
    fi
}

# Turn the "-s" value into the namespace size array. A single value applies to
# all namespaces, otherwise one value per namespace is expected.
apply_ns_sizes()
{
    local sizes
    local num_namespaces=${#SPDK_NS_SIZES_MIB[@]}
    local idx

    IFS=',' read -r -a sizes <<< "$NS_SIZES"

    if [ ${#sizes[@]} -eq 1 ]; then
        for idx in $(seq 0 $((num_namespaces - 1)) ); do
            SPDK_NS_SIZES_MIB[$idx]="${sizes[0]}"
        done
    elif [ ${#sizes[@]} -eq $num_namespaces ]; then
        SPDK_NS_SIZES_MIB=("${sizes[@]}")
    else
        echo "ERROR: \"-s\" needs either a single size or exactly $num_namespaces" \
            "comma-separated sizes." >&2
        exit 1
    fi

    for idx in "${!SPDK_NS_SIZES_MIB[@]}"; do
        case "${SPDK_NS_SIZES_MIB[$idx]}" in
            ''|*[!0-9]*)
                echo "ERROR: Not a valid namespace size in MiB: ${SPDK_NS_SIZES_MIB[$idx]}" >&2
                exit 1
                ;;
        esac
    done
}

print_connection_info()
{
    local idx
    local conf_note=""

    [ "$INTERACTIVE_CLEAN" -ne 0 ] && conf_note=" (started clean)"

    echo
    echo "=== SPDK NVMe-oF target is up -- press ctrl+c to stop it ==="
    echo
    echo "Listening on : $LISTEN_ADDR:$SPDK_TGT_PORT (tcp)"
    echo "Clients use  : $INTERACTIVE_CONNECT_ADDR:$SPDK_TGT_PORT"
    echo "Data dir     : $TEST_DIR$conf_note"
    echo "               (kept when stopping, \"-C\" starts clean)"
    echo "Config file  : $SPDK_CONF"
    echo "Target log   : $TEST_DIR/nvmf_tgt.log"
    echo
    echo "Namespaces:"
    for idx in "${!SPDK_NS_SIZES_MIB[@]}"; do
        printf '  %-16s %6s MiB   %s\n' \
            "$(spdk_ns_name "$idx")" "${SPDK_NS_SIZES_MIB[$idx]}" "$(spdk_ns_uuid "$idx")"
    done
    echo
    echo "List the namespaces as elbencho sees them:"
    echo "  $ELBENCHO_TEST_BIN --spdkconffile $SPDK_CONF"
    echo
    echo "Write all 4 namespaces with verifiable data (\"sys.*\" matches their names):"
    echo "  $ELBENCHO_TEST_BIN --spdkconffile $SPDK_CONF \\"
    echo "      -w -t 4 --iodepth 8 -b 128k --verify 1 'sys.*'"
    echo
    echo "Read it back and verify, also after a restart of this target:"
    echo "  $ELBENCHO_TEST_BIN --spdkconffile $SPDK_CONF \\"
    echo "      -r -t 4 --iodepth 8 -b 128k --verify 1 'sys.*'"
    echo
    echo "Use it from other hosts (the config is transferred to the services):"
    echo "  on each client: $ELBENCHO_TEST_BIN --service --foreground"
    echo "  here:           $ELBENCHO_TEST_BIN --hosts client1,client2 \\"
    echo "                      --spdkconffile $SPDK_CONF -r -t 4 -b 128k 'sys.*'"
    echo
    echo "Statistics of the target itself:"
    echo "  cd $TEST_DIR && python3 $ELBENCHO_TEST_SPDK_RPC -s ./nvmf.sock bdev_get_iostat"
    echo
    echo "Note: this target runs on a single core without huge pages, so it is meant"
    echo "      for functional testing rather than for performance measurements."
    echo
}

################################ Main ######################################

interactive_translate_long_args "$@"
parse_args "${INTERACTIVE_ARGV[@]}"

require_spdk

if [ ! -x "$ELBENCHO_TEST_BIN" ]; then
    echo "NOTE: elbencho binary not found, so the example commands below will not" >&2
    echo "      work as printed: $ELBENCHO_TEST_BIN" >&2
fi

apply_ns_sizes

[ -n "$TGT_MEM_MB" ] && SPDK_TGT_MEM_MB="$TGT_MEM_MB"

# NVMe-oF checks whether the address that a client connected to is registered as
# a listener of the subsystem, and a wildcard listener matches no concrete
# address, so every client would be rejected with "does not allow host ... to
# connect at this address". Hence a concrete address is required here, unlike
# for a plain http server.
if interactive_addr_is_wildcard "$LISTEN_ADDR"; then
    if [ -n "$LISTEN_ADDR" ]; then
        echo "ERROR: \"$LISTEN_ADDR\" cannot be used as listen address for an NVMe-oF" >&2
        echo "       target: clients are only allowed to connect to an address that is" >&2
        echo "       registered as a listener, so a wildcard address rejects everyone." >&2
        echo "       Give a concrete address, or leave \"-a\" out to auto-detect one." >&2
        exit 1
    fi

    LISTEN_ADDR="$(interactive_detect_addr)"

    if [ -z "$LISTEN_ADDR" ]; then
        LISTEN_ADDR="127.0.0.1"
        echo "NOTE: No non-loopback address of this host found, so the target is only" >&2
        echo "      reachable locally. Use \"-a ADDR\" to select an address." >&2
    fi
fi

interactive_resolve_connect_addr "$LISTEN_ADDR" "$CONNECT_ADDR"
interactive_warn_if_exposed "$LISTEN_ADDR" \
    "allows any host to read and write its namespaces."

interactive_init "$DATA_DIR_NAME"

interactive_require_free_port "$TGT_PORT" "NVMe-oF target"

# Tell the target library to keep the data, to use the requested port and
# addresses, and to leave the log file entry out of the generated config, since
# that config also gets used by service instances on other hosts.
SPDK_TGT_PRESERVE_DATA=1
SPDK_TGT_FIXED_PORT="$TGT_PORT"
SPDK_TGT_LISTEN_ADDR="$LISTEN_ADDR"
SPDK_TGT_CONNECT_ADDR="$INTERACTIVE_CONNECT_ADDR"
SPDK_CONF_WITH_LOG_FILE=0

# This holds no data, but a leftover runtime dir of a previous run can confuse
# the memory setup after a reboot, so always start with a fresh one.
rm -rf "$TEST_DIR/dpdk-run"

echo "Starting the SPDK NVMe-oF target..."

start_nvmf_tgt
if [ $? -ne 0 ]; then
    exit 1
fi

print_connection_info

interactive_serve "$SPDK_TGT_PID" "the SPDK NVMe-oF target" \
    "$TEST_DIR/nvmf_tgt.log" stop_nvmf_tgt
