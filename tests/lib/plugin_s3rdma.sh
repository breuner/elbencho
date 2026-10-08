#!/bin/bash
#
# Helper library for the tests of the S3-over-RDMA plugins (tests_plugin_s3rdma/).
# Starts a private instance of the S3-over-RDMA test server (tools/s3rdma/server)
# for a single test script and configures elbencho to use one of the s3rdma_*
# plugins against it. The server speaks two RDMA protocols, and a test selects
# one via require_s3rdma (NVIDIA cuObject's protocol, used by the elbencho
# plugins: through NVIDIA's client library with DC transport, or through the
# client shim from tools/s3rdma/cuobjclient-shim with RC transport) or
# require_hipobj_rc (AMD hipObject's hipobj-rc-v2, driven by hipObject's own
# test client, because no elbencho plugin speaks it yet).
#
# Source this after tests/lib/testlib.sh and tests/lib/minio.sh, which provides
# the aws cli helpers and configure_s3(). With ELBENCHO_TEST_S3_ENDPOINT set, the
# tests run against that external S3 server instead of a private instance.
#
# Every test gets its own server process, its own data dir below the test's
# temporary dir and its own random TCP port (and a random cuObject server port,
# which cannot be shared between processes), so the tests can run in parallel.
#
# The RDMA NICs and the GPUs may be in use by other traffic and other programs
# while the tests run, so the tests do not assume exclusive use: the NIC's
# counters only have to grow, the transfers are counted in the log of the
# private server instance, and the buffer allocations stay small (a few MiB per
# thread, both in elbencho and in the server).

# The test server, built by "run-tests.sh -r" from tools/s3rdma/server in a copy
# of the tool sources below the temporary dir (so that the tests write nothing
# outside of it).
: ${ELBENCHO_TEST_S3RDMA_SERVER:="$ELBENCHO_TEST_TMP/s3rdma/server/bin/s3rdma-server"}

# AMD hipObject's RDMA test client, built next to the server from the hipObject
# sources that the server's Makefile downloads.
: ${ELBENCHO_TEST_HIPOBJ_CLIENT:="$(dirname "$ELBENCHO_TEST_S3RDMA_SERVER")/hipobj-v2-data-client"}

# The S3-over-RDMA test client (tools/s3rdma/client), built next to the server
# against the RC shim for the cuObject client API (tools/s3rdma/cuobjclient-shim).
: ${ELBENCHO_TEST_S3RDMA_CLIENT:="$ELBENCHO_TEST_TMP/s3rdma/client/bin/s3rdma-client"}

# The plugin to activate. Empty means "the first of the known plugins that the
# elbencho binary was built with".
: ${ELBENCHO_TEST_S3RDMA_PLUGIN:=""}

# The RDMA transport for the plugin tests: "cuobj" = NVIDIA's cuObject client
# library (DC transport: needs a ConnectX-5 or newer NIC and libcufile_rdma.so),
# "rc" = the cuObject client shim from tools/s3rdma/cuobjclient-shim (Reliable
# Connections on any RoCE NIC, no GPU needed). Empty means "cuobj where it works,
# else rc with a note".
: ${ELBENCHO_TEST_S3RDMA_TRANSPORT:=""}

# Dir of the shim's libcuobjclient.so, built next to the server. It is put on
# LD_LIBRARY_PATH for the "rc" transport, which also redirects an elbencho that
# was linked against NVIDIA's library to the shim.
: ${ELBENCHO_TEST_CUOBJ_SHIM_LIB_DIR:="$ELBENCHO_TEST_TMP/s3rdma/cuobjclient-shim/lib"}

# The GPU for the tests that transfer from and to GPU memory. Empty means "the
# first GPU that nvidia-smi lists".
: ${ELBENCHO_TEST_GPU_ID:=""}

# IPv4 address of the RDMA NIC, used by the server and by the cuObject client.
# Empty means "the first active RDMA link that has an IPv4 address".
: ${ELBENCHO_TEST_RDMA_ADDR:=""}

