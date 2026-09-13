#!/bin/bash
#
# Start a minio S3 server for interactive elbencho testing and keep it running
# until ctrl+c. The objects are kept, so the same dataset can be used again
# after a restart of the server or of the whole host.
#
# This reuses the same server setup as the automated S3 tests, see
# tests/lib/minio.sh.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/minio.sh" || exit 1
source "$ELBENCHO_TEST_LIB/interactive.sh" || exit 1

DEFAULT_PORT=9000
DEFAULT_CONSOLE_PORT=9001
DATA_DIR_NAME="tools-minio"

LISTEN_ADDR="0.0.0.0"               # user-definable via "-a"
CONNECT_ADDR=""                     # user-definable via "-c"
S3_PORT=$DEFAULT_PORT               # user-definable via "-p"
CONSOLE_PORT=$DEFAULT_CONSOLE_PORT  # user-definable via "-P"
BUCKET=""                           # user-definable via "-b"

usage()
{
    echo "About:"
    echo "  Start a minio S3 server for interactive elbencho testing and keep it"
    echo "  running until ctrl+c."
    echo
    echo "  The objects are kept when the server is stopped, so the same dataset can"
    echo "  be used again the next day. Use \"-C\" to start with an empty server."
    echo
    echo "Usage:"
    echo "  $ $(basename "$0") [OPTIONS]"
    echo
    echo "Optional Arguments:"
    echo "  -a ADDR   Address to listen on. (Default: $LISTEN_ADDR, i.e. all interfaces)"
    echo "  -c ADDR   Address that clients connect to, for the case that they reach"
    echo "            this host under a different address or name."
    echo "            (Default: auto-detected address of this host)"
    echo "  -p PORT   TCP port for the S3 endpoint. (Default: $DEFAULT_PORT)"
    echo "  -P PORT   TCP port for the web console, which always stays on localhost,"
    echo "            because it serves a login form for the credentials below."
    echo "            (Default: $DEFAULT_CONSOLE_PORT)"
    echo "  -b NAME   Bucket to create on startup. (Default: $(bucket_name_for "$DATA_DIR_NAME"))"
    echo "  -k KEY    S3 access key. (Default: $S3_KEY)"
    echo "  -s SECRET S3 access secret. (Default: $S3_SECRET)"
    echo "  -n NAME   Name of the data dir under the temporary dir."
    echo "            (Default: $DATA_DIR_NAME)"
    echo "  -T DIR    Parent dir for the data dir. (Default: $ELBENCHO_TEST_TMP)"
    echo "  -e PATH   elbencho binary to use in the printed example commands."
    echo "            (Default: $ELBENCHO_TEST_BIN)"
    echo "  -C        Start clean, i.e. delete existing objects first."
    echo "  -h        Print this help."
    echo
    echo "  Long forms are also accepted: --clean, --listen-addr, --connect-addr,"
    echo "  --port, --name, --tmp-dir, --help."
    echo
    echo "Examples:"
    echo "  Start a server with the default settings:"
    echo "    $ $(basename "$0")"
    echo
    echo "  Local use only, on a different port:"
    echo "    $ $(basename "$0") -a 127.0.0.1 -p 9100"
    echo
    echo "  Start over with an empty server:"
    echo "    $ $(basename "$0") --clean"

    exit 1
}

# Same rule as bucket_name() in minio.sh, but for a name given on the command
# line, so that the default can be shown in the help text.
bucket_name_for()
{
    echo "elbencho-test-$(echo "$1" | tr '[:upper:]_.' '[:lower:]--')"
}

parse_args()
{
    local OPTIND # local to prevent effects from other subscripts

    while getopts ":a:b:c:Ce:hk:n:p:P:s:T:" opt; do
        case "${opt}" in
        a)
            LISTEN_ADDR="${OPTARG}"
            ;;
        b)
            BUCKET="${OPTARG}"
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
        k)
            S3_KEY="${OPTARG}"
            ;;
        n)
            DATA_DIR_NAME="${OPTARG}"
            ;;
        p)
            S3_PORT="${OPTARG}"
            ;;
        P)
            CONSOLE_PORT="${OPTARG}"
            ;;
        s)
            S3_SECRET="${OPTARG}"
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

