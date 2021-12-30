#!/bin/bash
#
# GNU General Public License v3.0; it is intended to be always in sync
# with the license term employed by the elbencho itself.
#
# Name   : dgen.sh - a bash wrapper of the elbencho program by Sven Breuner
#                    for generating test data sets.
#
# Author : Chin Fang <fangchin@zettar.com>
#
# Purpose: 0. Facilitate the conduction of a storage read sweep in a
#             client of a POSIX compliant file storage service,
#             e.g. BeeGFS, NFS.
#          1. Make it easy to carry out data transfer sweep by generating
#             test datasets conveniently.
#          2. Show some best practices of using elbencho.
#
# Notes :  0. Only test datasets containing files of the same size in
#             2th power (e.g. KiB, MiB, GiB, TiB) are created using
#             this wrapper. Note also elbencho, together with its 
#             companion tools, are more verstile than many realize.
#             The wrapper leverages only a small fraction of such
#             versatility.
#          1. Only hyperscale datasets are generated, i.e. each
#             dataset's overall size >= 1TB or has >= 1M files or
#             both. For file size <= 1MiB, a generated dataset always
#             consists of 1048576 of files.
#          2. This bash elbencho wrapper doesn't generate multiple
#             test datasets in one shot by default.  This is because
#             often times, insufficient storage space is available.
#          3. Should generating multiple test datasets be desired,
#             writing a small bash script that call this wrapper
#             repeatedly using its -f option for different file sizes
#             should be trivial. There is also a -r option for
#             generating all defined test data sets in a file size
#             range, if there is enough storage capacity available. In
#             fact, with the naming convention for test datasets, it's
#             very feasible to generate only datasets in the range s
#             (small); 1KiB <= file sizes <= 512KiB, m (medium); 1MiB
#             <= file sizes <= 512MiB, l (large); 1GiB <= file size <=
#             512GiB, or h (huge); 1TiB <= file sizes are generated.
#          4. Even it's possible to write a bash elbencho wrapper that
#             uses elbencho first to generate the test dataset, then
#             run the read benchmarking, this approach is deemed less
#             general. Thus this approach of generating test data set
#             separately, even it still uses elbencho.  The generated
#             datasets can be used subsequently for other benchmarking
#             purposes, either prior or after running elbencho.
#          5. The wrapper is optimized for flash based storage. The end
#             of using HDDs for primary storage is approaching fast.
#          6. This script is meant to be used in conjunction with the
#             elbencho storage sweep tools hosted on the github.
#          7. This script is checked with shellcheck without any warnings
#             or errors :)
#
#

#
# A few global variables. Contrary to a myth popular among newbie
# programmers, if judiciously employed, global variables simplify
# programming :D
#
# A terabyte is 1,000,000 megabytes. A megabytes is 1000000 bytes.
# So, a terabyte is 1000000 * 1000000 bytes. We require 2 TB at
# least. If the -r option is used, then 10 TB or 40 TB space should be
# available.
#
minimal_space=2000000000000

# global array index
idx=0 # initial index for an iteration
fdx=  # final index for an iteration
nfiles=
fsize=
range=
cmd=
# this value is applicable to RHEL {7,8}.x because of the getconf
# ARG_MAX value.  It's a kind of a "sliding window".
window=32768
# The number of iterations necessary to generate a test dataset for a
# particular file size.
niter=
# preferred file descriptor limit for both unlimit -Hn and -Sn
fdlimit=262144
verbose=
block_size=1m
dry_run=
threads=$(nproc)
#
# The following is just a reasonable default. Set it to whatever you wish.
# This is a bash script after all :D
#
src_data_dir=/mnt/data/src
#
# Should it be desired to test more, e.g. 2TiB, 4TiB etc, just add such
# desired entries to the following bash array. Also, the array should be
# regarded as a kind of "map".  In the future, the map array should be
# read in via an external configuration file, which is shared with the
# elbencho storage sweep tools.  This should maintain consistency.
#
declare -a xlabels=('1048576x1KiB' '1048576x2KiB' '1048576x4KiB'
    '1048576x8KiB' '1048576x16KiB' '1048576x32KiB'
    '1048576x64KiB' '1048576x128KiB' '1048576x256KiB'
    '1048576x512KiB' '1048576x1MiB' '524288x2MiB'
    '262144x4MiB' '131072x8MiB' '65536x16MiB'
    '32768x32MiB' '16384x64MiB' '8192x128MiB'
    '4096x256MiB' '2048x512MiB' '1024x1GiB'
    '512x2GiB' '256x4GiB' '128x8GiB'
    '64x16GiB' '32x32GiB' '16x64GiB'
    '8x128GiB' '4x256GiB' '2x512GiB'
    '1x1TiB'
)

