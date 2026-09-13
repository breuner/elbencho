#!/bin/bash
#
# Support library for the interactive server tools in tests/tools/.
#
# It makes the server helpers of this test suite usable outside of a test case:
# they keep running until ctrl+c, they keep their data, and they report through
# plain text instead of TAP.
#
# Source this AFTER testlib.sh and after the server library, e.g.:
#   source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
#   source "$ELBENCHO_TEST_LIB/spdk.sh" || exit 1
#   source "$ELBENCHO_TEST_LIB/interactive.sh" || exit 1
#
# It has to come last, because it replaces the output functions of testlib.sh.

INTERACTIVE_CLEAN=0            # set from the command line before interactive_init
INTERACTIVE_STOP=0
INTERACTIVE_SERVING=0
INTERACTIVE_CLEANED=0
INTERACTIVE_RC=0
INTERACTIVE_SERVER_NAME=""
INTERACTIVE_SERVER_LOG=""
INTERACTIVE_STOP_FUNC=""
INTERACTIVE_CONNECT_ADDR=""

############################ Plain text output ##############################
#
# The server libraries report through the TAP helpers of testlib.sh, which is
# the wrong format outside of a test: "# ..." lines are just noise here, and
# tap_skip_all() even exits successfully, which would make a server that failed
# to start look like a success. So the output functions get replaced.
#
# They write to stderr, so that the connection info on stdout stays usable for
# redirection.

# The command tracing of the test suite is off here as well: these tools print
# their own ready to paste command lines. trace_init() in testlib.sh already
# skips a script whose name does not end in ".t", so this is belt and braces.
ELBENCHO_TEST_TRACE=0
TRACE_FILE=""

tap_diag()
{
    echo "$*" >&2
}

tap_diag_file()
{
    if [ -s "$1" ]; then
        tail -40 "$1" | sed -e 's/^/  /' >&2
    fi
}

# A missing prerequisite is a hard error here, not a skipped test.
tap_skip_all()
{
    echo "ERROR: $*" >&2
    exit 1
}

tap_bail()
{
    echo "ERROR: $*" >&2
    exit 1
}

# Guard against accidentally using the test case setup, which would delete the
# data dir and arm a trap that deletes it again on exit.
test_init()
{
    echo "ERROR: test_init() must not be used here, use interactive_init()." >&2
    exit 1
}

############################### Temp dir ####################################

# interactive_set_tmp_dir DIR
#
# Handles a user-given parent dir for the server's data. testlib.sh has already
# applied its own defaults when it was sourced, so this has to assign instead of
# using ":=", and it has to move the minio binary path along with it - unless
# that one was pointed somewhere else explicitly.
interactive_set_tmp_dir()
{
    local newtmp="$1"

    mkdir -p "$newtmp"
    if [ ! -d "$newtmp" ]; then
        echo "ERROR: Unable to create dir for temporary files: $newtmp" >&2
        exit 1
    fi

    # readlink needs all components but the last to exist, hence the mkdir above
    newtmp="$(readlink -f "$newtmp")"

    # Only move the minio binary path along if it still holds the derived
    # default, i.e. the caller did not point it somewhere else. An already
    # downloaded binary in the old location keeps being used when the new
    # location does not have one, because downloading it again is pointless.
    if [ "$ELBENCHO_TEST_MINIO" = "$ELBENCHO_TEST_TMP/minio" ]; then
        if [ -x "$newtmp/minio" ] || [ ! -x "$ELBENCHO_TEST_MINIO" ]; then
            ELBENCHO_TEST_MINIO="$newtmp/minio"
        fi
    fi

    ELBENCHO_TEST_TMP="$newtmp"

    export ELBENCHO_TEST_TMP
    export ELBENCHO_TEST_MINIO
}

############################ Setup and cleanup ##############################

# interactive_init NAME
#
# Takes over the parts of test_init() that make sense for a long running server
# and leaves out the rest: no watchdog, because this is supposed to run for
# hours, and above all no deletion of the data dir and no cleanup trap that
# would delete it on exit - keeping the data is the whole point here.
#
# NAME should start with "tools-", so that the dir can never collide with the
# dir of a test case, which is named after the test script.
interactive_init()
{
    TEST_NAME="$1"
    TEST_DIR="$ELBENCHO_TEST_TMP/$TEST_NAME"

    mkdir -p "$ELBENCHO_TEST_TMP"

    if [ "$INTERACTIVE_CLEAN" -ne 0 ] && [ -d "$TEST_DIR" ]; then
        echo "Starting clean, removing: $TEST_DIR"
        rm -rf "$TEST_DIR"
    fi

    mkdir -p "$TEST_DIR"
    if [ $? -ne 0 ]; then
        echo "ERROR: Unable to create the data dir: $TEST_DIR" >&2
        exit 1
    fi

    interactive_lock_dir
    interactive_warn_volatile_dir

    trap interactive_on_exit EXIT
    trap interactive_on_signal INT TERM HUP
}