# Dir containing libcufile_rdma.so, which the cuObject client loads via dlopen
# and therefore needs on LD_LIBRARY_PATH. Empty means "auto-detect".
: ${ELBENCHO_TEST_CUFILE_LIB_DIR:=""}

# The "rdma_peer_type" for the cuObject client config, i.e. how cuFile pins the
# buffers for RDMA. Empty means "dmabuf" where the nvidia_peermem module is not
# loaded (the open kernel module uses dma-buf), else cuFile's default.
: ${ELBENCHO_TEST_CUOBJ_PEER_TYPE:=""}

# 1 = set "allow_compat_mode" in the cuObject client config. cuFile may refuse to
# initialize on a host without a GPU otherwise. This only affects cuFile's file
# I/O fallback; the object data of these tests can still only move via RDMA,
# because the control requests carry no body. Off by default so that the tests
# run with the plain RDMA configuration where possible.
: ${ELBENCHO_TEST_CUFILE_COMPAT_MODE:="0"}

S3RDMA_KNOWN_PLUGINS="s3rdma_minio s3rdma_cloudian"

: ${S3RDMA_LISTEN_ADDR:="127.0.0.1"}

S3RDMA_TRANSPORT=""        # "cuobj", "rc" or "" for plain HTTP; set by the require_* functions
S3RDMA_TRANSPORT_ERR=""    # why a transport is not usable; set by the *_usable functions
S3RDMA_PLUGIN=""           # set by require_s3rdma; empty = no plugin
S3RDMA_ADDR=""             # set by require_s3rdma / require_hipobj_rc
S3RDMA_CUFILE_LIB_DIR=""   # set by s3rdma_cuobj_usable
S3RDMA_GPU_ID=""           # set by require_s3rdma_gpu
S3RDMA_PORT=""
S3RDMA_PID=""
S3RDMA_SERVER_OPTS=()      # extra server options, set by a test before start_s3rdma_server
S3RDMA_SERVER_BUFSIZE="4m" # size of the server's RDMA staging buffers (one per server thread)
CUOBJ_MIN_GPU_FREE_MIB=2048 # free GPU memory for the CUDA contexts of parallel tests
S3RDMA_DATA_DIR=""         # set by start_s3rdma_server: where the server stores the objects
S3RDMA_LOG=""              # set by start_s3rdma_server: the server log

############################ Capability checks ##############################

# The first known plugin that the binary was built with.
s3rdma_detect_plugin()
{
    local plugin

    for plugin in $S3RDMA_KNOWN_PLUGINS; do
        if has_plugin "$plugin"; then
            echo "$plugin"
            return 0
        fi
    done

    return 1
}

