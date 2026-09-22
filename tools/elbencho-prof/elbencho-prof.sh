#!/bin/bash

# ==============================================================================
# Profile-Driven Wrapper For Elbencho
# ==============================================================================

# Default configurations
HOSTS=""
TARGET_DIR=""
S3_MODE=false
PREPARE_SIZE="auto"
PREP_GIVEN=false
AUTO_SIZE="16g"
AUTO_TIME="60s"
NO_PREP_WRITE=false
FILE_SIZE="1g"
TEST_PROFILES=()
TEST_PROFILES_GIVEN=false
DURATION="60"
TIME_GIVEN=false
NO_LOOP=false
THREADS=""
BLOCK_SIZE=""
IODEPTH=""
RAND_ACCESS=""
DIRECT_IO=""
MEASURE_LATENCY=false
ELBENCHO=""
AWS_CLI="aws"
RESULTS_DIR=""
CUSTOM_ARGS=()
HOST_ARGS=()
NUM_HOSTS=0
STARTUP_CONCURRENCY=32
STOP_SERVICES=false
VERBOSE=false
DRYRUN=false
TOTAL_RUNS=0

SCRIPT_NAME=$(basename "$0")
SCRIPT_DIR=$(dirname "$0")
ARTIFACTS_DIR="$SCRIPT_DIR/artifacts"
PROFILES_DIR="$SCRIPT_DIR/profiles"

# Print usage
usage() 
{
    local show_adv=${1:-false}

    echo "About:"
    echo "  Profile-driven elbencho wrapper for coordinated distributed tests."
    echo ""
    echo "Prereqs:"
    echo "  * Passwordless ssh to the given hosts."
    echo "  * If elbencho is not preinstalled on the hosts then:"
    echo "    * This script will try to download elbencho from GitHub."
    echo "    * This script needs to run from a shared file system to have the executable"
    echo "      available on all hosts."
    echo "  * For S3 mode, set AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY,"
    echo "    AWS_ENDPOINT_URL_S3 environment variables."
    echo ""
    echo "Usage:"
    echo "  $SCRIPT_NAME [--dir /some/benchdir] [--dir s3://bucket/prefix] [host1] [hostN...] [options]"
    echo ""
    echo "Hosts:"
    echo "  [hosts...]          Hostnames, bracket ranges (\"node[01,03-05]\"), or"
    echo "                      path to a hosts file for distributed runs. Requires"
    echo "                      passwordless ssh for elbencho service start."
    echo ""
    echo "Options:"
    echo "  --dir <dir>         Target directory for the benchmark or S3 bucket path."
    echo "                      Example: /mnt/storage or s3://mybucket/bench"
    echo "                      (Required unless --stop is used)"
    echo "  --prep <size>       Lay out a dataset of the specified total size"
    echo "                      (e.g., 100G, 1T) or 'auto'. (Default: auto)"
    echo "  --test <profiles>   Run standardized test profiles against the dataset."
    echo "                      Can be a comma-separated list, use wildcards (*, ?),"
    echo "                      or be specified multiple times."
    echo "                      <profile> must match config file(s) in the"
    echo "                      'profiles/' subdirectory. (Default: file-read-bw,"
    echo "                      file-read-iops or s3-* equivalents in S3 mode)"
    echo "  --time <duration>   Max duration for test phases. Workload runs in a"
    echo "                      loop until this time limit is reached."
    echo "                      (Examples: 90s, 5m, 1h. Default: 1m.)"
    echo "                      Hint: Use --noloop to run the workload only once"
    echo "                      instead of looping until this time limit."
    echo "  --threads <num>     Number of threads per host (Optional)."
    echo "                      (Default: defined in respective profile)"
    echo "  --stop              Stop the running elbencho services on the"
    echo "                      specified hosts (can be used alone or at the"
    echo "                      end of a run)."

    echo ""
    echo "Advanced Options:"

    if [[ "$show_adv" == true ]]; then
        echo "  --                  Elbencho passthrough: Everything after this flag"
        echo "                      is passed through as argument to elbencho."
        echo "  --autosize <size>   Initial dataset size for automatic preparation mode."
        echo "                      (Default: 16G)"
        echo "  --autotime <dur>    Minimum target elapsed time for automatic preparation."
        echo "                      Dataset size gets doubled until target is reached."
        echo "                      (Examples: 90s, 5m, 1h. Default: 1m.)"
        echo "  --aws <path>        Path to the aws cli binary (default: 'aws' from PATH)."
        echo "  --block <size>      Block size for IO operations (e.g., 4k, 1m). Applies"
        echo "                      to both prepare and test phases."
        echo "  --direct <1|0>      Enable (1) or disable (0) direct IO for both prepare"
        echo "                      and test phases."
        echo "  --dryrun            Show the command that would be run, but do not"
        echo "                      execute it."
        echo "  --elbencho <path>   Path to the elbencho binary to use locally and"
        echo "                      on all hosts (default: auto-download to"
        echo "                      artifacts/ if not already installed)."
        echo "  --filesize <size>   Size of each individual file/object in the dataset."
        echo "                      (Default: 1G)"
        echo "  --iodepth <int>     Set IO depth for asynchronous IO for both prepare"
        echo "                      and test phases."
        echo "  --lat               Enable latency measurements for both prepare and"
        echo "                      test phases."
        echo "  --noloop            Run workload only once per test phase instead of"
        echo "                      looping until the --time limit is reached."
        echo "  --noprepwrite       Generate treefile based on --prep size but skip"
        echo "                      the sequential laying out of data."
        echo "  --rand <1|0>        Enable (1) or disable (0) random access for both"
        echo "                      prepare and test phases."
        echo "  --resdir <path>     Directory to store result files (results.txt,"
        echo "                      results.csv, results.json). Default is"
        echo "                      results/YYYYMMDD/ under the script directory."
        echo "  --verbose           Show the full elbencho benchmarking command"
        echo "                      before executing it."
    else
        echo "  --help-adv          Show advanced options (e.g., custom binary"
        echo "                      path, verbose, dryrun)."
    fi


    echo ""
    echo "Examples:"
    echo "  Prepare a 1TiB dataset and then run all file read and all file write profiles"
    echo "  from the given hosts for max 30 seconds per test profile:"
    echo "    $ $SCRIPT_NAME node[01-03,05] --dir /mnt/storage --test file-read-* \\"
    echo "      --prep 1T --time 30s"
    echo ""
    echo "  Automatically size a dataset and run the default file tests against it from"
    echo "  the hosts in the given file:"
    echo "    $ $SCRIPT_NAME ./myhosts.txt --dir /mnt/storage"
    echo ""
    echo "  Automatically re-size dataset for changed number of hosts:"
    echo "    $ $SCRIPT_NAME ./myhosts.txt --dir /mnt/storage --prep auto"
    echo ""

    exit 1
}

