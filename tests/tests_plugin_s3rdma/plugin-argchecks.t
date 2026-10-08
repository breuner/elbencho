#!/bin/bash
#
# Runtime activation of an s3rdma plugin and its argument checks.
#
# A compiled-in plugin stays inactive until "--plugins" names it, and an active
# plugin rejects the options that its RDMA path cannot serve. All of this is
# decided while the arguments are checked, so the runs below have to fail before
# any request is sent and need neither an S3 server nor RDMA hardware. The checks
# of the second half depend on the plugin that the binary was built with.

source "$(dirname "$(readlink -f "$0")")/../lib/testlib.sh" || exit 1
source "$ELBENCHO_TEST_LIB/minio.sh" || exit 1
source "$ELBENCHO_TEST_LIB/plugin_s3rdma.sh" || exit 1

require_build_feature s3

PLUGIN="${ELBENCHO_TEST_S3RDMA_PLUGIN:-$(s3rdma_detect_plugin)}"

if [ -z "$PLUGIN" ]; then
    tap_skip_all "elbencho was built without an S3-over-RDMA plugin (ELB_PLUGIN_S3RDMA_MINIO=1 or ELB_PLUGIN_S3RDMA_CLOUDIAN=1)"
fi

require_plugin "$PLUGIN"

case "$PLUGIN" in
    s3rdma_minio) PLUGIN_CHECKS=8 ;;
    s3rdma_cloudian) PLUGIN_CHECKS=6 ;;
    *) tap_skip_all "no argument checks known for plugin \"$PLUGIN\"" ;;
esac

test_init
tap_plan $((7 + PLUGIN_CHECKS))

# Nothing listens here: a run that gets past the argument checks fails
# differently, which the output checks below would show. The expected failures
# are run with their output discarded, so that it does not show up as diagnostics.
S3_ARGS=( --s3endpoints http://127.0.0.1:1 --s3key k --s3secret s )
S3_TARGET="s3://plugin-argchecks"
POSIX_DIR="$TEST_DIR/data"

mkdir -p "$POSIX_DIR"

################## Plugin selection ##################

assert_match "$("$ELBENCHO_TEST_BIN" --version)" "Included plugins: .*\b$PLUGIN\b" \
    "\"--version\" lists plugin $PLUGIN"
assert_match "$("$ELBENCHO_TEST_BIN" --help-all)" "compiled into this executable: .*\b$PLUGIN\b" \
    "\"--help-all\" names plugin $PLUGIN in the description of \"--plugins\""

run_elbencho unknownplugin --plugins doesnotexist -d -w -t 1 -s 4k -b 4k "$POSIX_DIR" \
    > /dev/null 2>&1
assert_nok $? "an unknown plugin name is rejected"
assert_match "$(cat "$ELB_OUT")" "Plugin not available in this .*build: doesnotexist.*Available plugins: .*\b$PLUGIN\b" \
    "the error names the unknown plugin and lists the available ones"

run_elbencho posixactive --plugins "$PLUGIN" -d -w -t 1 -s 4k -b 4k "$POSIX_DIR" \
    > /dev/null 2>&1
assert_nok $? "the active plugin rejects a run against a file system path"
assert_match "$(cat "$ELB_OUT")" "Plugin \"--plugins $PLUGIN\" can only be used with S3" \
    "the error says that the plugin is for S3 only"

run_elbencho posixinactive -d -w -t 1 -s 4k -b 4k "$POSIX_DIR"
assert_ok $? "the same run succeeds while the plugin is not activated"

################## Checks of the active plugin ##################

# argcheck_fails TAG REGEX DESC ARGS...
#
# Runs elbencho with the plugin active and the given extra arguments against the
# S3 target and checks that it fails with an error matching REGEX.
argcheck_fails()
{
    local tag="$1"
    local regex="$2"
    local desc="$3"
    shift 3

    run_elbencho "$tag" "${S3_ARGS[@]}" --plugins "$PLUGIN" -w -t 1 "$@" "$S3_TARGET" \
        > /dev/null 2>&1
    assert_nok $? "$desc"
    assert_match "$(cat "$ELB_OUT")" "$regex" "the error message of \"$tag\" explains the rejection"
}

case "$PLUGIN" in
    s3rdma_minio)
        argcheck_fails iodepth 'requires "--iodepth=1"' \
            "an I/O depth above 1 is rejected" -s 4m -b 4m --iodepth 2
        argcheck_fails blocksize 'at most 4095 MiB' \
            "a block size above 4095 MiB is rejected" -s 8g -b 4g
        argcheck_fails fastget 'cannot be used together with "--s3fastget"' \
            "\"--s3fastget\" is rejected" -s 4m -b 4m --s3fastget
        argcheck_fails checksum 'does not support inline ACLs, server-side encryption or checksum' \
            "a checksum algorithm is rejected" -s 4m -b 4m --s3chksumalgo CRC32
        ;;
    s3rdma_cloudian)
        # (the GPU options only exist in a build with CUDA and cuFile support)
        if has_build_feature cuda && has_build_feature cufile/gds; then
            argcheck_fails cufile 'requires "--gdsbufreg"' \
                "\"--cufile\" without \"--gdsbufreg\" is rejected" \
                -s 4m -b 4m --cufile --gpuids 0
            argcheck_fails bufreg 'cannot be used together with "--gdsbufreg"' \
                "\"--cuobjhostbufreg\" together with \"--gdsbufreg\" is rejected" \
                -s 4m -b 4m --cuobjhostbufreg --gdsbufreg
        else
            tap_skip "\"--cufile\" without \"--gdsbufreg\" is rejected"
            tap_skip "the error message of \"cufile\" explains the rejection"
            tap_skip "\"--cuobjhostbufreg\" together with \"--gdsbufreg\" is rejected"
            tap_skip "the error message of \"bufreg\" explains the rejection"
        fi

        # not plugin-specific options, but a build with this plugin has no HTTP body downloads
        run_elbencho fastget "${S3_ARGS[@]}" -r -t 1 -s 4m -b 4m --s3fastget "$S3_TARGET" \
            > /dev/null 2>&1
        assert_nok $? "\"--s3fastget\" is rejected also while the plugin is inactive"
        assert_match "$(cat "$ELB_OUT")" "not available in a build with plugin $PLUGIN" \
            "the error names the plugin as the reason"
        ;;
esac
