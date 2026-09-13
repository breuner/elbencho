#!/bin/bash
#
# Run the elbencho black-box test suite through the "prove" TAP harness.
#
# Test scripts are plain bash and live in group subdirs below tests/. This
# wrapper checks the prerequisites, prepares the dir for temporary files and
# hands the selected test scripts or dirs over to "prove".

SCRIPT_PATH="$(dirname "$(readlink -f "$0")")"
REPO_PATH="$(cd "$SCRIPT_PATH/.." && pwd)"

# Dirs containing test case scripts are prefixed, so that they can be told apart
# from the dirs holding supporting files ("lib", "tmp", "tools").
GROUP_DIR_PREFIX="tests_"

EXE_PATH="$REPO_PATH/bin/elbencho"    # user-definable via "-e"
TMP_PATH="$SCRIPT_PATH/tmp"           # user-definable via "-T"
NUM_JOBS=1                            # user-definable via "-j"
KEEP_FAILED=0                         # user-definable via "-k"
SHOW_COMMANDS=0                       # user-definable via "-c"
RUN_S3_TESTS=0                        # user-definable via "-s" or "-a"
RUN_SPDK_TESTS=0                      # user-definable via "-p" or "-a"
CMD_TIMEOUT=60                        # user-definable via "-w"
SCRIPT_TIMEOUT=300                    # user-definable via "-W"
VERBOSE=0                             # user-definable via "-v"

TARGETS=()

usage()
{
    echo "About:"
    echo "  Run the elbencho black-box test suite via the \"prove\" TAP harness."
    echo "  Without arguments, all posix and distributed mode tests are run."
    echo
    echo "Usage:"
    echo "  $ $(basename "$0") [OPTIONS] [TEST_SCRIPT_OR_DIR]..."
    echo
    echo "Optional Arguments:"
    echo "  -e PATH   elbencho binary to test."
    echo "            (Default: $REPO_PATH/bin/elbencho)"
    echo "  -T DIR    Parent dir for temporary files of the test cases."
    echo "            (Default: $SCRIPT_PATH/tmp)"
    echo "  -j NUM    Number of test scripts to run in parallel. (Default: 1)"
    echo "  -k        Keep the temporary dir of failed test cases for inspection."
    echo "            (Default: temporary dirs are always removed.)"
    echo "  -c        Write the commands that the tests execute and the console output"
    echo "            of these commands to one transcript file per test script in the"
    echo "            \"commands\" subdir of the dir for temporary files."
    echo "  -s        Also run the S3 tests. The minio S3 server gets downloaded"
    echo "            into the temporary dir if it does not exist there yet."
    echo "  -p        Also run the SPDK tests. Each of these starts its own private"
    echo "            SPDK NVMe-oF target and requires python3 for spdk's rpc.py."
    echo "  -a        Run all test groups."
    echo "  -w SECS   Timeout for a single elbencho or aws invocation, after which"
    echo "            it gets killed and the test fails. (Default: $CMD_TIMEOUT)"
    echo "  -W SECS   Timeout for a complete test script. (Default: $SCRIPT_TIMEOUT)"
    echo "  -v        Verbose output, i.e. show the result of each single check."
    echo "  -h        Print this help."
    echo
    echo "Examples:"
    echo "  Run the default set of tests:"
    echo "    $ $(basename "$0")"
    echo
    echo "  Run everything, including S3 tests, 4 test scripts in parallel:"
    echo "    $ $(basename "$0") -a -j 4"
    echo
    echo "  Run a single test script and keep its files if it fails:"
    echo "    $ $(basename "$0") -k tests/tests_posix/smallfiles-dirmode.t"
    echo
    echo "  Run a single test script and see which commands it executes:"
    echo "    $ $(basename "$0") -c tests/tests_posix/smallfiles-dirmode.t"

    exit 1
}

parse_args()
{
    local OPTIND # local to prevent effects from other subscripts

    while getopts ":ace:hj:kpsT:vw:W:" opt; do
        case "${opt}" in
        a)
            RUN_S3_TESTS=1
            RUN_SPDK_TESTS=1
            ;;
        c)
            SHOW_COMMANDS=1
            ;;
        e)
            EXE_PATH="${OPTARG}"
            ;;
        j)
            NUM_JOBS="${OPTARG}"
            ;;
        k)
            KEEP_FAILED=1
            ;;
        p)
            RUN_SPDK_TESTS=1
            ;;
        s)
            RUN_S3_TESTS=1
            ;;
        T)
            TMP_PATH="${OPTARG}"
            ;;
        v)
            VERBOSE=1
            ;;
        w)
            CMD_TIMEOUT="${OPTARG}"
            ;;
        W)
            SCRIPT_TIMEOUT="${OPTARG}"
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

    TARGETS=("$@")
}

check_prereqs_or_exit()
{
    if [ ! -f "$EXE_PATH" ]; then
        echo "ERROR: elbencho binary not found: $EXE_PATH" >&2
        echo "       Run \"make\" first or use \"-e\" to select a different binary." >&2
        exit 1
    fi

    if [ ! -x "$EXE_PATH" ]; then
        echo "ERROR: elbencho binary is not executable: $EXE_PATH" >&2
        exit 1
    fi

    local cmd
    for cmd in prove jq timeout; do
        if ! command -v "$cmd" >/dev/null 2>&1; then
            echo "ERROR: Required command not found in PATH: $cmd" >&2
            exit 1
        fi
    done

    # spdk's rpc.py is a python script, so the SPDK tests cannot run without it
    if [ $RUN_SPDK_TESTS -ne 0 ] && ! command -v python3 >/dev/null 2>&1; then
        echo "ERROR: The python3 interpreter is required for the SPDK tests" >&2
        echo "       (spdk's rpc.py), but was not found in PATH." >&2
        exit 1
    fi

    mkdir -p "$TMP_PATH"
    if [ $? -ne 0 ]; then
        echo "ERROR: Unable to create dir for temporary files: $TMP_PATH" >&2
        exit 1
    fi
}