# Take an exclusive lock on the data dir, so that a second instance cannot
# attach to the same data. A port check alone would not catch this, because a
# second instance started with a different port would happily corrupt the data.
# The lock is held on a file descriptor and released by the kernel on exit, so
# it cannot go stale.
interactive_lock_dir()
{
    command -v flock > /dev/null 2>&1 || return 0 # best effort only

    exec 9> "$TEST_DIR/.tool.lock" || return 0

    if flock -n 9; then
        return 0
    fi

    echo "ERROR: Another instance is already using this data dir:" >&2
    echo "       $TEST_DIR" >&2
    echo "       Stop it first, or use \"-n NAME\" or \"-T DIR\" for separate data." >&2
    echo "       If you think nothing is running, look for a leftover server:" >&2
    echo "         pgrep -a -f 'nvmf_tgt|minio'" >&2
    exit 1
}

# Data on a ram based file system does not survive a reboot, which defeats the
# point of keeping it, and it also prevents direct IO on the backing files.
interactive_warn_volatile_dir()
{
    local fstype

    fstype="$(df --output=fstype "$TEST_DIR" 2>/dev/null | tail -1 | tr -d ' ')"

    case "$fstype" in
    tmpfs|ramfs|devtmpfs)
        echo "WARNING: The data dir is on $fstype, so its data is lost on reboot:" >&2
        echo "         $TEST_DIR" >&2
        echo "         Use \"-T DIR\" to put it on a real file system." >&2
        ;;
    esac
}

interactive_on_signal()
{
    # Ignore any further signal for the rest of the shutdown. Bash does not
    # block a signal while its own handler runs, so without this a second ctrl+c
    # would start the handler again.
    trap '' INT TERM HUP

    INTERACTIVE_STOP=1

    echo >&2
    echo "Signal received, shutting down..." >&2

    # Nobody polls the flag outside of the serve loop, e.g. while the server is
    # still starting up, so leave through the exit trap right away.
    if [ "$INTERACTIVE_SERVING" -eq 0 ]; then
        exit 1
    fi
}

interactive_on_exit()
{
    local exitcode=$?

    trap - EXIT
    trap '' INT TERM HUP

    interactive_cleanup

    if [ "$INTERACTIVE_RC" -ne 0 ] && [ "$exitcode" -eq 0 ]; then
        exitcode="$INTERACTIVE_RC"
    fi

    exit $exitcode
}

# Runs exactly once, no matter how the script terminates.
interactive_cleanup()
{
    [ "$INTERACTIVE_CLEANED" -ne 0 ] && return 0
    INTERACTIVE_CLEANED=1

    # Give the server a longer graceful period of its own first. Afterwards
    # kill_registered() is only a backstop, and its escalation to a hard kill
    # after 5 seconds would be too early to flush everything.
    if [ -n "$INTERACTIVE_STOP_FUNC" ]; then
        echo "Stopping $INTERACTIVE_SERVER_NAME..."
        "$INTERACTIVE_STOP_FUNC"
    fi

    kill_registered

    # The backing files may be written through the page cache, so flush before
    # claiming that the data is safely on disk.
    sync

    echo
    if [ -d "$TEST_DIR" ]; then
        echo "Data kept in: $TEST_DIR"
        echo "Use \"-C\" to start over with empty data next time."
    fi
}

################################ Serving ####################################

# interactive_serve PID NAME LOGFILE STOPFUNC
#
# Keep the server running until ctrl+c, and notice when it terminates on its
# own instead of waiting forever. Returns 0 for a requested shutdown and 1 if
# the server went away by itself.
interactive_serve()
{
    local pid="$1"
    local rc

    INTERACTIVE_SERVER_NAME="$2"
    INTERACTIVE_SERVER_LOG="$3"
    INTERACTIVE_STOP_FUNC="$4"

    INTERACTIVE_SERVING=1

    while [ "$INTERACTIVE_STOP" -eq 0 ]; do
        # "wait" for a background child returns as soon as a signal with a
        # handler arrives, after running that handler, so this costs nothing
        # while still reacting to ctrl+c immediately. A sleep loop would delay
        # the handler until the sleep is over.
        wait "$pid" 2>/dev/null
        rc=$?

        # ctrl+c or terminate: the handler has set the flag already
        [ "$INTERACTIVE_STOP" -ne 0 ] && break

        # some other handled signal, and the server is still there
        if [ $rc -gt 128 ] && kill -0 "$pid" 2>/dev/null; then
            continue
        fi

        # the server terminated on its own
        INTERACTIVE_SERVING=0
        INTERACTIVE_STOP_FUNC=""    # nothing left to stop
        INTERACTIVE_RC=1

        echo >&2
        echo "ERROR: $INTERACTIVE_SERVER_NAME terminated on its own (exit code $rc)." >&2
        echo "       Last lines of $INTERACTIVE_SERVER_LOG:" >&2
        tap_diag_file "$INTERACTIVE_SERVER_LOG"

        return 1
    done

    INTERACTIVE_SERVING=0

    return 0
}