# The IPv4 address of the first active RDMA link that has one.
rdma_first_ipv4()
{
    local netdev
    local addr
    local netdevs

    netdevs=$(rdma link show 2>/dev/null | \
        awk '/state ACTIVE/ { for(i=1; i<=NF; i++) if($i == "netdev") print $(i+1) }')

    # fallback without the rdma tool: every netdev of an RDMA device
    if [ -z "$netdevs" ]; then
        netdevs=$(find /sys/class/infiniband/*/device/net -mindepth 1 -maxdepth 1 \
            -printf '%f\n' 2>/dev/null)
    fi

    for netdev in $netdevs; do
        addr=$(ip -4 -o addr show dev "$netdev" 2>/dev/null | awk '{print $4}' | \
            cut -d/ -f1 | head -n1)

        if [ -n "$addr" ]; then
            echo "$addr"
            return 0
        fi
    done

    return 1
}

# rdma_device_for_ipv4 ADDR
#
# The RDMA device (e.g. "mlx5_0") whose netdev carries the given IPv4 address.
rdma_device_for_ipv4()
{
    local netdev
    local device

    netdev=$(ip -4 -o addr show to "$1" 2>/dev/null | awk '{print $2}' | head -n1)
    [ -n "$netdev" ] || return 1

    device=$(rdma link show 2>/dev/null | awk -v nd="$netdev" \
        '{ for(i=1; i<=NF; i++) if($i == "netdev" && $(i+1) == nd) { sub("/.*", "", $2); print $2 } }' | \
        head -n1)

    # fallback without the rdma tool
    if [ -z "$device" ]; then
        device=$(ls "/sys/class/net/$netdev/device/infiniband/" 2>/dev/null | head -n1)
    fi

    [ -n "$device" ] || return 1

    echo "$device"
}

# rdma_hw_counter DEVICE COUNTER
#
# Value of an RDMA hardware counter of the given device, e.g. "rx_read_requests"
# (RDMA reads by a remote side) or "rx_write_requests" (RDMA writes by a remote
# side). Prints nothing if the counter is not available.
rdma_hw_counter()
{
    cat "/sys/class/infiniband/$1/ports/1/hw_counters/$2" 2>/dev/null
}

# The dir of libcufile_rdma.so, via the dynamic linker cache or the CUDA dirs.
cufile_lib_dir()
{
    local libpath

    if [ -n "$ELBENCHO_TEST_CUFILE_LIB_DIR" ]; then
        echo "$ELBENCHO_TEST_CUFILE_LIB_DIR"
        return 0
    fi

    libpath=$(ldconfig -p 2>/dev/null | awk '/libcufile_rdma\.so / {print $NF}' | head -n1)

    if [ -z "$libpath" ]; then
        libpath=$(find /usr/local/cuda*/targets/*/lib -maxdepth 1 -name 'libcufile_rdma.so' \
            2>/dev/null | head -n1)
    fi

    [ -n "$libpath" ] || return 1

    dirname "$libpath"
}

# Skip the whole test file unless elbencho has an s3rdma plugin and the
# S3-over-RDMA test server can be started (or an external S3 server is given).
# This is all that a plain HTTP test of the server itself needs; the plugin
# requirement keeps the whole group out of builds without a plugin, whose test
# runs also do not build the tools. Call this before test_init/tap_plan.
require_s3rdma_server()
{
    require_build_feature s3
    require_aws_cli

    if ! s3rdma_detect_plugin > /dev/null; then
        tap_skip_all "elbencho was built without an S3-over-RDMA plugin (ELB_PLUGIN_S3RDMA_MINIO=1 or ELB_PLUGIN_S3RDMA_CLOUDIAN=1)"
    fi

    [ -n "$ELBENCHO_TEST_S3_ENDPOINT" ] && return 0

    if [ ! -x "$ELBENCHO_TEST_S3RDMA_SERVER" ]; then
        tap_skip_all "S3-over-RDMA test server not found at $ELBENCHO_TEST_S3RDMA_SERVER (\"run-tests.sh -r\" builds it there)"
    fi
}

# Skip the whole test file unless an RDMA NIC with an IPv4 address is available
# and the locked memory limit allows the RDMA buffer registrations of the server
# (its threads times the staging buffer size) and the clients. A limit of a few
# MiB is e.g. what systemd gives its services by default. Sets S3RDMA_ADDR.
require_rdma_addr()
{
    local memlock_kib

    S3RDMA_ADDR="${ELBENCHO_TEST_RDMA_ADDR:-$(rdma_first_ipv4)}"

    if [ -z "$S3RDMA_ADDR" ]; then
        tap_skip_all "no active RDMA link with an IPv4 address found on this host"
    fi

    memlock_kib="$(ulimit -l)"

    if [ "$memlock_kib" != "unlimited" ] && [ "$memlock_kib" -lt $((64 * 1024)) ]; then
        tap_skip_all "the locked memory limit is too low for RDMA buffer registration (ulimit -l: $memlock_kib KiB, need at least 64 MiB)"
    fi
}

# s3rdma_server_transport_usable cuobj|rc
#
# Whether the private test server can set up the given RDMA transport on
# S3RDMA_ADDR. cuObject's DC transport e.g. fails on NICs without support for
# Dynamically Connected queue pairs (ConnectX-4 and older). Sets S3RDMA_TRANSPORT
# on success, S3RDMA_TRANSPORT_ERR on failure.
s3rdma_server_transport_usable()
{
    local opts=( --no-rc --rdma-port "$(find_free_port)" ) # (cuObject port: one process per port)
    local check_output
    local reason

    [ "$1" = "rc" ] && opts=( --no-cuobj )

    trace_cmd "rdma check" "$ELBENCHO_TEST_S3RDMA_SERVER" --rdma-check --rdma-addr "$S3RDMA_ADDR" \
        "${opts[@]}"

    check_output=$("$ELBENCHO_TEST_S3RDMA_SERVER" --rdma-check --rdma-addr "$S3RDMA_ADDR" \
        "${opts[@]}" 2>&1)
    if [ $? -ne 0 ]; then
        # the server's first ERROR line, or whatever else it printed (e.g. a loader error)
        reason="$(echo "$check_output" | grep -m1 '^ERROR' | sed -e 's/^ERROR: //')"
        [ -n "$reason" ] || reason="$(echo "$check_output" | grep -v '^INFO' | tail -n1)"

        S3RDMA_TRANSPORT_ERR="RDMA transport \"$1\" not usable on $S3RDMA_ADDR: $reason"
        return 1
    fi

    S3RDMA_TRANSPORT="$1"
}

# require_s3rdma_server_transport cuobj|rc
#
# Skip the whole test file unless the private test server can set up the given
# RDMA transport on S3RDMA_ADDR.
require_s3rdma_server_transport()
{
    s3rdma_server_transport_usable "$1" || tap_skip_all "$S3RDMA_TRANSPORT_ERR"
}

# Whether an s3rdma plugin can run through NVIDIA's cuObject client library:
# elbencho not linked against the shim, libcufile_rdma.so present and the test
# server's cuObject transport usable (not checked for an external S3 server).
# Sets S3RDMA_CUFILE_LIB_DIR and S3RDMA_TRANSPORT on success, S3RDMA_TRANSPORT_ERR
# on failure.
s3rdma_cuobj_usable()
{
    local gpu_free_mib

    if ldd "$ELBENCHO_TEST_BIN" 2>/dev/null | grep -q 'libcuobjclient.*cuobjclient-shim/'; then
        S3RDMA_TRANSPORT_ERR="elbencho is linked against the cuObject client shim"
        return 1
    fi

    S3RDMA_CUFILE_LIB_DIR="$(cufile_lib_dir)"

    if [ -z "$S3RDMA_CUFILE_LIB_DIR" ]; then
        S3RDMA_TRANSPORT_ERR="libcufile_rdma.so not found (set ELBENCHO_TEST_CUFILE_LIB_DIR)"
        return 1
    fi

    # cuFile creates a CUDA context per elbencho process and aborts the process
    # when that fails, so leave the GPU alone when other programs have filled it
    gpu_free_mib="$(nvidia-smi --query-gpu=memory.free --format=csv,noheader,nounits 2>/dev/null | \
        head -n1 | tr -d ' ')"

    if [ -n "$gpu_free_mib" ] && [ "$gpu_free_mib" -lt "$CUOBJ_MIN_GPU_FREE_MIB" ]; then
        S3RDMA_TRANSPORT_ERR="the GPU has only $gpu_free_mib MiB free (need $CUOBJ_MIN_GPU_FREE_MIB MiB for the CUDA contexts of the cuObject clients)"
        return 1
    fi

    if [ -n "$ELBENCHO_TEST_S3_ENDPOINT" ]; then
        S3RDMA_TRANSPORT="cuobj"
        return 0
    fi

    s3rdma_server_transport_usable cuobj
}

# Whether an s3rdma plugin can run through the cuObject client shim: the shim
# library built and the test server's RC transport usable. Sets S3RDMA_TRANSPORT
# on success, S3RDMA_TRANSPORT_ERR on failure.
s3rdma_rc_usable()
{
    if [ -n "$ELBENCHO_TEST_S3_ENDPOINT" ]; then
        S3RDMA_TRANSPORT_ERR="the cuObject client shim only works with the private test server"
        return 1
    fi

    if [ ! -e "$ELBENCHO_TEST_CUOBJ_SHIM_LIB_DIR/libcuobjclient.so.1" ]; then
        S3RDMA_TRANSPORT_ERR="cuObject client shim not found at $ELBENCHO_TEST_CUOBJ_SHIM_LIB_DIR (\"run-tests.sh -r\" builds it there)"
        return 1
    fi

    s3rdma_server_transport_usable rc
}

# Skip the whole test file unless everything for S3-over-RDMA transfers of an
# s3rdma plugin is in place: the plugin in the elbencho binary, the test server,
# an RDMA NIC with an IPv4 address and a usable transport behind the cuObject
# client API. NVIDIA's library is preferred; where it cannot work (no DC-capable
# NIC, no GPU driver), the shim takes over with a note. ELBENCHO_TEST_S3RDMA_TRANSPORT
# pins one of the two. Call this before test_init/tap_plan.
require_s3rdma()
{
    local cuobj_err

    require_s3rdma_server

    S3RDMA_PLUGIN="${ELBENCHO_TEST_S3RDMA_PLUGIN:-$(s3rdma_detect_plugin)}"

    if [ -z "$S3RDMA_PLUGIN" ]; then
        tap_skip_all "elbencho was built without an S3-over-RDMA plugin (ELB_PLUGIN_S3RDMA_MINIO=1 or ELB_PLUGIN_S3RDMA_CLOUDIAN=1)"
    fi

    require_plugin "$S3RDMA_PLUGIN"
    require_rdma_addr

    case "$ELBENCHO_TEST_S3RDMA_TRANSPORT" in
        cuobj)
            s3rdma_cuobj_usable || tap_skip_all "$S3RDMA_TRANSPORT_ERR"
            ;;
        rc)
            s3rdma_rc_usable || tap_skip_all "$S3RDMA_TRANSPORT_ERR"
            ;;
        "")
            if ! s3rdma_cuobj_usable; then
                cuobj_err="$S3RDMA_TRANSPORT_ERR"

                s3rdma_rc_usable || tap_skip_all "$cuobj_err; $S3RDMA_TRANSPORT_ERR"

                tap_note "$(basename "$0"): using the cuObject client shim (RDMA over RC) instead of NVIDIA's cuObject library: $cuobj_err"
            fi
            ;;
        *)
            tap_bail "Invalid ELBENCHO_TEST_S3RDMA_TRANSPORT \"$ELBENCHO_TEST_S3RDMA_TRANSPORT\" (expected \"cuobj\" or \"rc\")"
            ;;
    esac
}

# Skip the whole test file unless S3-over-RDMA transfers from and to GPU memory
# can be tested: everything of require_s3rdma with NVIDIA's cuObject library (the
# client shim handles host memory only), elbencho built with CUDA support and a
# GPU on this host. Sets S3RDMA_GPU_ID. Call this before test_init/tap_plan.
require_s3rdma_gpu()
{
    # no fallback to the shim here, so that the skip reason names the real cause
    : ${ELBENCHO_TEST_S3RDMA_TRANSPORT:="cuobj"}

    require_s3rdma

    if [ "$S3RDMA_TRANSPORT" != "cuobj" ]; then
        tap_skip_all "GPU memory transfers need NVIDIA's cuObject library (the client shim handles host memory only)"
    fi

    has_build_feature cuda || tap_skip_all "elbencho was built without CUDA support"

    S3RDMA_GPU_ID="${ELBENCHO_TEST_GPU_ID:-$(nvidia-smi --query-gpu=index --format=csv,noheader 2>/dev/null | head -n1)}"

    if [ -z "$S3RDMA_GPU_ID" ]; then
        tap_skip_all "no GPU found on this host (nvidia-smi; set ELBENCHO_TEST_GPU_ID)"
    fi
}

# Skip the whole test file unless a transfer through the cuObject client API over
# the RC shim can be tested: the private test server, the S3-over-RDMA test client
# built against the shim, and an RDMA NIC with an IPv4 address on which RC queue
# pairs work. Always uses the private server, since the shim's connect requests
# are an extension of this server. Call this before test_init/tap_plan.
require_s3rdma_client_rc()
{
    if [ -n "$ELBENCHO_TEST_S3_ENDPOINT" ]; then
        tap_skip_all "the RC shim tests only run against the private test server"
    fi

    require_s3rdma_server

    if [ ! -x "$ELBENCHO_TEST_S3RDMA_CLIENT" ]; then
        tap_skip_all "S3-over-RDMA test client not found at $ELBENCHO_TEST_S3RDMA_CLIENT (\"run-tests.sh -r\" builds it there)"
    fi

    # the shim has the soname of NVIDIA's library, so a client whose rpath does
    # not resolve (e.g. a copied build) silently gets NVIDIA's library instead
    if ! ldd "$ELBENCHO_TEST_S3RDMA_CLIENT" 2>/dev/null | grep -q 'libcuobjclient.*cuobjclient-shim/'; then
        tap_skip_all "S3-over-RDMA test client at $ELBENCHO_TEST_S3RDMA_CLIENT does not use the cuObject client shim (rebuild it with \"make -C $(dirname "$(dirname "$ELBENCHO_TEST_S3RDMA_CLIENT")") clean all\")"
    fi

    require_rdma_addr
    require_s3rdma_server_transport rc
}

# Skip the whole test file unless a transfer via AMD hipObject's hipobj-rc-v2
# protocol can be tested: the private test server, hipObject's test client next
# to it, and an RDMA NIC with an IPv4 address on which RC queue pairs work. This
# always uses the private server, because the client is hipObject's test tool
# with fixed credentials. Call this before test_init/tap_plan.
require_hipobj_rc()
{
    if [ -n "$ELBENCHO_TEST_S3_ENDPOINT" ]; then
        tap_skip_all "the hipObject tests only run against the private test server"
    fi

    require_s3rdma_server

    if [ ! -x "$ELBENCHO_TEST_HIPOBJ_CLIENT" ]; then
        tap_skip_all "hipObject test client not found at $ELBENCHO_TEST_HIPOBJ_CLIENT (built with the test server when libibverbs and OpenSSL are available)"
    fi

    require_rdma_addr
    require_s3rdma_server_transport rc
}

########################### Starting the server ############################

# Point S3_OPTS at the current S3_ENDPOINT and, when an RDMA plugin is selected,
# activate the plugin and configure the cuObject client library behind it.
# S3_OPTS_NOPLUGIN keeps the options without the plugin, for runs that have to
# move the data in the HTTP body.
s3rdma_configure_client()
{
    configure_s3

    S3_OPTS_NOPLUGIN=( "${S3_OPTS[@]}" )

    [ -n "$S3RDMA_PLUGIN" ] || return 0

    S3_OPTS+=( --plugins "$S3RDMA_PLUGIN" )

    case "$S3RDMA_TRANSPORT" in
        cuobj) s3rdma_configure_cuobj ;;
        rc) s3rdma_configure_shim ;;
    esac
}

# NVIDIA's cuObject reads the client NIC and the RDMA peer type from a config
# file. Without nvidia_peermem, cuFile disables userspace RDMA unless the peer
# type is dma-buf, and buffer registration fails with "no devices found".
s3rdma_configure_cuobj()
{
    local compat_mode=""
    local peer_type="$ELBENCHO_TEST_CUOBJ_PEER_TYPE"
    local peer_type_line=""

    if [ "$ELBENCHO_TEST_CUFILE_COMPAT_MODE" = "1" ]; then
        compat_mode='"allow_compat_mode": true,'
    fi

    if [ -z "$peer_type" ] && [ ! -d /sys/module/nvidia_peermem ]; then
        peer_type="dmabuf"
    fi

    if [ -n "$peer_type" ]; then
        peer_type_line="\"rdma_peer_type\": \"$peer_type\","
    fi

    cat > "$TEST_DIR/cuobj.json" <<-EOF
	{
	    "properties": {
	        $compat_mode
	        $peer_type_line
	        "rdma_dev_addr_list": ["$S3RDMA_ADDR"]
	    }
	}
	EOF

    export CUFILE_ENV_PATH_JSON="$TEST_DIR/cuobj.json"
    export CUFILE_LOGFILE_PATH="$TEST_DIR/cufile.log"
    export LD_LIBRARY_PATH="$S3RDMA_CUFILE_LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

    trace_add_log "$CUFILE_LOGFILE_PATH"
}

# The shim reads the server endpoint and the client NIC from the environment. Its
# dir first on LD_LIBRARY_PATH also makes an elbencho that was linked against
# NVIDIA's library, or that loads it at runtime like the Cloudian SDK fork does,
# use the shim.
s3rdma_configure_shim()
{
    export S3RDMA_RC_ENDPOINT="$S3_ENDPOINT"
    export S3RDMA_RC_ADDR="$S3RDMA_ADDR"
    export LD_LIBRARY_PATH="$ELBENCHO_TEST_CUOBJ_SHIM_LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
}

# Start a private S3-over-RDMA test server with the RDMA transport that the
# require_* function selected, or as a plain HTTP S3 server. Returns non-zero if
# the server did not come up. With ELBENCHO_TEST_S3_ENDPOINT set, this only
# points S3_OPTS at that server.
start_s3rdma_server()
{
    local tries=0
    local maxtries=5
    local rdma_opts=( --no-rdma )

    S3RDMA_DATA_DIR="$TEST_DIR/s3-data"
    S3RDMA_LOG="$TEST_DIR/s3rdma-server.log"

    if [ -n "$ELBENCHO_TEST_S3_ENDPOINT" ]; then
        S3_ENDPOINT="$ELBENCHO_TEST_S3_ENDPOINT"
        s3rdma_configure_client
        return 0
    fi

    while [ $tries -lt $maxtries ]; do
        tries=$((tries+1))

        rm -rf "$S3RDMA_DATA_DIR"

        S3RDMA_PORT=$(find_free_port) || return 1

        # only the transport under test, so that the other one cannot get in the
        # way. A cuObject server port can be used by one process only, so it is
        # picked like the HTTP port (free as TCP port, which is what rdma_cm uses).
        case "$S3RDMA_TRANSPORT" in
            cuobj) rdma_opts=( --rdma-addr "$S3RDMA_ADDR" --no-rc
                       --rdma-port "$(find_free_port)" ) ;;
            rc) rdma_opts=( --rdma-addr "$S3RDMA_ADDR" --no-cuobj ) ;;
        esac

        trace_cmd "s3rdma-server" \
            "$ELBENCHO_TEST_S3RDMA_SERVER" --dir "$S3RDMA_DATA_DIR" \
            --addr "$S3RDMA_LISTEN_ADDR" --port "$S3RDMA_PORT" "${rdma_opts[@]}" \
            --bufsize "$S3RDMA_SERVER_BUFSIZE" --verbose "${S3RDMA_SERVER_OPTS[@]}"
        trace_add_log "$S3RDMA_LOG"

        # stdout/stderr must go to a file: prove reads the test's stdout until
        # EOF, so a background process inheriting it would hang the whole run.
        "$ELBENCHO_TEST_S3RDMA_SERVER" --dir "$S3RDMA_DATA_DIR" \
            --addr "$S3RDMA_LISTEN_ADDR" --port "$S3RDMA_PORT" "${rdma_opts[@]}" \
            --bufsize "$S3RDMA_SERVER_BUFSIZE" --verbose "${S3RDMA_SERVER_OPTS[@]}" \
            >> "$S3RDMA_LOG" 2>&1 &

        S3RDMA_PID=$!
        register_pid "$S3RDMA_PID"

        if wait_for_port "$S3RDMA_LISTEN_ADDR" "$S3RDMA_PORT" 15; then
            S3_ENDPOINT="http://$S3RDMA_LISTEN_ADDR:$S3RDMA_PORT"
            s3rdma_configure_client
            return 0
        fi

        tap_diag "s3rdma-server did not become ready on port $S3RDMA_PORT, retrying..."
        kill -KILL "$S3RDMA_PID" >/dev/null 2>&1
        wait "$S3RDMA_PID" 2>/dev/null
    done

    tap_diag "Giving up on starting s3rdma-server. Server log:"
    tap_diag_file "$S3RDMA_LOG"

    return 1
}

# Terminate the server and wait for it to be gone.
stop_s3rdma_server()
{
    [ -z "$S3RDMA_PID" ] && return 0

    kill -TERM "$S3RDMA_PID" >/dev/null 2>&1
    wait_for_pid_gone "$S3RDMA_PID" 10
    kill -KILL "$S3RDMA_PID" >/dev/null 2>&1
    wait "$S3RDMA_PID" 2>/dev/null

    S3RDMA_PID=""

    return 0
}

# hipobj_data_client PUT|GET TARGET SIZE
#
# One transfer with hipObject's test client against the private server, e.g.
# "hipobj_data_client PUT /mybucket/mykey 64". A PUT writes SIZE bytes starting
# with "dp-put-payload", a GET verifies that prefix. Output goes to
# $TEST_DIR/hipobj-client.out; returns the client's exit code.
hipobj_data_client()
{
    local out="$TEST_DIR/hipobj-client.out"

    trace_cmd "hipobj client" "$ELBENCHO_TEST_HIPOBJ_CLIENT" "$S3RDMA_LISTEN_ADDR" "$S3RDMA_PORT" \
        "$1" "$2" "$3"

    timeout "$ELBENCHO_TEST_CMD_TIMEOUT" "$ELBENCHO_TEST_HIPOBJ_CLIENT" "$S3RDMA_LISTEN_ADDR" \
        "$S3RDMA_PORT" "$1" "$2" "$3" > "$out" 2>&1
    local rc=$?

    [ $rc -eq 0 ] || tap_diag_file "$out"

    return $rc
}

# s3rdma_client ARGS...
#
# Run the S3-over-RDMA test client against the private server, with the RC shim
# configured for the server's endpoint and the test NIC. Output goes to
# $TEST_DIR/s3rdma-client.out; returns the client's exit code.
s3rdma_client()
{
    local out="$TEST_DIR/s3rdma-client.out"

    trace_cmd "s3rdma-client" env S3RDMA_RC_ENDPOINT="$S3_ENDPOINT" S3RDMA_RC_ADDR="$S3RDMA_ADDR" \
        "$ELBENCHO_TEST_S3RDMA_CLIENT" --endpoint "$S3_ENDPOINT" "$@"

    S3RDMA_RC_ENDPOINT="$S3_ENDPOINT" S3RDMA_RC_ADDR="$S3RDMA_ADDR" \
        timeout "$ELBENCHO_TEST_CMD_TIMEOUT" "$ELBENCHO_TEST_S3RDMA_CLIENT" \
        --endpoint "$S3_ENDPOINT" "$@" > "$out" 2>&1
    local rc=$?

    [ $rc -eq 0 ] || tap_diag_file "$out"

    return $rc
}

############################# Server inspection #############################

# s3rdma_object_file BUCKET KEY
#
# Path of the file in which the private server stores the given object.
s3rdma_object_file()
{
    echo "$S3RDMA_DATA_DIR/$1/$2"
}

# s3rdma_server_rdma_ops PUT|GET
#
# Number of RDMA transfers of the given kind that the private server logged.
s3rdma_server_rdma_ops()
{
    local count

    count=$(grep -c "^RDMA $1 " "$S3RDMA_LOG" 2>/dev/null)

    echo "${count:-0}"
}

# Whether the tests run against a private server whose files and log can be
# inspected, as opposed to an external S3 server.
s3rdma_server_is_private()
{
    [ -z "$ELBENCHO_TEST_S3_ENDPOINT" ]
}