# If no parameters are given, show usage and exit
if [[ "$#" -eq 0 ]]; then
    usage false
fi

# Parse arguments
while [[ "$#" -gt 0 ]]; do
    case "$1" in
        --dir) TARGET_DIR="$2"; shift 2 ;;
        --prep) PREPARE_SIZE="$2"; PREP_GIVEN=true; shift 2 ;;
        --autosize) AUTO_SIZE="$2"; shift 2 ;;
        --autotime) AUTO_TIME="$2"; shift 2 ;;
        --noprepwrite) NO_PREP_WRITE=true; shift 1 ;;
        --filesize) FILE_SIZE="$2"; shift 2 ;;
        --test) 
            IFS=',' read -ra PROFS <<< "$2"
            TEST_PROFILES+=("${PROFS[@]}")
            TEST_PROFILES_GIVEN=true
            shift 2 
            ;;
        --time)
            # Use awk to parse duration suffix (s, m, h) and convert to seconds
            DURATION=$(echo "$2" | awk '{val=$1+0; if ($1 ~ /m$/) val*=60; else if ($1 ~ /h$/) val*=3600; print val}')
            TIME_GIVEN=true
            shift 2
            ;;
        --noloop) NO_LOOP=true; shift 1 ;;
        --threads) THREADS="$2"; shift 2 ;;
        --block) BLOCK_SIZE="$2"; shift 2 ;;
        --iodepth) IODEPTH="$2"; shift 2 ;;
        --lat) MEASURE_LATENCY=true; shift 1 ;;
        --rand) RAND_ACCESS="$2"; shift 2 ;;
        --direct) DIRECT_IO="$2"; shift 2 ;;
        --stop) STOP_SERVICES=true; shift 1 ;;
        --elbencho) ELBENCHO="$2"; shift 2 ;;
        --aws) AWS_CLI="$2"; shift 2 ;;
        --resdir) RESULTS_DIR="$2"; shift 2 ;;
        --verbose) VERBOSE=true; shift 1 ;;
        --dryrun) DRYRUN=true; shift 1 ;;
        -h|--help) usage false ;;
        --help-adv) usage true ;;
        --) 
            shift
            CUSTOM_ARGS=("$@") # Store remaining args in an array
            break 
            ;;
        -*) 
            echo "Unknown option passed: $1"
            usage false
            ;;
        *) 
            # Collect any non-option arguments as hosts/hostfiles
            HOST_ARGS+=("$1")
            shift 1
            ;;
    esac
done

# Strip trailing slash from TARGET_DIR for consistency
if [[ "$TARGET_DIR" == */ ]]; then
    TARGET_DIR="${TARGET_DIR%/}"
fi

# Detect S3 Mode
if [[ "$TARGET_DIR" == s3://* ]]; then
    S3_MODE=true
fi

# ==============================================================================
# Helper Functions
# ==============================================================================

log_msg() 
{
    local level=$1
    shift
    local msg=""
    local dest=1 # 1 for stdout, 2 for stderr
    
    case "$level" in
        0) msg="[ERROR] $*"; dest=2 ;;
        1) msg="[WARN] $*"; dest=2 ;;
        2) msg="[INFO] $*" ;;
        3) 
            if [[ "$VERBOSE" == true ]]; then
                msg="[VERBOSE] $*"
            fi
            ;;
    esac

    if [[ -n "$msg" ]]; then
        if [[ "$dest" -eq 2 ]]; then
            echo "$msg" >&2
        else
            echo "$msg"
        fi
        
        if [[ "$DRYRUN" == false && -n "$RESULTS_DIR" && -d "$RESULTS_DIR" ]]; then
            echo "$msg" >> "$RESULTS_DIR/results.txt"
        fi
    fi
}

