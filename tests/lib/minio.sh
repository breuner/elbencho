#!/bin/bash
#
# Helper library to run a private minio S3 server for a single test script.
# Source this after tests/lib/testlib.sh and after calling test_init().
#
# Every test gets its own minio process, its own data dir below the test's
# temporary dir and its own random TCP ports, so S3 tests stay independent and
# can run in parallel.

: ${S3_KEY:="elbenchotest"}
: ${S3_SECRET:="elbenchotestsecret"}
: ${S3_REGION:="us-east-1"}
# Set this to enable S3 over RDMA for the S3 tests.
: ${ELBENCHO_TEST_S3RDMA:="0"}
# Set this to use an already running S3-compatible server instead of starting
# a private minio instance for each test.
: ${ELBENCHO_TEST_S3_ENDPOINT:=""}
S3_ENDPOINT=""
MINIO_PORT=""
MINIO_CONSOLE_PORT=""
MINIO_PID=""

# Fixed TCP ports. Empty means "pick random free ports and retry on collision",
# which is what the parallel test suite needs. Fixed ports turn a collision into
# a hard error instead.
: ${MINIO_FIXED_PORT:=""}
: ${MINIO_FIXED_CONSOLE_PORT:=""}

# Address minio binds to. The test suite stays on loopback. The web console gets
# its own value on purpose, so that the S3 port can be exposed while the console
# - which serves a login form for the root credentials - stays on loopback.
: ${MINIO_LISTEN_ADDR:="127.0.0.1"}
: ${MINIO_CONSOLE_LISTEN_ADDR:=""}

# Address used for the endpoint url and for the readiness probe. Empty means
# "same as the listen address".
: ${MINIO_CONNECT_ADDR:=""}

# 1 = keep the existing data dir, so that its objects survive a restart.
: ${MINIO_PRESERVE_DATA:="0"}

# Address the endpoint url actually points at, resolved by start_minio.
MINIO_ADDR="127.0.0.1"

# elbencho arguments to talk to this test's minio instance. Set by start_minio.
S3_OPTS=()

########################## Downloading the server ###########################

# The server that gets downloaded is libreFS, a community fork of the minio
# server, which was discontinued as open source. It keeps minio's command line
# and environment variable interface, and the downloaded executable is stored
# under the name "minio", so that it is a drop-in replacement and everything
# else in this test suite stays unchanged.
MINIO_URL_AMD64="https://github.com/libreFS/libreFS/releases/latest/download/librefs-linux-amd64"
MINIO_URL_ARM64="https://github.com/libreFS/libreFS/releases/latest/download/librefs-linux-arm64"

# minio_ensure_binary DEST_PATH
#
# Download the S3 server to the given path if it is not there yet. An already
# existing file is kept as it is, so delete it to pick up a different one. This is
# the one function of this library that is deliberately self-contained, so that
# it can also be used by tests/run-tests.sh and by the interactive tools without
# sourcing the rest of the test suite. Prints plain messages and returns
# non-zero on failure, it never exits by itself.
minio_ensure_binary()
{
    local minio_path="$1"
    local minio_url
    local arch

    if [ -x "$minio_path" ]; then
        return 0
    fi

    arch="$(uname -m)"
    case "$arch" in
    x86_64|amd64)
        minio_url="$MINIO_URL_AMD64"
        ;;
    aarch64|arm64)
        minio_url="$MINIO_URL_ARM64"
        ;;
    *)
        echo "ERROR: No S3 server download available for this architecture: $arch" >&2
        return 1
        ;;
    esac

    echo "Downloading the libreFS S3 server (a fork of the discontinued minio server)..."
    echo "  $minio_url"

    mkdir -p "$(dirname "$minio_path")"

    if command -v curl > /dev/null 2>&1; then
        curl -fLsS -o "$minio_path.tmp" "$minio_url"
    elif command -v wget > /dev/null 2>&1; then
        wget -q -O "$minio_path.tmp" "$minio_url"
    else
        echo "ERROR: Neither curl nor wget found in PATH to download the S3 server." >&2
        return 1
    fi

    if [ $? -ne 0 ]; then
        rm -f "$minio_path.tmp"
        echo "ERROR: Download of the S3 server failed." >&2
        echo "       Alternatively, put an S3 server binary here yourself..." >&2
        echo "         $minio_path" >&2
        echo "       ...or point ELBENCHO_TEST_MINIO at an existing one. The S3 tests" >&2
        echo "       skip themselves while no server binary is available." >&2
        return 1
    fi

    chmod +x "$minio_path.tmp" && mv "$minio_path.tmp" "$minio_path"
    if [ $? -ne 0 ]; then
        echo "ERROR: Unable to install the downloaded S3 server at: $minio_path" >&2
        rm -f "$minio_path.tmp"
        return 1
    fi

    return 0
}