print_connection_info()
{
    local clean_note=""

    [ "$INTERACTIVE_CLEAN" -ne 0 ] && clean_note=" (started clean)"

    echo
    echo "=== minio S3 server is up -- press ctrl+c to stop it ==="
    echo
    echo "Endpoint     : $S3_ENDPOINT"
    echo "Listening on : $LISTEN_ADDR:$MINIO_PORT (S3)"
    echo "Web console  : http://127.0.0.1:$MINIO_CONSOLE_PORT (localhost only)"
    echo "Bucket       : $BUCKET"
    echo "Access key   : $S3_KEY"
    echo "Secret       : $S3_SECRET"
    echo "Region       : $S3_REGION"
    echo "Data dir     : $TEST_DIR$clean_note"
    echo "               (kept when stopping, \"-C\" starts clean)"
    echo "Server log   : $TEST_DIR/minio.log"
    echo
    echo "Write 8000 objects of 1 MiB each with verifiable data (\"-N\" is per thread):"
    echo "  $ELBENCHO_TEST_BIN --s3endpoints $S3_ENDPOINT \\"
    echo "      --s3key $S3_KEY --s3secret $S3_SECRET \\"
    echo "      -w -t 8 -n 0 -N 1000 -s 1m -b 1m --verify 1 s3://$BUCKET"
    echo
    echo "Read them back and verify, also after a restart of this server:"
    echo "  $ELBENCHO_TEST_BIN --s3endpoints $S3_ENDPOINT \\"
    echo "      --s3key $S3_KEY --s3secret $S3_SECRET \\"
    echo "      -r -t 8 -n 0 -N 1000 -s 1m -b 1m --verify 1 s3://$BUCKET"
    echo
    echo "List the objects with the aws cli:"
    echo "  AWS_ACCESS_KEY_ID=$S3_KEY AWS_SECRET_ACCESS_KEY=$S3_SECRET \\"
    echo "  AWS_DEFAULT_REGION=$S3_REGION AWS_EC2_METADATA_DISABLED=true \\"
    echo "  aws --endpoint-url $S3_ENDPOINT s3 ls s3://$BUCKET/"
    echo
}

################################ Main ######################################

interactive_translate_long_args "$@"
parse_args "${INTERACTIVE_ARGV[@]}"

require_cmd curl

if [ ! -x "$ELBENCHO_TEST_BIN" ]; then
    echo "NOTE: elbencho binary not found, so the example commands below will not" >&2
    echo "      work as printed: $ELBENCHO_TEST_BIN" >&2
fi

minio_ensure_binary "$ELBENCHO_TEST_MINIO"
if [ $? -ne 0 ]; then
    exit 1
fi

interactive_resolve_connect_addr "$LISTEN_ADDR" "$CONNECT_ADDR"
interactive_warn_if_exposed "$LISTEN_ADDR" \
    "uses the well known test credentials shown below."

interactive_init "$DATA_DIR_NAME"

interactive_require_free_port "$S3_PORT" "S3 endpoint"
interactive_require_free_port "$CONSOLE_PORT" "web console"

[ -z "$BUCKET" ] && BUCKET="$(bucket_name)"

# Tell the server library to keep the objects and to use the requested ports and
# addresses. The web console stays on localhost on purpose: it is not needed
# remotely and it offers a login form for the credentials.
MINIO_PRESERVE_DATA=1
MINIO_FIXED_PORT="$S3_PORT"
MINIO_FIXED_CONSOLE_PORT="$CONSOLE_PORT"
MINIO_LISTEN_ADDR="$LISTEN_ADDR"
MINIO_CONSOLE_LISTEN_ADDR="127.0.0.1"
MINIO_CONNECT_ADDR="$INTERACTIVE_CONNECT_ADDR"

echo "Starting the minio S3 server..."

start_minio
if [ $? -ne 0 ]; then
    exit 1
fi

# Create the bucket, so that the aws cli and elbencho read phases work right
# away. An already existing bucket is not an error.
if command -v aws > /dev/null 2>&1; then
    aws_s3api create-bucket --bucket "$BUCKET" > /dev/null 2>&1
else
    echo "NOTE: The aws cli tool was not found, so the bucket was not created." >&2
    echo "      elbencho creates it itself when its \"-d\" option is used." >&2
fi

print_connection_info

interactive_serve "$MINIO_PID" "the minio S3 server" "$TEST_DIR/minio.log" stop_minio