# Make sure the prerequisites for the S3 tests are in place. The minio server
# lives in the temp dir parent, so it is downloaded only once and gets removed
# together with everything else by a plain "rm -rf tests/tmp".
prepare_minio_or_exit()
{
    if ! command -v aws >/dev/null 2>&1; then
        echo "ERROR: The aws cli tool is required for the S3 tests, but was not" >&2
        echo "       found in PATH." >&2
        exit 1
    fi

    # the download itself lives in the library, so that the interactive tools in
    # tests/tools/ can use the very same code
    source "$SCRIPT_PATH/lib/minio.sh" || exit 1

    minio_ensure_binary "$TMP_PATH/minio"
    if [ $? -ne 0 ]; then
        exit 1
    fi
}

# Add a test group dir to the target list, unless it contains no test scripts.
# The given name is the short group name, the dir name gets the prefix added.
add_group()
{
    local group="$1"
    local groupdir="$GROUP_DIR_PREFIX$group"

    if [ ! -d "$SCRIPT_PATH/$groupdir" ]; then
        return 0
    fi

    if [ -z "$(find "$SCRIPT_PATH/$groupdir" -name '*.t' -print -quit)" ]; then
        echo "NOTE: Test group dir \"$groupdir\" contains no test scripts yet, skipping it."
        return 0
    fi

    TARGETS+=("tests/$groupdir")
}

select_default_targets()
{
    add_group posix
    add_group distributed
    add_group netbench

    if [ $RUN_S3_TESTS -ne 0 ]; then
        add_group s3
    fi

    if [ $RUN_SPDK_TESTS -ne 0 ]; then
        add_group spdk
    fi

    if [ ${#TARGETS[@]} -eq 0 ]; then
        echo "ERROR: No test scripts found to run." >&2
        exit 1
    fi
}

run_prove()
{
    local prove_opts=(--exec /bin/bash --recurse)

    if [ "$NUM_JOBS" -gt 1 ] 2>/dev/null; then
        prove_opts+=(--jobs "$NUM_JOBS")
    fi

    if [ $VERBOSE -ne 0 ]; then
        prove_opts+=(--verbose)
    fi

    echo "elbencho binary: $EXE_PATH"
    echo "Temporary files: $TMP_PATH"
    if [ $SHOW_COMMANDS -ne 0 ]; then
        echo "Command log:     $ELBENCHO_TEST_TRACE_DIR/<test script>.log"
    fi
    echo

    exec prove "${prove_opts[@]}" "${TARGETS[@]}"
}

################################ Main ######################################

parse_args "$@"

check_prereqs_or_exit

if [ $RUN_S3_TESTS -ne 0 ]; then
    prepare_minio_or_exit
fi

EXE_PATH="$(readlink -f "$EXE_PATH")"
TMP_PATH="$(readlink -f "$TMP_PATH")"

# Test scripts need the absolute paths, e.g. because "--opslog" requires one.
export ELBENCHO_TEST_BIN="$EXE_PATH"
export ELBENCHO_TEST_TMP="$TMP_PATH"
export ELBENCHO_TEST_KEEP="$KEEP_FAILED"
export ELBENCHO_TEST_CMD_TIMEOUT="$CMD_TIMEOUT"
export ELBENCHO_TEST_SCRIPT_TIMEOUT="$SCRIPT_TIMEOUT"
export ELBENCHO_TEST_LIB="$SCRIPT_PATH/lib"
export ELBENCHO_TEST_MINIO="$TMP_PATH/minio"

# Exported in any case, so that a leftover value in the user's environment
# cannot silently enable the command transcripts.
export ELBENCHO_TEST_TRACE="$SHOW_COMMANDS"
export ELBENCHO_TEST_TRACE_DIR="$TMP_PATH/commands"

if [ $SHOW_COMMANDS -ne 0 ]; then
    # Starting from an empty dir, because the transcript of a test script that
    # is not part of the current run cannot be told apart from a fresh one.
    # Each test script then creates its own file when it sources the library.
    rm -rf "$ELBENCHO_TEST_TRACE_DIR"
    mkdir -p "$ELBENCHO_TEST_TRACE_DIR"
    if [ $? -ne 0 ]; then
        echo "ERROR: Unable to create dir for the command transcripts:" >&2
        echo "       $ELBENCHO_TEST_TRACE_DIR" >&2
        exit 1
    fi
fi

# SPDK tools, built together with elbencho when SPDK_SUPPORT=1 is used. These
# can be overridden to test against a different spdk build.
: ${ELBENCHO_TEST_NVMF_TGT:="$REPO_PATH/external/spdk/build/bin/nvmf_tgt"}
: ${ELBENCHO_TEST_SPDK_RPC:="$REPO_PATH/external/spdk/scripts/rpc.py"}
export ELBENCHO_TEST_NVMF_TGT
export ELBENCHO_TEST_SPDK_RPC

if [ ${#TARGETS[@]} -eq 0 ]; then
    # No test scripts or dirs given, so run the default groups. Paths are
    # relative to the repository root to keep the prove output readable.
    cd "$REPO_PATH" || exit 1
    select_default_targets
fi

run_prove