# Bucket name derived from the test script name. Lower case and hyphens only,
# so it is a valid DNS-style bucket name.
bucket_name()
{
    echo "elbencho-test-$(echo "$TEST_NAME" | tr '[:upper:]_.' '[:lower:]--')"
}

# Skip the whole test file unless a minio server binary is available. Call this
# before test_init/tap_plan, because skipping after the plan line has been
# printed would result in two plan lines, which is not valid TAP.
require_minio()
{
    if [ "$ELBENCHO_TEST_S3RDMA" = "1" ]; then
        require_build_feature s3rdma
    fi

    if [ -n "$ELBENCHO_TEST_S3_ENDPOINT" ]; then
        require_cmd aws
        return 0
    fi

    require_cmd curl

    if [ ! -x "$ELBENCHO_TEST_MINIO" ]; then
        tap_skip_all "minio server not found at $ELBENCHO_TEST_MINIO (use \"run-tests.sh -s\" to download it)"
    fi
}

# Configure the elbencho arguments and aws cli environment shared by both the
# private-server and external-server modes.
configure_s3()
{
    S3_OPTS=( --s3endpoints "$S3_ENDPOINT"
              --s3key "$S3_KEY"
              --s3secret "$S3_SECRET"
              --s3region "$S3_REGION" )

    if [ "$ELBENCHO_TEST_S3RDMA" = "1" ]; then
        S3_OPTS+=( --s3rdma )
    fi

    # for the aws cli
    export AWS_ACCESS_KEY_ID="$S3_KEY"
    export AWS_SECRET_ACCESS_KEY="$S3_SECRET"
    export AWS_REGION="$S3_REGION"
    export AWS_DEFAULT_REGION="$S3_REGION"
    export AWS_EC2_METADATA_DISABLED="true"
    export AWS_PAGER=""
}

# Wait until the minio instance can actually serve S3 requests. Gives up if the
# process died in the meantime.
#
# Note that the health endpoint already answers while the server is still
# initializing, so it is not sufficient here: a bucket creation right after it
# passes can still fail with "XMinioServerNotInitialized". Therefore this probes
# the S3 API itself. An anonymous request gets a definitive "access denied" once
# the server is ready, but a 503 while it is still starting up.
wait_for_minio_ready()
{
    local waited=0
    local code

    wait_for_port "$MINIO_ADDR" "$MINIO_PORT" 15 || return 1

    while [ $waited -lt 300 ]; do
        code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 \
            "http://$MINIO_ADDR:$MINIO_PORT/" 2>/dev/null)

        case "$code" in
            2*|4*) return 0 ;; # server answered as an S3 endpoint
        esac

        kill -0 "$MINIO_PID" >/dev/null 2>&1 || return 1
        sleep 0.1
        waited=$((waited+1))
    done

    return 1
}