# AWS CLI wrapper to inject endpoint url if required
aws_cmd() 
{
    local ep_url="${AWS_ENDPOINT_URL:-$AWS_ENDPOINT_URL_S3}"
    local cmd=("$AWS_CLI" --no-verify-ssl)
    
    if [[ -n "$ep_url" ]]; then
        local expanded_eps
        expanded_eps=$(expand_hosts "$ep_url")
        IFS=',' read -ra EP_ARR <<< "$expanded_eps"
        local rand_idx=$(( RANDOM % ${#EP_ARR[@]} ))
        local selected_ep="${EP_ARR[$rand_idx]}"
        cmd+=(--endpoint-url "$selected_ep")
    fi
    
    cmd+=("$@")
    
    if [[ "$VERBOSE" == true || "$DRYRUN" == true ]]; then
        echo -n "[CMD] " >&2
        printf '%q ' "${cmd[@]}" >&2
        echo >&2
    fi
    
    "${cmd[@]}"
}

log_s3_info()
{
    local ep_url="${AWS_ENDPOINT_URL:-$AWS_ENDPOINT_URL_S3}"
    if [[ -n "$ep_url" ]]; then
        local protocol="https"
        if [[ "$ep_url" == *"http://"* ]]; then
            protocol="http"
        fi
        local expanded_eps
        expanded_eps=$(expand_hosts "$ep_url")
        local num_eps
        num_eps=$(echo "$expanded_eps" | tr ',' '\n' | wc -l | awk '{print $1}')
        log_msg 2 "Number of S3 endpoints after expansion: $num_eps (Protocol: $protocol)"
    else
        log_msg 2 "The AWS_ENDPOINT_URL_S3 environment variable is empty. You can set this to specify custom S3 endpoints."
    fi

    if [[ -z "$AWS_ACCESS_KEY_ID" ]]; then
        log_msg 2 "The AWS_ACCESS_KEY_ID environment variable is empty. You can set this for S3 authentication."
    fi

    if [[ -z "$AWS_SECRET_ACCESS_KEY" ]]; then
        log_msg 2 "The AWS_SECRET_ACCESS_KEY environment variable is empty. You can set this for S3 authentication."
    fi
}

ensure_s3_bucket_exists()
{
    if ! command -v "$AWS_CLI" >/dev/null 2>&1; then
        log_msg 0 "AWS CLI ('$AWS_CLI') is required for S3 mode but not found in PATH or not executable."
        log_msg 0 "  Please install the AWS CLI or specify its path using --aws."
        exit 1
    fi
    
    S3_PATH_NO_SCHEME="${TARGET_DIR#s3://}"
    S3_BUCKET="${S3_PATH_NO_SCHEME%%/*}"
    
    local s3_err_file
    s3_err_file=$(mktemp)
    
    aws_cmd s3 ls "s3://$S3_BUCKET" >/dev/null 2>"$s3_err_file"
    local ls_status=$?
    
    local aws_ls_err
    aws_ls_err=$(cat "$s3_err_file")
    rm -f "$s3_err_file"
    
    if [[ "$VERBOSE" == true && -n "$aws_ls_err" ]]; then
        printf "%s\n" "$aws_ls_err" >&2
    fi
    
    if [[ $ls_status -ne 0 ]]; then
        # Check if the error indicates a missing bucket
        if [[ "$aws_ls_err" =~ NoSuchBucket ]] || [[ "$aws_ls_err" =~ "Not Found" ]] || \
            [[ "$aws_ls_err" =~ NotFound ]] || [[ "$aws_ls_err" =~ 404 ]]; then
            log_msg 2 "S3 bucket 's3://$S3_BUCKET' not found. Attempting to create it..."
            
            s3_err_file=$(mktemp)
            aws_cmd s3 mb "s3://$S3_BUCKET" >/dev/null 2>"$s3_err_file"
            local mb_status=$?
            
            local aws_mb_err
            aws_mb_err=$(cat "$s3_err_file")
            rm -f "$s3_err_file"
            
            if [[ "$VERBOSE" == true && -n "$aws_mb_err" ]]; then
                printf "%s\n" "$aws_mb_err" >&2
            fi
            
            if [[ $mb_status -ne 0 ]]; then
                log_msg 0 "Failed to create S3 bucket 's3://$S3_BUCKET'."
                if [[ "$VERBOSE" == false ]]; then
                    log_msg 0 "  AWS Error: $aws_mb_err"
                fi
                log_msg 0 "  Hint: Verify that AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY," \
                    "and AWS_ENDPOINT_URL_S3 are exported."
                exit 1
            fi
            log_msg 2 "Successfully created S3 bucket 's3://$S3_BUCKET'."
        else
            log_msg 0 "Failed to communicate with S3 endpoint."
            if [[ "$VERBOSE" == false ]]; then
                log_msg 0 "  AWS Error: $aws_ls_err"
            fi
            log_msg 0 "  Hint: Verify your network connection, endpoint URL (AWS_ENDPOINT_URL_S3)," \
                "and credentials (AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY) env vars."
            exit 1
        fi
    fi
}

ensure_target_dir_exists()
{
    if [[ ! -d "$TARGET_DIR" ]]; then
        log_msg 2 "Target directory '$TARGET_DIR' does not exist. Attempting to create it..."
        if ! mkdir -p "$TARGET_DIR" >/dev/null 2>&1; then
            log_msg 0 "Failed to create target directory '$TARGET_DIR'. Please check permissions."
            exit 1
        fi
        log_msg 2 "Successfully created target directory '$TARGET_DIR'."
    fi
}

validate_dataset_s3()
{
    local treefile="$1"
    local silent="${2:-false}"
    local check_failed=false
    local fail_reason=""
    local fail_file=""
    
    local s3_cache_file="$ARTIFACTS_DIR/s3_inventory.txt"
    rm -f "$s3_cache_file"
    
    log_msg 3 "Fetching S3 bucket inventory for dataset validation..."
    if [[ "$VERBOSE" == true ]]; then
        aws_cmd s3 ls --recursive "$TARGET_DIR/" > "$s3_cache_file"
    else
        aws_cmd s3 ls --recursive "$TARGET_DIR/" > "$s3_cache_file" 2>/dev/null
    fi
    
    local s3_path_no_scheme="${TARGET_DIR#s3://}"
    local bucket="${s3_path_no_scheme%%/*}"
    local prefix="${s3_path_no_scheme#*/}"
    if [[ "$prefix" == "$bucket" ]]; then
        prefix=""
    fi
    if [[ -n "$prefix" && "$prefix" != */ ]]; then
        prefix="${prefix}/"
    fi
    
    local awk_script='
    BEGIN { failed = 0 }
    NR==FNR {
        # Parse AWS S3 recursive list format: Date Time Size Key
        if (NF >= 4) {
            size = $3
            key = $4
            for(i=5; i<=NF; i++) key = key " " $i
            s3_sizes[key] = size
        }
        next
    }
    {
        # Parse treefile format: type target_sz filepath
        if ($1 != "f" && $1 != "F") next;
        target_sz = $2
        filepath = $3
        for(i=4; i<=NF; i++) filepath = filepath " " $i
        
        key = prefix filepath
        
        if (!(key in s3_sizes)) {
            print "MISSING|" filepath "|File is missing in S3 bucket."
            failed = 1
            exit 1
        }
        
        logical_sz = s3_sizes[key]
        
        if (logical_sz !~ /^[0-9]+$/) {
            print "ERROR|" filepath "|Could not determine file size via aws s3 ls."
            failed = 1
            exit 1
        }
        
        if (logical_sz + 0 < target_sz + 0) {
            print "TOOSMALL|" filepath "|File is too small (Expected >= " target_sz ", found " logical_sz ")."
            failed = 1
            exit 1
        }
    }
    '
    
    local awk_out
    awk_out=$(awk -v prefix="$prefix" "$awk_script" "$s3_cache_file" "$treefile")
    local awk_status=$?
    
    if [[ $awk_status -ne 0 || -n "$awk_out" ]]; then
        check_failed=true
        fail_file="$TARGET_DIR/$(echo "$awk_out" | awk -F'|' '{print $2}')"
        fail_reason=$(echo "$awk_out" | awk -F'|' '{print $3}')
    fi
    
    if [[ "$check_failed" == true ]]; then
        if [[ "$silent" == false ]]; then
            log_msg 0 "Dataset validation failed for file: $fail_file"
            log_msg 0 "  Reason: $fail_reason"
        fi
        return 1
    fi
    
    return 0
}

validate_dataset_posix()
{
    local treefile="$1"
    local silent="${2:-false}"
    local check_failed=false
    local fail_reason=""
    local fail_file=""
    
    while read -r type target_sz filepath; do
        if [[ "$type" != "f" && "$type" != "F" ]]; then
            continue
        fi
        
        local full_filepath="$TARGET_DIR/$filepath"
        
        # Native filesystem POSIX validation
        if [[ ! -f "$full_filepath" ]]; then
            check_failed=true
            fail_reason="File is missing."
            fail_file="$full_filepath"
            break
        fi
        
        # Use GNU stat to get logical size (%s), blocks (%b), and block size (%B)
        local stat_out
        stat_out=$(stat -c "%s %b %B" "$full_filepath" 2>/dev/null)
        if [[ $? -ne 0 || -z "$stat_out" ]]; then
            check_failed=true
            fail_reason="Could not stat file."
            fail_file="$full_filepath"
            break
        fi
        
        local logical_sz blocks block_sz
        read -r logical_sz blocks block_sz <<< "$stat_out"
        if [[ -z "$block_sz" ]]; then
            block_sz=512 # Fallback if %B is empty
        fi
        
        if [[ "$logical_sz" -lt "$target_sz" ]]; then
            check_failed=true
            fail_reason="File is too small (Expected >= $target_sz, found $logical_sz)."
            fail_file="$filepath"
            break
        fi
        
        local allocated_sz=$(( blocks * block_sz ))
        local threshold=$(( target_sz * 9 / 10 ))
        
        if [[ "$allocated_sz" -lt "$threshold" ]]; then
            check_failed=true
            fail_reason="File appears sparse or compressed (Allocated $allocated_sz is < 90% of expected $target_sz)."
            fail_file="$filepath"
            break
        fi
    done < "$treefile"
    
    if [[ "$check_failed" == true ]]; then
        if [[ "$silent" == false ]]; then
            log_msg 0 "Dataset validation failed for file: $fail_file"
            log_msg 0 "  Reason: $fail_reason"
        fi
        return 1
    fi
    
    return 0
}

validate_dataset()
{
    local treefile="$1"
    
    if [[ ! -f "$treefile" ]]; then
        return 1
    fi

    if [[ "$S3_MODE" == true ]]; then
        validate_dataset_s3 "$@"
    else
        validate_dataset_posix "$@"
    fi
}

check_auto_prep_host_increase()
{
    local auto_prep_hosts_file="$ARTIFACTS_DIR/auto_prep_hosts.txt"
    if [[ -f "$auto_prep_hosts_file" ]]; then
        local old_hosts
        old_hosts=$(cat "$auto_prep_hosts_file" 2>/dev/null)
        if [[ "$old_hosts" =~ ^[0-9]+$ ]] && [[ "$NUM_HOSTS" -gt 0 ]]; then
            # Use awk for accurate floating point comparison to bypass bash integer truncation
            local is_higher=$(awk -v old="$old_hosts" -v cur="$NUM_HOSTS" 'BEGIN { if (cur > old * 1.2) print 1; else print 0 }')
            if [[ "$is_higher" -eq 1 ]]; then
                log_msg 1 "Host count ($NUM_HOSTS) is >20% higher than during the last '--prep auto' run ($old_hosts)."
                log_msg 1 "  It is recommended to rerun with '--prep auto' to regenerate the dataset for the increased host count."
            fi
        fi
    fi
}

resolve_elbencho_binary()
{
    if [[ -z "$ELBENCHO" ]]; then
        if [[ -f "$ARTIFACTS_DIR/elbencho" && -x "$ARTIFACTS_DIR/elbencho" ]]; then
            ELBENCHO="$ARTIFACTS_DIR/elbencho"
        elif [[ -f "/usr/local/bin/elbencho" && -x "/usr/local/bin/elbencho" ]]; then
            ELBENCHO="/usr/local/bin/elbencho"
            log_msg 2 "Using system-installed elbencho at $ELBENCHO"
        elif [[ -f "/usr/bin/elbencho" && -x "/usr/bin/elbencho" ]]; then
            ELBENCHO="/usr/bin/elbencho"
            log_msg 2 "Using system-installed elbencho at $ELBENCHO"
        else
            log_msg 2 "elbencho binary not found. Downloading static release..."
            mkdir -p "$ARTIFACTS_DIR"
            ARCH=$(uname -m)
            if [[ "$ARCH" == "x86_64" || "$ARCH" == "amd64" ]]; then
                DL_URL="https://github.com/breuner/elbencho/releases/latest/download/elbencho-static-x86_64.tar.gz"
            elif [[ "$ARCH" == "aarch64" || "$ARCH" == "arm64" ]]; then
                DL_URL="https://github.com/breuner/elbencho/releases/latest/download/elbencho-static-aarch64.tar.gz"
            else
                log_msg 0 "Unsupported CPU architecture ($ARCH) for automatic download."
                exit 1
            fi
            
            TAR_FILE="$ARTIFACTS_DIR/elbencho.tar.gz"
            if command -v curl >/dev/null 2>&1; then
                curl -sL -o "$TAR_FILE" "$DL_URL"
            elif command -v wget >/dev/null 2>&1; then
                wget -qO "$TAR_FILE" "$DL_URL"
            else
                log_msg 0 "Neither curl nor wget found to download elbencho."
                exit 1
            fi
            
            tar -xzf "$TAR_FILE" -C "$ARTIFACTS_DIR"
            rm -f "$TAR_FILE"
            
            ELBENCHO="$ARTIFACTS_DIR/elbencho"
            if [[ ! -x "$ELBENCHO" ]]; then
                log_msg 0 "Failed to obtain executable elbencho binary at $ELBENCHO"
                exit 1
            fi
            log_msg 2 "Downloaded and extracted elbencho to $ELBENCHO"
        fi
    fi

    # Convert to absolute path to ensure remote hosts can find it regardless of their working directory
    if command -v "$ELBENCHO" >/dev/null 2>&1; then
        local resolved_bin
        resolved_bin=$(command -v "$ELBENCHO")
        if [[ "$resolved_bin" != /* ]]; then
            ELBENCHO="$(cd "$(dirname "$resolved_bin")" >/dev/null 2>&1 && pwd)/$(basename "$resolved_bin")"
        else
            ELBENCHO="$resolved_bin"
        fi
    else
        log_msg 0 "Cannot find executable elbencho binary: $ELBENCHO"
        exit 1
    fi
}

parse_size() 
{
    echo "$1" | awk '{
        val=$1+0;
        if ($1 ~ /[kK]/) val*=1024;
        else if ($1 ~ /[mM]/) val*=1048576;
        else if ($1 ~ /[gG]/) val*=1073741824;
        else if ($1 ~ /[tT]/) val*=1099511627776;
        else if ($1 ~ /[pP]/) val*=1125899906842624;
        printf "%.0f\n", val
    }'
}

format_base2_size() 
{
    echo "$1" | awk '{
        val=$1+0;
        if (val >= 1125899906842624) printf "%.1f PiB\n", val/1125899906842624;
        else if (val >= 1099511627776) printf "%.1f TiB\n", val/1099511627776;
        else if (val >= 1073741824) printf "%.1f GiB\n", val/1073741824;
        else if (val >= 1048576) printf "%.1f MiB\n", val/1048576;
        else if (val >= 1024) printf "%.1f KiB\n", val/1024;
        else printf "%.0f B\n", val;
    }' | sed 's/\.0 / /g'
}

expand_hosts() 
{
    awk -v hosts="$1" '
    BEGIN {
        len = length(hosts)
        in_bracket = 0
        cur_token = ""
        for (i=1; i<=len; i++) {
            c = substr(hosts, i, 1)
            if (c == "[") {
                in_bracket = 1
                cur_token = cur_token c
            } else if (c == "]") {
                in_bracket = 0
                cur_token = cur_token c
            } else if (c == "," && in_bracket == 0) {
                # We hit a comma OUTSIDE of brackets. Print the token.
                print_token(cur_token)
                cur_token = ""
            } else {
                cur_token = cur_token c
            }
        }
        if (cur_token != "") print_token(cur_token)
    }
    function print_token(tok) {
        s = index(tok, "[")
        e = index(tok, "]")
        if (s > 0 && e > s) {
            prefix = substr(tok, 1, s-1)
            suffix = substr(tok, e+1)
            ranges_str = substr(tok, s+1, e-s-1)
            
            n = split(ranges_str, parts, ",")
            for (j=1; j<=n; j++) {
                part = parts[j]
                dash = index(part, "-")
                if (dash > 0) {
                    start_str = substr(part, 1, dash-1)
                    end_str = substr(part, dash+1)
                    start_num = start_str + 0
                    end_num = end_str + 0
                    
                    # Dynamically determine zero-padding length from the start value
                    pad_len = length(start_str)
                    format_str = sprintf("%%s%%0%dd%%s,", pad_len)
                    
                    for (v=start_num; v<=end_num; v++) {
                        printf(format_str, prefix, v, suffix)
                    }
                } else {
                    printf("%s%s%s,", prefix, part, suffix)
                }
            }
        } else {
            printf("%s,", tok)
        }
    }' | sed 's/,$//' # Strip the final trailing comma
}

start_services() 
{
    resolve_elbencho_binary

    if [[ "$DRYRUN" == true ]]; then
        log_msg 2 "(DRYRUN) Would check and start elbencho services on: $HOSTS"
        return
    fi
    
    if [[ "$VERBOSE" == true ]]; then
        log_msg 3 "Checking and starting elbencho services on: $HOSTS"
    else
        log_msg 2 "Preparing $NUM_HOSTS benchmark host(s) via ssh..."
    fi
    
    IFS=',' read -ra HOST_ARR <<< "$HOSTS"
    
    # note: the square brackets for "--[s]ervice" and the FLAGS var are important for the grep command
    # to not catch the ssh command itself, which would otherwise match "elbencho --service" too. thus,
    # we avoid appearances of "elbencho --service" directly in the ssh command through this.
    
    local check_cmd="ps aux | grep '$(basename "$ELBENCHO") --[s]ervice' > /dev/null"
    local start_cmd="FLAGS='--service'; $ELBENCHO \$FLAGS >/dev/null"
    
    if command -v xargs >/dev/null 2>&1; then
        if [[ "$VERBOSE" == true ]]; then
            log_msg 3 "Parallelizing service startup (max concurrency: $STARTUP_CONCURRENCY)..."
            printf "%s\n" "${HOST_ARR[@]}" | xargs -P "$STARTUP_CONCURRENCY" -I {} ssh -n {} "if $check_cmd; then
                echo '  -> [{}] Service is already running. Keeping existing instance.'
            else
                echo '  -> [{}] Starting elbencho service...'
                $start_cmd
            fi"
        else
            printf "%s\n" "${HOST_ARR[@]}" | xargs -P "$STARTUP_CONCURRENCY" -I {} ssh -n {} "if ! $check_cmd; then $start_cmd; fi"
        fi
    else
        for host in "${HOST_ARR[@]}"; do
            if [[ "$VERBOSE" == true ]]; then
                ssh -n "$host" "if $check_cmd; then
                    echo '  -> [$host] Service is already running. Keeping existing instance.'
                else
                    echo '  -> [$host] Starting elbencho service...'
                    $start_cmd
                fi"
            else
                ssh -n "$host" "if ! $check_cmd; then $start_cmd; fi"
            fi
        done
    fi
}

stop_services() 
{
    resolve_elbencho_binary

    if [[ "$DRYRUN" == true ]]; then
        log_msg 2 "(DRYRUN) Would shut down elbencho services..."
        return
    fi
    log_msg 2 "Shutting down elbencho services..."
    $ELBENCHO --quit --hostsfile "$ARTIFACTS_DIR/hosts.txt" > /dev/null 2>&1
}

run_elbencho_cmd() 
{
    resolve_elbencho_binary

    if [[ "$VERBOSE" == true || "$DRYRUN" == true ]]; then
        echo -n "[CMD] $ELBENCHO "
        # Print arguments safely quoted so they can be copy-pasted
        printf '%q ' "$@"
        echo
    fi
    
    if [[ "$DRYRUN" == false ]]; then
        "$ELBENCHO" "$@"
    fi
}

process_host_args()
{
    # Process all provided host arguments
    RAW_HOSTS=""
    for arg in "${HOST_ARGS[@]}"; do
        if [[ -f "$arg" ]]; then
            # Read the file, strip comments (lines starting with #), remove any carriage returns (Windows compat), 
            # and replace newlines with commas
            parsed_file=$(grep -v '^[[:space:]]*#' "$arg" | tr -d '\r' | tr '\n' ',')
            RAW_HOSTS="${RAW_HOSTS},${parsed_file}"
        else
            RAW_HOSTS="${RAW_HOSTS},${arg}"
        fi
    done

    # Clean up leading/trailing/duplicate commas
    RAW_HOSTS=$(echo "$RAW_HOSTS" | sed 's/,,*/,/g' | sed 's/^,//' | sed 's/,$//')

    if [[ -n "$RAW_HOSTS" ]]; then
        # Expand host list to handle bracketed ranges (e.g., node[01-05,08,11-12])
        HOSTS=$(expand_hosts "$RAW_HOSTS")
        
        # Calculate total number of hosts safely 
        NUM_HOSTS=$(echo "$HOSTS" | tr ',' '\n' | wc -l | awk '{print $1}')
        log_msg 3 "Number of benchmark hosts: $NUM_HOSTS"
        
        # Generate the hosts file for elbencho to prevent overly long command lines
        mkdir -p "$ARTIFACTS_DIR"
        echo "$HOSTS" | tr ',' '\n' > "$ARTIFACTS_DIR/hosts.txt"
        
        HOSTS_FLAG=(--hostsfile "$ARTIFACTS_DIR/hosts.txt")
    else
        NUM_HOSTS=0
        HOSTS=""
        log_msg 2 "No benchmark hosts provided. Running elbencho in standalone mode."
        HOSTS_FLAG=()
    fi
}

generate_summary_table()
{
    # Generate Benchmark Summary if tests were executed
    if [[ "$TOTAL_RUNS" -gt 0 && "$DRYRUN" == false && -f "$RESULTS_DIR/results.json" ]]; then
        TREEFILE="$ARTIFACTS_DIR/treefile.txt"
        SUMMARY_DS_INFO=""
        
        if [[ -f "$TREEFILE" ]]; then
            # Fast extraction of total bytes and file count from treefile
            read -r SUMMARY_TOTAL_BYTES SUMMARY_FILE_COUNT <<< $(awk 'BEGIN {bytes=0; count=0} /^f / || /^F / {bytes+=$2; count++} END {printf "%.0f %d\n", bytes, count}' "$TREEFILE")
            
            if [[ -n "$SUMMARY_TOTAL_BYTES" && "$SUMMARY_FILE_COUNT" -gt 0 ]]; then
                SUMMARY_TOTAL_STR=$(format_base2_size "$SUMMARY_TOTAL_BYTES")
                SUMMARY_DS_INFO=" | Dataset: $SUMMARY_TOTAL_STR, $SUMMARY_FILE_COUNT files"
            fi
        fi

        {
            echo ""
            echo "========================================================================================"
            echo " SUMMARY ($(date '+%Y-%m-%d %H:%M:%S %z'))$SUMMARY_DS_INFO"
            echo "========================================================================================"
        } | tee -a "$RESULTS_DIR/results.txt"
        
        # Prepare header
        printf "Profile\tHosts\tThreads\tBlock\tThroughput\tIOPS\tAvg_IO_Lat\tElapsed\n" > "$ARTIFACTS_DIR/summary.tsv"
        printf -- "---------------\t-----\t-------\t-----\t----------\t--------\t-----------\t------------\n" >> "$ARTIFACTS_DIR/summary.tsv"
        
        # Extract last N lines matching the number of runs to avoid showing past executions
        tail -n "$TOTAL_RUNS" "$RESULTS_DIR/results.json" | jq -r '
        [
          .label,
          .config.hosts,
          .config.threads,
          (.config.block_size | tonumber | if . >= 1048576 then ((. / 1048576 | tostring) + "M") elif . >= 1024 then ((. / 1024 | tostring) + "K") else tostring end),
          ([(.first_done."bytes/s" // "0" | tonumber), (.last_done."bytes/s" // "0" | tonumber)] | max | if . >= 1000000000 then ((. / 1000000000 * 10 | round / 10 | tostring) + " GB/s") elif . >= 1000000 then ((. / 1000000 * 10 | round / 10 | tostring) + " MB/s") elif . >= 1000 then ((. / 1000 * 10 | round / 10 | tostring) + " KB/s") else (. | tostring + " B/s") end),
          ([(.first_done.iops // "0" | tonumber), (.last_done.iops // "0" | tonumber)] | max | if . >= 1000000 then ((. / 1000000 * 10 | round / 10 | tostring) + "M") elif . >= 1000 then ((. / 1000 * 10 | round / 10 | tostring) + "K") else tostring end),
          (try (.last_done.latency.IO.avg_us | tonumber | if . >= 1000 then ((. / 1000 * 10 | round / 10 | tostring) + " ms") else (. | tostring + " us") end) catch "-"),
          (try (.last_done.elapsed_time_ms | tonumber | (. / 3600000 | floor) as $h | ((. / 60000 | floor) % 60) as $m | ((. % 60000) / 1000) as $s | (($s * 10 | round) / 10 | tostring | if contains(".") then . else . + ".0" end) as $s_str | (if . >= 3600000 then ($h | tostring) + "h" else "" end) + (if . >= 60000 then ($m | tostring) + "m" else "" end) + $s_str + "s") catch "-")
        ] | @tsv
        ' >> "$ARTIFACTS_DIR/summary.tsv"
        
        # Format perfectly aligned output using awk (tab delimited to visual column widths)
        {
            cat "$ARTIFACTS_DIR/summary.tsv" | awk -F'\t' '{ printf " %-15s  %5s  %7s  %5s  %10s  %8s  %11s  %12s\n", $1, $2, $3, $4, $5, $6, $7, $8 }'
            echo "========================================================================================"
        } | tee -a "$RESULTS_DIR/results.txt"
    fi
}

generate_treefile()
{
    local phase_name="$1"
    local do_write="$2"
    
    FILE_BYTES=$(parse_size "$FILE_SIZE")
    if [[ "$FILE_BYTES" -eq 0 ]]; then
        log_msg 0 "File size cannot be zero."
        exit 1
    fi
    
    if [[ "$TARGET_BYTES" -lt "$FILE_BYTES" && "$TARGET_BYTES" -gt 0 ]]; then
        log_msg 2 "Target dataset size is smaller than configured file size. Adjusting file size."
        FILE_BYTES="$TARGET_BYTES"
    fi
    
    NUM_FILES=$(( TARGET_BYTES / FILE_BYTES ))
    if [[ "$NUM_FILES" -eq 0 ]]; then NUM_FILES=1; fi

    local total_str=$(format_base2_size "$TARGET_BYTES")
    local file_str=$(format_base2_size "$FILE_BYTES")
    log_msg 2 "$phase_name (Total: $total_str, File: $file_str, Files: $NUM_FILES)..."
    if [[ ${#CUSTOM_ARGS[@]} -gt 0 ]]; then
        log_msg 2 "  Custom Args: ${CUSTOM_ARGS[*]}"
    fi

    if [[ "$DRYRUN" == true ]]; then
        log_msg 2 "(DRYRUN) Would generate treefile... ($TREEFILE)"
    else
        log_msg 3 "Generating treefile... ($TREEFILE)"
        
        awk -v fsize="$FILE_BYTES" -v nfiles="$NUM_FILES" '
        BEGIN {
            for (i=1; i<=nfiles; i++) {
                printf("f %d file_%06d\n", fsize, i)
            }
        }' > "$TREEFILE"
    fi
    
    if [[ "$do_write" == false ]]; then
        log_msg 2 "Treefile generated. Skipping data layout."
    fi
}

run_prepare_write()
{
    local PREPARE_PROFILE="$PROFILES_DIR/.prepare"
    
    if [[ ! -f "$PREPARE_PROFILE" ]]; then
        log_msg 0 "Preparation profile not found at $PREPARE_PROFILE"
        exit 1
    fi

    local BASE_ARGS=(
        "$TARGET_DIR"
        "${HOSTS_FLAG[@]}"
        --treefile "$TREEFILE"
        --resfile "$RESULTS_DIR/results.txt"
        --csvfile "$RESULTS_DIR/results.csv"
        --jsonfile "$RESULTS_DIR/results.json"
        --label "prepare"
        "${COMMON_ELBENCHO_ARGS[@]}"
    )
    
    # If threads parameter was explicitly set, override profile
    if [[ -n "$THREADS" ]]; then
        BASE_ARGS+=(--threads "$THREADS")
    fi
    
    run_elbencho_cmd "${BASE_ARGS[@]}" -c "$PREPARE_PROFILE" "${CUSTOM_ARGS[@]}"
        
    if [[ $? -ne 0 ]]; then
        log_msg 0 "Dataset preparation terminated with an error."
        exit 1
    fi
}

run_auto_prepare_loop()
{
    local phase_name="$1"
    local auto_time_ms="$2"
    local max_bytes=-1
    
    if [[ "$S3_MODE" == false ]]; then
        local free_kbytes=$(df -Pk "$TARGET_DIR" | awk 'NR==2 {print $4}')
        if [[ -z "$free_kbytes" || ! "$free_kbytes" =~ ^[0-9]+$ ]]; then
            log_msg 0 "Failed to determine free space on $TARGET_DIR"
            exit 1
        fi
        local free_bytes=$(( free_kbytes * 1024 ))
        max_bytes=$(( free_bytes / 2 ))
    fi

    while true; do
        local capped=false
        if [[ "$S3_MODE" == false && "$TARGET_BYTES" -gt "$max_bytes" ]]; then
            TARGET_BYTES="$max_bytes"
            capped=true
            log_msg 2 "Auto-preparation dataset size reduced to 50% of free capacity ($(format_base2_size $max_bytes))."
        fi
        
        generate_treefile "$phase_name" true
        
        run_prepare_write
        
        local last_time_ms=$(tail -n 1 "$RESULTS_DIR/results.json" | jq -r '.last_done.elapsed_time_ms // 0')
        
        if [[ "$capped" == true || "$last_time_ms" -ge "$auto_time_ms" ]] || [[ "$DRYRUN" == true ]]; then
            log_msg 2 "Auto-preparation complete (Elapsed: ${last_time_ms}ms)."
            TOTAL_RUNS=$((TOTAL_RUNS + 1))
            if [[ "$DRYRUN" == false ]]; then
                echo "$NUM_HOSTS" > "$ARTIFACTS_DIR/auto_prep_hosts.txt"
            fi
            break
        else
            log_msg 2 "Preparation took ${last_time_ms}ms (less than target ${auto_time_ms}ms). Doubling size..."
            TARGET_BYTES=$(( TARGET_BYTES * 2 ))
        fi
    done
}

run_test_profiles()
{
    if [[ ${#TEST_PROFILES[@]} -eq 0 ]]; then
        return
    fi
    
    # Resolve all profiles, applying wildcards
    local RESOLVED_PROFILES=()
    if [[ ! -d "$PROFILES_DIR" ]]; then
        log_msg 0 "Profiles directory not found: $PROFILES_DIR"
        exit 1
    fi
    
    pushd "$PROFILES_DIR" > /dev/null
    shopt -s nullglob
    for p in "${TEST_PROFILES[@]}"; do
        local matches=($p)
        local matched_any=false
        
        for match in "${matches[@]}"; do
            if [[ -f "$match" ]]; then
                RESOLVED_PROFILES+=("$PROFILES_DIR/$match")
                matched_any=true
            fi
        done
        
        if [[ "$matched_any" == false ]]; then
            log_msg 0 "No test profile matching '$p' found."
            log_msg 0 "  Please check the '$PROFILES_DIR' directory for valid profile names."
            shopt -u nullglob
            popd > /dev/null
            exit 1
        fi
    done
    shopt -u nullglob
    popd > /dev/null

    log_msg 3 "Test phase: Preparing to run ${#RESOLVED_PROFILES[@]} profile(s)..."
    
    if [[ ! -f "$TREEFILE" && "$DRYRUN" == false ]]; then
        log_msg 0 "No treefile found at $TREEFILE."
        log_msg 0 "  You must run the script with the '--prep' (or '--noprepwrite') parameter first."
        exit 1
    fi

    for PROFILE_PATH in "${RESOLVED_PROFILES[@]}"; do
        local prof_file=$(basename "$PROFILE_PATH")
        local prof_name="${prof_file%.*}"
        
        log_msg 2 "Running test profile '$prof_file' for ${DURATION}s..."
        if [[ ${#CUSTOM_ARGS[@]} -gt 0 ]]; then
            log_msg 2 "  Custom Args: ${CUSTOM_ARGS[*]}"
        fi

        # Base command args for reading/testing existing files
        local BASE_ARGS=(
            "$TARGET_DIR"
            "${HOSTS_FLAG[@]}" 
            --timelimit "$DURATION"
            --treefile "$TREEFILE"
            --resfile "$RESULTS_DIR/results.txt"
            --csvfile "$RESULTS_DIR/results.csv"
            --jsonfile "$RESULTS_DIR/results.json"
            --label "${prof_name}"
            "${COMMON_ELBENCHO_ARGS[@]}"
        )
        
        if [[ "$TIME_GIVEN" == true && "$NO_LOOP" == false ]]; then
            BASE_ARGS+=(--infloop)
        fi
        
        # If threads parameter was explicitly set, override profile
        if [[ -n "$THREADS" ]]; then
            BASE_ARGS+=(--threads "$THREADS")
        fi
        
        run_elbencho_cmd "${BASE_ARGS[@]}" -c "$PROFILE_PATH" "${CUSTOM_ARGS[@]}"
        
        if [[ $? -ne 0 ]]; then
            log_msg 0 "Test profile '$prof_file' terminated with an error."
            log_msg 0 "  Aborting subsequent test profiles."
            exit 1
        fi
        
        TOTAL_RUNS=$((TOTAL_RUNS + 1))
    done
}

# ==============================================================================
# Main Execution
# ==============================================================================

# Resolve result directory early so we can log to it
if [[ -z "$RESULTS_DIR" ]]; then
    RESULTS_DIR="$SCRIPT_DIR/results/$(date +%Y%m%d)"
fi

if [[ "$DRYRUN" == false ]]; then
    mkdir -p "$RESULTS_DIR"
fi

COMMON_ELBENCHO_ARGS=()
if [[ -n "$IODEPTH" ]]; then COMMON_ELBENCHO_ARGS+=(--iodepth "$IODEPTH"); fi
if [[ -n "$RAND_ACCESS" ]]; then COMMON_ELBENCHO_ARGS+=(--rand="$RAND_ACCESS"); fi
if [[ -n "$DIRECT_IO" ]]; then COMMON_ELBENCHO_ARGS+=(--direct="$DIRECT_IO"); fi
if [[ -n "$BLOCK_SIZE" ]]; then COMMON_ELBENCHO_ARGS+=(--block "$BLOCK_SIZE"); fi
if [[ "$MEASURE_LATENCY" == true ]]; then COMMON_ELBENCHO_ARGS+=(--lat); fi

process_host_args

# If the user ONLY wants to stop services (no prepare, no noprep, no test)
if [[ "$STOP_SERVICES" == true && "$PREP_GIVEN" == false && "$NO_PREP_WRITE" == false && "$TEST_PROFILES_GIVEN" == false ]]; then
    if [[ -n "$HOSTS" ]]; then
        stop_services
    else
        log_msg 2 "Standalone mode: no services to stop."
    fi
    exit 0
fi

# Default to read tests if no test profile was explicitly provided
if [[ "$TEST_PROFILES_GIVEN" == false ]]; then
    if [[ "$S3_MODE" == true ]]; then
        TEST_PROFILES=("s3-read-bw" "s3-read-iops")
    else
        TEST_PROFILES=("file-read-bw" "file-read-iops")
    fi
fi

# Make jq a mandatory requirement for generating the summary table
if ! command -v jq >/dev/null 2>&1; then
    log_msg 0 "The 'jq' tool is required to generate the benchmark summary table."
    log_msg 0 "  Please install 'jq' before running this script."
    exit 1
fi

if [[ -z "$TARGET_DIR" ]]; then
    log_msg 0 "--dir is required unless solely stopping services."
    usage
fi

if [[ "$S3_MODE" == false ]]; then
    ensure_target_dir_exists
else
    log_s3_info
    ensure_s3_bucket_exists
fi

if [[ -n "$HOSTS" ]]; then
    start_services
fi

# 1. PREPARE PHASE: Lay out the dataset or generate treefile only
TREEFILE="$ARTIFACTS_DIR/treefile.txt"

# Delete existing treefile if --prep auto is explicitly given
if [[ -f "$TREEFILE" && "$PREP_GIVEN" == true && "$PREPARE_SIZE" == "auto" ]]; then
    if [[ "$DRYRUN" == true ]]; then
        log_msg 2 "(DRYRUN) Would delete existing treefile to force new auto preparation phase."
    else
        log_msg 3 "Explicit '--prep auto' requested. Deleting existing treefile to force new auto preparation phase."
        rm -f "$TREEFILE"
    fi
fi

# Determine Prep Phase Actions
RUN_AUTO_PREP=false
RUN_MANUAL_PREP=false
RUN_VALIDATION=false

if [[ "$PREP_GIVEN" == true && "$PREPARE_SIZE" != "auto" ]]; then
    RUN_MANUAL_PREP=true
elif [[ "$PREP_GIVEN" == true && "$PREPARE_SIZE" == "auto" ]]; then
    RUN_AUTO_PREP=true
elif [[ ! -f "$TREEFILE" ]]; then
    RUN_AUTO_PREP=true
else
    RUN_VALIDATION=true
fi

if [[ "$RUN_VALIDATION" == true ]]; then
    log_msg 2 "Validating existing dataset files from treefile..."
    if ! validate_dataset "$TREEFILE" false; then
        log_msg 0 "Dataset validation failed. No valid dataset found and no explicit '--prep' argument given."
        log_msg 0 "  Please run the script again with '--prep auto' or '--prep <size>' to prepare a dataset."
        exit 1
    fi
    log_msg 2 "Dataset validation passed."

    check_auto_prep_host_increase
elif [[ "$RUN_AUTO_PREP" == true || "$RUN_MANUAL_PREP" == true ]]; then
    if [[ "$RUN_MANUAL_PREP" == true ]]; then
        TARGET_BYTES=$(parse_size "$PREPARE_SIZE")
        DO_WRITE=true
        PHASE_NAME="Prepare phase: Laying out dataset"
        if [[ "$DRYRUN" == false ]]; then
            rm -f "$ARTIFACTS_DIR/auto_prep_hosts.txt"
        fi
    elif [[ "$RUN_AUTO_PREP" == true ]]; then
        TARGET_BYTES=$(parse_size "$AUTO_SIZE")
        DO_WRITE=true
        PHASE_NAME="Auto-prepare phase: Laying out dataset"
        
        # Convert duration to seconds, then to ms
        AUTO_TIME_SEC=$(echo "$AUTO_TIME" | awk '{val=$1+0; if ($1 ~ /m$/) val*=60; else if ($1 ~ /h$/) val*=3600; printf "%.0f", val}')
        AUTO_TIME_MS=$(( AUTO_TIME_SEC * 1000 ))
    fi

    if [[ "$NO_PREP_WRITE" == true ]]; then
        if [[ "$RUN_MANUAL_PREP" == true ]]; then
            DO_WRITE=false
            PHASE_NAME="Prepare phase w/o write: Generating treefile only"
        else
            log_msg 2 "Ignoring '--noprepwrite' because automatic preparation mode requires writing data to scale dataset size."
        fi
    fi

    if [[ "$DO_WRITE" == false ]]; then
        # NOPREP logic
        generate_treefile "$PHASE_NAME" false
    elif [[ "$RUN_AUTO_PREP" == true ]]; then
        run_auto_prepare_loop "$PHASE_NAME" "$AUTO_TIME_MS"
    elif [[ "$RUN_MANUAL_PREP" == true ]]; then
        generate_treefile "$PHASE_NAME" true
        
        log_msg 3 "Checking if dataset preparation is necessary..."
        if validate_dataset "$TREEFILE" true; then
            log_msg 2 "Valid dataset already exists for the requested size. Skipping preparation write phase."
        else
            log_msg 2 "Dataset preparation required. Starting elbencho write phase..."
            run_prepare_write
            log_msg 2 "Dataset preparation complete."
            TOTAL_RUNS=$((TOTAL_RUNS + 1))
        fi
    fi
fi

# 2. TEST PHASE: Run specific profiles
run_test_profiles

if [[ "$STOP_SERVICES" == true && -n "$HOSTS" ]]; then
    stop_services
fi

generate_summary_table

log_msg 3 "All Done."