help() {
    local help_str=''
    read -r -d '' help_str <<'EOF'
This script is a bash wrapper for the elbencho program by Sven Breuner.

It uses elbencho as a fast test dataset generator. This is a very
uncommon usage. Most people simply take elbencho strictly as a storage
benchmark :D

Please see https://github.com/breuner/elbencho.

Command line options:
    -h show this help
    -t the number of threads to run elbencho. Default: auto-detect
    -s the path to the directory holding test datasets. Default: /mnt/data/src
    -f file size desired.  Format: integer followed by K, M, G, T -
       both lower and upper case accepted, e.g. 512M or 512m. This
       option conflicts with -r.  To use this option, there should be
       at least 2TiB free storage capacity available (except in
       dry-run mode).
    -r the dataset range to auto-generate, one of s: LOSF, m: medium,
       l: large, h: huge, A: all.  This option conflicts with -f. To
       use this option, there should be at least 10TiB free storage
       capacity available (except in dry-run mode).
    -b block size. Integer, e.g. 1, 2, 16. Number of mebibytes to
       read/write in a single elbencho operation.  Default: 1M as used
       by elbencho.
    -v verbose mode; to print out the run set up and info. This mode is mainly
       for learning and possible debugging. It's slower too
    -n dry run; to print out tests that would have run. It also helps learning.

The usage: 
$ sudo dgen.sh [-h][-t num_threads][-r range][-s dirpath][-f file_size]\
  [-r file_size_range][-b block_size][-v][-n]

Examples: 0. dgen.sh -s /var/local/zettar/sweep -f 2K -b 16 -v -n
          1. dgen.sh -s /var/local/zettar/sweep -f 4K -b 16 -v
          2. dgen.sh -s /var/local/zettar/sweep -f 1G -b 16
          3. dgen.sh -s /var/local/zettar/sweep -r l -b 16
          4. dgen.sh -s /var/local/zettar/sweep -r A -b 16

0. shows how to do a dry run. It also shows that for small file size,
   the O_DIRECT is disabled.  
1. shows how to run the wrapper in the verbose mode (slower!)
2. shows how to run the wrapper in quiet mode (faster :)
3. shows how to generate test datasets with file sizes in the small range in
   quiet mode (may take a while)
4. shows how to generate all defined test datasets in quiet mode (will take
   a while even in this mode!

Notes: 
        0. Your system's getconf ARG_MAX value may limit how many
           files can be generated in one shot.  Typical error: -bash:
           /usr/bin/elbencho: Argument list too long.
        1. The script attempts to get around such error intelligently using
           a "sliding window" approach.
        2. The verbose mode provides even elbencho's output.  If
           disabled, the script runs completely quiet and is also
           faster.  We recommend using the verbose mode (-v) and
           dry-run mode (-n) to learn how to use the script first,
           then use the non-verbose mode for actual work.
        3. Only test datasets containing files of the same size in 2th
           power (e.g. KiB, MiB, GiB, TiB) are created using this
           wrapper.
        4. Datasets generated are always named in the convention
           number_of_filesxfile_size. For example, 2x512GiB - a
           dataset that has two (2) files - both of the size 512 GiB.
        5. Note also elbencho, together with its companion tools, are more 
           verstile than many realize. The wrapper leverages only a small 
           fraction of such versatility.
        6. The wrapper is optimized for flash based storage. The end
           of using HDDs for primary storage is approaching fast!
EOF
    echo "$help_str"
    exit 0
}

verify_threads() {
    # Note, threads is a global variable :)
    local msg

    if ! [[ $threads =~ ^[1-9][0-9]*$ ]]; then
        msg="threads must be a positive integer. Abort!"
        echo "$msg"
        exit 1
    fi
}

verify_directory_exists() {
    # For the dry-run mode, skip the src_data_dir check
    [[ "$dry_run" ]] && return 0
    if ! [[ "$(cd "$src_data_dir")" -eq 0 ]]; then
        msg="$src_data_dir does not exist. Abort!"
        echo "$msg"
        exit 1
    fi
}

has_element() {
    local e match="$1"
    shift
    for e; do
        if [[ $e =~ .*"$match".* ]]; then
            return 0
        fi
        ((++idx))
    done
    return 1
}

verify_range() {
    local range
    range=$1
    local msg

    if [[ $range != [smlhA] ]]; then
        msg="Only s:LOSF, m:medium files, l:large files, h:huge files, A: all "
        msg+="allowed. Abort!"
        echo "$msg"
        exit 1
    fi
}

verify_block_size() {
    # Note, block_size is a global variable :)
    local msg

    if ! [[ $block_size =~ ^[1-9][0-9]*$ ]]; then
        msg="block_size must be specified in a positive integer. Abort!"
        echo "$msg"
        exit 1
    else
        block_size="${block_size}m"
    fi
}

show_elbencho_version() {
    local vs
    if ! command -v elbencho &>/dev/null; then
        echo "elbencho could not be found. Abort!"
        exit 1
    fi
    vs=$(elbencho --version)
    echo "$vs"
}

show_installed_tool_path() {
    echo "dgen.sh is installed at $(command -v dgen.sh)"
}

show_option_settings() {
    echo "threads         : $threads"
    echo "src_data_dir    : $src_data_dir"
    [[ -z $range ]] && echo "number of files : $nfiles"
    [[ -z $range ]] && echo "file size       : $fsize"
    [[ -z $range ]] && echo "iterations      : $niter"
    [[ -z $range ]] && echo "sliding window  : $window"
    echo "block_size      : $block_size"
    echo "verbose         : $verbose"
    echo "dry_run         : $dry_run"
}

check_space_available() {
    if ! [[ -d $src_data_dir ]]; then
        echo "$src_data_dir doesn't exist! Abort!"
        exit 1
    fi
    local sa
    # The df man page states that SIZE units default to 1024 bytes
    sa=$(df $src_data_dir | tail -1 | awk '{ printf "%d", $4*1024 }')
    [[ "$verbose" ]] && echo "Space availability now is: $sa bytes."
    [[ "$verbose" ]] && echo "Minimal requirement is   : $minimal_space bytes."
    if [[ $sa -lt $minimal_space ]]; then
        local msg
        msg="Only hyperscale datasets are generated, i.e. each dataset's\n"
        msg+="overall size >= 1TB or has >= 1M files or both. As a\n"
        msg+="precaution, $minimal_space bytes free space is required to\n"
        msg+="run this script. Please free up more space and then re-run.\n"
        msg+="Aborting now."
        echo -e "$msg"
        exit 1
    fi
}

set_max_open_file_descriptors() {
    local hfdl
    hfdl=$(ulimit -Hn)
    local sfdl
    sfdl=$(ulimit -Sn)
    if (("$hfdl < "$fdlimit)); then
        ulimit -Hn "$fdlimit"
    fi
    if (("$sfdl < "$fdlimit)); then
        ulimit -Sn "$fdlimit"
    fi
}

check_elbencho_version() {
    local vs
    local mandatory
    mandatory="1.6"
    if ! command -v elbencho &>/dev/null; then
        echo "elbencho could not be found. Abort!"
        exit 1
    fi
    vs=$(elbencho --version | grep ^Version |
        cut -d ':' -f 2 | sed -e 's/^[ \t]*//' -e 's/\-[0-9]//')
    # Use bash's numeric context. Use the following to ensure that the
    # check remains functional with elbencho that's newer than 1.6.x
    if (($(echo "$vs < $mandatory" | bc -l))); then
        echo "Installed elbencho too old. Abort!"
        exit 1
    fi
}

set_full_dataset_path() {
    local dataset
    dataset=$1
    if [[ -z $src_data_dir ]]; then
        # I assume that you have cd src_data_dir already!
        dataset="./$dataset"
    else
        dataset="$src_data_dir/$dataset"
    fi
    echo "$dataset"
}

ensure_dataset_exists() {
    local dataset
    dataset=$1
    if [[ -z $dry_run ]]; then
        if ! [[ -d $dataset ]]; then
            mkdir "$dataset" || (echo "Couldn't create $dataset!" && exit 1)
        fi
    fi
}

#
# The following is for 1KiB - 512KiB
#
generate_losf() {
    local dataset="$src_data_dir/${nfiles}x$fsize"
    ensure_dataset_exists "$dataset"
    #
    # Note that elbencho doesn't understand KiB, MiB, GiB etc.
    #
    sfsize=${fsize/KiB/k}
    local indices
    mapfile -t indices < <(seq 0 $window "$nfiles")
    #
    # The following bash eval trick is used in mtelbencho.sh too.
    #
    for ((c = 0; c < niter; c++)); do
        idx="${indices[$c]}"
        local ca1=$((c + 1))
        fdx="${indices[$ca1]}"
        ((--fdx))
        idx=$(printf "%07d" "$idx")
        fdx=$(printf "%07d" "$fdx")
        dry_or_real_run
        unset cmd
    done
}

#
# The following is for 1MiB - 512MiB
#
generate_medium() {
    sfsize=${fsize/MiB/m}
    local dataset="$src_data_dir/${nfiles}x$fsize"
    ensure_dataset_exists "$dataset"
    if [[ $nfiles -gt $window ]]; then
        local indices
        mapfile -t indices < <(seq 0 $window "$nfiles")
        #
        # The following bash eval trick is used in mtelbencho.sh
        #
        for ((c = 0; c < niter; c++)); do
            idx=${indices[$c]}
            idx=$(printf "%07d" "$idx")
            local ca1=$((c + 1))
            fdx=${indices[$ca1]}
            ((--fdx))
            fdx=$(printf "%07d" "$fdx")
            dry_or_real_run
            unset cmd
        done
    else
        idx=$(printf "%07d" "0")
        fdx=$((nfiles - 1))
        fdx=$(printf "%07d" "$fdx")
        cmd="elbencho -w --nolive --direct -t $threads "
        cmd+="-b $block_size -s $sfsize "
        dry_or_real_run
        unset cmd
    fi
}

#
# The following is for 1GiB - 512GiB
#
generate_large() {
    sfsize=${fsize/GiB/g}
    local dataset="$src_data_dir/${nfiles}x$fsize"
    ensure_dataset_exists "$dataset"
    idx=$(printf "%07d" "0")
    fdx=$((nfiles - 1))
    fdx=$(printf "%07d" "$fdx")
    dry_or_real_run
    unset cmd
}

#
# The following is for 1TiB and bigger
#
generate_huge() {
    sfsize=${fsize/TiB/t}
    local dataset="$src_data_dir/${nfiles}x$fsize"
    ensure_dataset_exists "$dataset"
    idx=$(printf "%07d" "0")
    fdx=$((nfiles - 1))
    fdx=$(printf "%07d" "$fdx")
    dry_or_real_run
    unset cmd
}

dry_or_real_run() {
    #
    # O_DIRECT is not applicable to files whose sizes are smaller
    # then the file system's block size (4K typical)
    #
    if [[ $fsize != "1KiB" ]] && [[ $fsize != "2KiB" ]]; then
        cmd="elbencho -w --nolive --direct -t $threads "
    else
        cmd="elbencho -w --nolive -t $threads "
    fi
    cmd+="-b $block_size -s $sfsize "
    local dcmd="${cmd}"
    while ((${#dcmd} < ${#cmd[@]})); do
        dcmd="${dcmd}${cmd}"
    done
    dcmd+="f{$idx..$fdx}"
    if [[ -z $dry_run ]]; then
        cmd+="$(eval echo "f{$idx..$fdx}")"
        cd "$dataset" || exit
        [[ "$verbose" ]] && echo "Running $dcmd" && $cmd
        $cmd >/dev/null 2>&1
    else
        cmd+="f{$idx..$fdx}"
        echo "Command to run: $dcmd"
    fi
}

split_ds() {
    local ds=$1
    local nfile_n_size
    OIFS=$IFS
    IFS='x'
    read -ra nfile_n_size <<<"$ds"
    nfiles="${nfile_n_size[0]}"
    fsize="${nfile_n_size[1]}"
    IFS="$OIFS"
}

generate_ds() {
    if [[ $fsize =~ 'K' ]]; then
        generate_losf
    elif [[ $fsize =~ 'M' ]]; then
        generate_medium
    elif [[ $fsize =~ 'G' ]]; then
        generate_large
    elif [[ $fsize =~ 'T' ]]; then
        generate_huge
    else
        echo "Non-supported file size range given. Abort!"
        exit 1
    fi
}

create_dses() {
    local ii=$1
    local sal=$2
    local ds=
    for ds in "${xlabels[@]:ii:sal}"; do
        split_ds "$ds"
        ((niter = nfiles / window))
        generate_ds
        ((ne = ne + 1))
    done
}

show_test_duration() {
    local total_test_time
    total_test_time=$1
    local t_hsec
    local t_hor
    local t_min
    local t_sec

    t_hor=$((total_test_time / 3600))
    if [[ $t_hor -gt 0 ]]; then
        t_hsec=$((t_hor * 3600))
        t_hsec=$((total_test_time - t_hsec))
        t_min=$((t_hsec / 60))
        t_sec=$((total_test_time % 60))
        echo "Total test time: ${t_hor}h:${t_min}m:${t_sec}s"
    else
        t_min=$((total_test_time / 60))
        t_sec=$((total_test_time % 60))
        echo "Total test time: ${t_min}m:${t_sec}s"
    fi
}

# main()
{
    begin_test=$(date +"%s")
    while getopts ":ht:s:f:r:Ab:vn" opt; do
        case $opt in
        h)
            help
            ;;
        t)
            threads=$OPTARG
            verify_threads
            ;;
        s)
            src_data_dir=$OPTARG
            verify_directory_exists
            ;;
        f)
            fsize=$OPTARG
            if [[ "$range" ]]; then
                echo "-r conflicts with the -f option. Aborting!"
                exit 1
            fi
            fsize="${fsize^^}" # just to make sure
            if has_element "$fsize" "${xlabels[@]}"; then
                ds=${xlabels[$idx]}
                split_ds "$ds"
                ((niter = nfiles / window))
            else
                echo "Unsupported file size entered. Please enter file "
                echo "sizes prefixed by a 2th power, e.g. 1, 2, 16, "
                echo "followed by K, M, G, T. Both lower and upper case "
                echo "are accepted. Abort!"
                exit 1
            fi
            ;;
        r)
            range=$OPTARG
            if [[ "$fsize" ]]; then
                echo "-f conflicts with the -r option. Aborting!"
                exit 1
            fi
            verify_range "$range"
            if [[ $range != [sA] ]]; then
                minimal_space=10000000000000 # 10TB should be safe
            elif [[ $range == "A" ]]; then
                minimal_space=40000000000000 # 40TB should be safe
            fi
            ;;
        b)
            block_size=$OPTARG
            verify_block_size
            ;;
        v)
            verbose=1
            ;;
        n)
            dry_run=1
            ;;
        *)
            echo "Error: invalid option given!"
            exit 2
            ;;
        esac
    done
    [[ "$verbose" ]] && show_elbencho_version
    [[ "$verbose" ]] && show_installed_tool_path
    [[ "$verbose" ]] && show_option_settings
    [[ -z $dry_run ]] && check_space_available
    [[ -z $dry_run ]] && set_max_open_file_descriptors
    [[ -z $dry_run ]] && check_elbencho_version
    if [[ -z $range ]]; then
        generate_ds
    elif [[ $range == "s" ]]; then
        ii=0
        sal=10 # subarray lenth -1
        create_dses "$ii" "$sal"
    elif [[ $range == "m" ]]; then
        ii=10
        sal=10 # subarray lenth -1
        create_dses "$ii" "$sal"
    elif [[ $range == "l" ]]; then
        ii=20
        sal=10 # subarray lenth -1
        create_dses "$ii" "$sal"
    elif [[ $range == "h" ]]; then
        ii=30
        sal=1 # subarray lenth -1
        create_dses "$ii" "$sal"
    elif [[ $range == "A" ]]; then
        ii=0
        sal=31
        create_dses "$ii" "$sal"
    else
        echo "Impossible file size range entered: $range. Aborting!"
        exit 1
    fi
    end_test=$(date +"%s")
    total_test_time=$((end_test - begin_test))
    [[ "$verbose" ]] && show_test_duration "$total_test_time"
    exit 0
}