# Start a private minio server. Skips the whole test file if the minio binary
# has not been downloaded yet. Returns non-zero if the server did not come up.
start_minio()
{
    local datadir="$TEST_DIR/minio-data"
    local tries=0
    local maxtries=5
    local console_addr="${MINIO_CONSOLE_LISTEN_ADDR:-$MINIO_LISTEN_ADDR}"

    if [ -n "$ELBENCHO_TEST_S3_ENDPOINT" ]; then
        S3_ENDPOINT="$ELBENCHO_TEST_S3_ENDPOINT"
        configure_s3
        return 0
    fi

    require_cmd curl

    if [ ! -x "$ELBENCHO_TEST_MINIO" ]; then
        tap_skip_all "minio server not found at $ELBENCHO_TEST_MINIO (use \"run-tests.sh -s\" to download it)"
    fi

    # Never probe or advertise a wildcard address, even though it happens to
    # work as a destination on Linux.
    MINIO_ADDR="${MINIO_CONNECT_ADDR:-$MINIO_LISTEN_ADDR}"
    case "$MINIO_ADDR" in
        ""|0.0.0.0|::|"[::]") MINIO_ADDR="127.0.0.1" ;;
    esac

    # A fixed port that is taken is a hard error, so retrying is pointless.
    if [ -n "$MINIO_FIXED_PORT" ] || [ -n "$MINIO_FIXED_CONSOLE_PORT" ]; then
        maxtries=1
    fi

    while [ $tries -lt $maxtries ]; do
        tries=$((tries+1))

        [ "$MINIO_PRESERVE_DATA" = "1" ] || rm -rf "$datadir"
        mkdir -p "$datadir"

        MINIO_PORT="${MINIO_FIXED_PORT:-$(find_free_port)}"
        MINIO_CONSOLE_PORT="${MINIO_FIXED_CONSOLE_PORT:-$(find_free_port)}"
        if [ -z "$MINIO_PORT" ] || [ -z "$MINIO_CONSOLE_PORT" ]; then
            tap_diag "Unable to find a free TCP port for minio."
            return 1
        fi

        if [ -n "$MINIO_FIXED_PORT" ] && port_in_use "$MINIO_PORT"; then
            tap_diag "TCP port $MINIO_PORT is already in use."
            tap_diag "Check with:  ss -ltnp | grep ':$MINIO_PORT'"
            return 1
        fi

        if [ -n "$MINIO_FIXED_CONSOLE_PORT" ] && port_in_use "$MINIO_CONSOLE_PORT"; then
            tap_diag "TCP port $MINIO_CONSOLE_PORT (web console) is already in use."
            return 1
        fi

        # The "env" prefix makes the traced line paste-able. The credentials are
        # the well known test values from the top of this file.
        trace_cmd "minio server start (attempt $tries)" \
            env MINIO_ROOT_USER="$S3_KEY" MINIO_ROOT_PASSWORD="$S3_SECRET" \
            "$ELBENCHO_TEST_MINIO" server \
            --address "$MINIO_LISTEN_ADDR:$MINIO_PORT" \
            --console-address "$console_addr:$MINIO_CONSOLE_PORT" \
            --certs-dir "$TEST_DIR/certs" \
            "$datadir"
        trace_add_log "$TEST_DIR/minio.log"

        # stdout/stderr must go to a file: prove reads the test's stdout until
        # EOF, so a background process inheriting it would hang the whole run.
        MINIO_ROOT_USER="$S3_KEY" MINIO_ROOT_PASSWORD="$S3_SECRET" \
            "$ELBENCHO_TEST_MINIO" server \
            --address "$MINIO_LISTEN_ADDR:$MINIO_PORT" \
            --console-address "$console_addr:$MINIO_CONSOLE_PORT" \
            --certs-dir "$TEST_DIR/certs" \
            "$datadir" >> "$TEST_DIR/minio.log" 2>&1 &

        MINIO_PID=$!
        register_pid "$MINIO_PID"

        if wait_for_minio_ready; then
            S3_ENDPOINT="http://$MINIO_ADDR:$MINIO_PORT"

            configure_s3

            return 0
        fi

        tap_diag "minio did not become ready on port $MINIO_PORT, retrying..."
        kill -KILL "$MINIO_PID" >/dev/null 2>&1
        wait "$MINIO_PID" 2>/dev/null
    done

    tap_diag "Giving up on starting minio. Server log:"
    tap_diag_file "$TEST_DIR/minio.log"

    return 1
}

# Terminate minio cleanly and wait for it to be gone. Gives it more time than
# kill_registered does, so that it can flush its metadata.
stop_minio()
{
    local waited=0

    [ -z "$MINIO_PID" ] && return 0

    kill -TERM "$MINIO_PID" > /dev/null 2>&1

    while [ $waited -lt 200 ]; do
        kill -0 "$MINIO_PID" 2>/dev/null || break
        sleep 0.1
        waited=$((waited+1))
    done

    kill -KILL "$MINIO_PID" > /dev/null 2>&1
    wait "$MINIO_PID" 2>/dev/null

    MINIO_PID=""

    return 0
}

# aws cli wrapper for the s3api subcommand against this test's minio instance.
aws_s3api()
{
    trace_cmd "aws s3api" \
        aws --endpoint-url "$S3_ENDPOINT" --output text s3api "$@"

    timeout "$ELBENCHO_TEST_CMD_TIMEOUT" \
        aws --endpoint-url "$S3_ENDPOINT" --output text s3api "$@"
}

# Number of objects in the given bucket, optionally restricted to a key prefix.
s3_object_count()
{
    local bucket="$1"
    local prefix="$2"
    local count

    if [ -n "$prefix" ]; then
        count=$(aws_s3api list-objects-v2 --bucket "$bucket" --prefix "$prefix" \
            --query 'length(Contents)' 2>/dev/null)
    else
        count=$(aws_s3api list-objects-v2 --bucket "$bucket" \
            --query 'length(Contents)' 2>/dev/null)
    fi

    # an empty bucket yields "None" instead of 0
    if [ -z "$count" ] || [ "$count" = "None" ]; then
        echo "0"
    else
        echo "$count"
    fi
}

# Space separated sorted list of the distinct object sizes in the given bucket.
s3_unique_object_sizes()
{
    aws_s3api list-objects-v2 --bucket "$1" --query 'Contents[].Size' 2>/dev/null | \
        tr '\t' '\n' | sort -un | tr '\n' ' ' | sed -e 's/ $//'
}

# Space separated sorted list of all object keys in the given bucket.
s3_object_keys()
{
    aws_s3api list-objects-v2 --bucket "$1" --query 'Contents[].Key' 2>/dev/null | \
        tr '\t' '\n' | sort | tr '\n' ' ' | sed -e 's/ $//'
}

# s3_object_size BUCKET KEY
s3_object_size()
{
    aws_s3api head-object --bucket "$1" --key "$2" --query 'ContentLength' 2>/dev/null
}

# s3_bucket_exists BUCKET
s3_bucket_exists()
{
    aws_s3api head-bucket --bucket "$1" >/dev/null 2>&1
}