################################ Addresses ##################################

interactive_addr_is_wildcard()
{
    case "$1" in
        ""|0.0.0.0|::|"[::]"|"*") return 0 ;;
    esac

    return 1
}

# Echo a local address that other hosts can use to reach this host, or nothing
# if none could be determined.
interactive_detect_addr()
{
    local addr=""

    if command -v ip > /dev/null 2>&1; then
        # The source address the kernel would pick to reach the outside world.
        # This sends nothing, and unlike just taking the first address of the
        # first interface it follows the routing table, which is what gets it
        # right on hosts with alias addresses or container bridges.
        addr="$(ip -4 route get 1.1.1.1 2>/dev/null | \
            sed -n 's/^.* src \([0-9.]\{1,\}\).*$/\1/p' | head -1)"

        # no route to the outside, e.g. an isolated lab network
        if [ -z "$addr" ]; then
            addr="$(ip -4 -o addr show scope global up 2>/dev/null | \
                awk '{print $4}' | cut -d/ -f1 | head -1)"
        fi
    fi

    if [ -z "$addr" ]; then
        addr="$(hostname -I 2>/dev/null | tr ' ' '\n' | \
            grep -E '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$' | grep -v '^127\.' | head -1)"
    fi

    echo "$addr"
}

# interactive_resolve_connect_addr LISTEN_ADDR EXPLICIT_ADDR
#
# Sets INTERACTIVE_CONNECT_ADDR to an address that clients can connect to. A
# wildcard listen address works for binding, but it is not a usable destination
# for a client config or an endpoint url, so a concrete one is derived.
interactive_resolve_connect_addr()
{
    local listen_addr="$1"
    local explicit_addr="$2"

    if [ -n "$explicit_addr" ]; then
        INTERACTIVE_CONNECT_ADDR="$explicit_addr"
        return 0
    fi

    if ! interactive_addr_is_wildcard "$listen_addr"; then
        INTERACTIVE_CONNECT_ADDR="$listen_addr"
        return 0
    fi

    INTERACTIVE_CONNECT_ADDR="$(interactive_detect_addr)"

    if [ -z "$INTERACTIVE_CONNECT_ADDR" ]; then
        INTERACTIVE_CONNECT_ADDR="127.0.0.1"
        echo "WARNING: Unable to determine a non-loopback address of this host, so the" >&2
        echo "         commands below only work locally. Use \"-c ADDR\" to set the" >&2
        echo "         address that clients should connect to." >&2
    fi

    return 0
}

# interactive_warn_if_exposed LISTEN_ADDR WHAT
interactive_warn_if_exposed()
{
    local listen_addr="$1"
    local what="$2"

    case "$listen_addr" in
        127.0.0.1|localhost|::1) return 0 ;;
    esac

    echo "WARNING: This server is reachable from other hosts and $what" >&2
    echo "         Only use it on a trusted network, and prefer \"-a 127.0.0.1\"" >&2
    echo "         when you only need it locally." >&2
}

################################## Misc #####################################

# interactive_require_free_port PORT WHAT
interactive_require_free_port()
{
    if ! port_in_use "$1"; then
        return 0
    fi

    echo "ERROR: TCP port $1 ($2) is already in use." >&2
    echo "       Use \"-p PORT\" for a different port, or find out what is using it:" >&2
    echo "         ss -ltnp | grep ':$1'" >&2
    exit 1
}

# Accept long option spellings in addition to the short getopts ones. Sets
# INTERACTIVE_ARGV, which the caller passes on to its own parse_args.
# Usage: interactive_translate_long_args "$@" ; parse_args "${INTERACTIVE_ARGV[@]}"
interactive_translate_long_args()
{
    local arg

    INTERACTIVE_ARGV=()

    for arg in "$@"; do
        case "$arg" in
        --clean)         INTERACTIVE_ARGV+=(-C) ;;
        --help)          INTERACTIVE_ARGV+=(-h) ;;
        --listen-addr)   INTERACTIVE_ARGV+=(-a) ;;
        --listen-addr=*) INTERACTIVE_ARGV+=(-a "${arg#*=}") ;;
        --connect-addr)  INTERACTIVE_ARGV+=(-c) ;;
        --connect-addr=*) INTERACTIVE_ARGV+=(-c "${arg#*=}") ;;
        --port)          INTERACTIVE_ARGV+=(-p) ;;
        --port=*)        INTERACTIVE_ARGV+=(-p "${arg#*=}") ;;
        --name)          INTERACTIVE_ARGV+=(-n) ;;
        --name=*)        INTERACTIVE_ARGV+=(-n "${arg#*=}") ;;
        --tmp-dir)       INTERACTIVE_ARGV+=(-T) ;;
        --tmp-dir=*)     INTERACTIVE_ARGV+=(-T "${arg#*=}") ;;
        *)               INTERACTIVE_ARGV+=("$arg") ;;
        esac
    done
}
