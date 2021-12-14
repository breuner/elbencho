#!/bin/bash
#
# GNU General Public License v3.0; it is intended to be always in sync
# with the license term employed by the elbencho itself.
#
# Name   : mtelbencho.sh (mt: multiple test) - a bash wrapper of the
#          elbencho program by Sven Breuner.
#
# Author : Chin Fang <fangchin@zettar.com>
#
# Purpose: 0. Facilitate the conduction of a storage sweep in a client of
#             a POSIX compliant file storage service, e.g. BeeGFS, NFS.
#          1. Make it easy to select
#             1.1 A distributed POSIX compliant file system 
#             1.2 Storage devices (e.g. NVMe SSDS) based on actual test
#                 results obtained in the user's environment.
#          2. Show some best practices for using elbencho.
#
# Remarks: 0. In the following, it is assumed that the elbencho in the
#             $PATH.
#          1. The wrapper always invokes elbencho with its --dropcache
#             option. As such, it must be run either under root or
#             by a user with sudo root priviledge.
#          2. By default, it runs only tests in direct IO mode with
#             elbencho (see elbencho --help-all for --direct) but if
#             the option `-B` is specified, it will run elbencho in
#             buffered mode.  Note that the script runs only write
#             tests, which is the most important to the 4th IT pillar
#             - moving data at scale and speed.
#          3. The wrapper assumes all test datasets are located in the
#             current working directory (`pwd`). Nevertheless, the -s
#             option enables to specify the directory holding all test
#             datasets.
#          4. The '-b' option enables a user to specify the desired
#             block size (see elbencho --help-all for -b [ --block ]
#             arg). elbencho defaults to 1MiB.
#          5. mtelbencho.sh sweeps from 1KiB to 1TiB, incremented in
#             power-of-two file sizes, over the following three file
#             size ranges:
#             4.1 LOSF  : 1KiB <= file size < 1MiB
#             4.2 Medium: 1MiB <= file size < 1GiB
#             4.3 Large : 1GiB <= file size <= 1024GiB (i.e. 1TiB)
#             Nevertheless, if so desired, a user can use the -r
#             option to indicate the particular range to sweep,
#             e.g. -r s for LOSF range only. Only one range at a time
#             is supported.
#          6. Only hyperscale datasets are used, where the term
#             "hyperscale dataset" is defined as a dataset that
#             has overall size >= 1TB (terabyte), or contains
#             >= 1 million files, or both.
#          7. The test dataset naming convention used by the script
#             is: number_of_files x the common size of all files.  So,
#             2x512GiB means there are 2 files in this dataset and
#             each file is of the common size 512GiB.  If anyone of
#             such directories does not exist when the test is
#             launched, it will be created. But all will be left
#             nearly completely empty (using the elbencho -F option)
#             after the test so as to be ready for the next sweep.
#          8. The wrapper must be used with elbencho version 1.6.1 or
#             later! It will enforce this restriction within the
#             script!
#          9. Any user can run this script's help or dry-run mode. But to
#             use it for actual tests, one must be either root or have
#             the sudo root privilege.
#         10. The script has been statically checked with shellcheck
#             without any errors/warnings.
#
# Usage : Assuming mtelbencho.sh is available from the $PATH,
#         cd src_data_dir\
#         sudo mtelbencho.sh [-t N] [-r s|m|l] -b arg [-S fs_block_size]
#         [-v] [-n], where src_data_dir is the directory that holds
#         all test datasets, N is the desired thread number to run
#         elbencho. For other usage tips and examples, on the CLI, type
#         mtelbencho.sh -h for more info.
#
# References:
# 0. https://github.com/breuner/elbencho/blob/master/README.md
# 1. https://www.es.net/assets/Uploads/zettar-zx-dtn-report.pdf
#    Fig. 3 for an actual application of this bash wrapper
# 2. https://shellcheck.net/
#
# A few global variables. Contrary to a myth popular among newbie
# programmers, if judiciously employed, global variables simplify
# programming :D
#
# A terabyte is 1,000,000 megabytes. A megabytes is 1000000 bytes.
# So, a terabyte is 1000000 * 1000000 bytes. We require 
minimal_space=2000000000000
# preferred file descriptor limit for both unlimit -Hn and -Sn
fdlimit=262144
type='w'
verbose=
block_size=1m
dry_run=
buffered=
threads=$(nproc)
src_data_dir=/var/tmp
fs_block_size=4
cmd=

check_elbencho_version()
{
    local vs
    local mandatory
    mandatory="1.6"
    if ! command -v elbencho &> /dev/null
    then
        echo "elbencho could not be found. Abort!"
        exit 1
    fi
    vs=$(elbencho --version|grep ^Version|
             cut -d ':' -f 2|sed -e 's/^[ \t]*//' -e 's/\-[0-9]//')
    # Use bash's numeric context. Use the following to ensure that the
    # check remains functional with elbencho that's newer than 1.6.x
    if (( $(echo "$vs < $mandatory"|bc -l) )); then 
        echo "Installed elbencho too old. Abort!"
        exit 1
    fi    
}

#
# NOTE: the following function is already in graph_sweep.sh, so why
#       it's here again? Please note that this script can be run by
#       itself too!
#
# The mtelbencho.sh uses individual files for the large-file range
# sweep. Their corresponding file descriptors could be all opened at
# the same time. Thus, the system default (often set to a modest 1024)
# may not be enough.  This can be raised with root privileges. Since
# this script is supposed to be run under root, so we can and will do
# it below temporarily - an application should NEVER touch the
# system's default settings!  In the following, both the hard and soft
# file descriptor limits are set to the same and adequate value per
# our experience.
#
set_max_open_file_descriptors()
{
    local hfdl
    hfdl=$(ulimit -Hn)
    local sfdl
    sfdl=$(ulimit -Sn)
    if (( "$hfdl < "$fdlimit )); then
	ulimit -Hn "$fdlimit"
    fi
    if (( "$sfdl < "$fdlimit )); then
	ulimit -Sn "$fdlimit"
    fi
}

help()
{
    local help_str=''
    read -r -d '' help_str <<'EOF'
This script is a bash wrapper for the elbencho program by Sven Breuner.

Its purpose is to 
0. Facilitate the conduction of a storage "sweep" in a client of a POSIX 
   compliant file storage service, e.g. BeeGFS, NFS.
1. Make it easy to select
   1.1 A distributed POSIX compliant file system 
   1.2 Storage devices (e.g. NVMe SSDS) based on actual test results obtained
       in the user's environment
2. Show some best practices for using elbencho

Please see https://github.com/breuner/elbencho

Command line options:
    -h show this help
    -r the range to sweep, one of s: LOSF, m: medium, l: large.  If not 
       specified, a sweep covering all three ranges is carried out.
    -t the number of threads to run elbencho. Default is auto-detect
    -s the path to the directory holding test datasets
    -S filesystem (aka fs) block size - positive integer; must be 2th
       power. File systems can have block size from 512 to (usually)
       65536 bytes. The default blocksize in most Linux filesystems is
       4096 bytes. So, the default is 4(K aka KiB). Note: the 512 bytes
       file system block size is deemed obsolete and is not supported!
    -b block size. Number of mebibytes to read/write in a single operation. 
       Default is 1M as used by elbencho
    -B use buffered I/O
    -v verbose mode; to print out the test dataset and number of threads
    -n dry run; to print out tests that would have run

The usage: 
$ sudo mtelbencho.sh [-h][-n][-v][-r range][-t numthreads][-s dirpath]\
  [-S fs_block_size][-b block_size][-B][-v][-n]

Examples: 0. $ mtelbencho.sh -r s -t 56 -b 16 -v -n
          1. $ sudo mtelbencho.sh -r s -t 12 -b 16 -v
          2. # (date; mtelbencho.sh -r s -t 56 -b 16 -s \
               /data/zettar/zx/src/sweep -v; date)\
               |tee /tmp/s_"$(hostname)"_tests_"$(date +%F).txt"   
          3. # (date; mtelbencho.sh -r s -t 56 -b 16 -s \
               /data/zettar/zx/src/sweep -v; date)\
               |tee /tmp/s_"$(hostname)"_tests_"$(date +%F).txt"   
          4. # (date; mtelbencho.sh -r s -t 56 -b 16 -s \
               /data/zettar/zx/src/sweep -v; date)\
               |tee /tmp/s_"$(hostname)"_tests_"$(date +%F).txt"   

In the above examples 
0   shows to carry out a verbose (-v) dry run (-n)
1   shows to use sudo to carry out a sweep for the LOSF range after the 
    cd "$src_data_dir" has been done
2-4 show a way to save the results of a sweep to a file for further
    processing.  Note, for graphing sweep results, please see the
    companiaon graph_sweep.sh
EOF
    echo "$help_str"
    exit 0
}

# Helper functions to the following main functions
check_space_available()
{
    if ! [[ -d "$src_data_dir" ]]; then
        echo "$src_data_dir doesn't exist! Abort!"
        exit 1
    fi
    local sa
    # The df man page states that SIZE units default to 1024 bytes
    sa=$(df $src_data_dir|tail -1|awk '{ printf "%.0f", $4*1024 }')
    [[ "$verbose" ]] && echo "Space availability now is: $sa bytes."
    [[ "$verbose" ]] && echo "Minimal requirement is   : $minimal_space bytes."
    if [[ "$sa" -lt "$minimal_space" ]]; then
        local msg
        msg="Any storage sweep is carried out using hyperscale datasets, i.e.\n"
        msg+="each dataset's overall size >= 1TB or has >= 1M files or both.\n"
        msg+="As a precaution, 2TB free space is required to run a sweep.\n"
        msg+="Please free up more space and then re-run. Aborting now."
        echo -e "$msg"
        exit 1
    fi
}

set_full_dataset_path()
{
    local dataset
    dataset=$1
    if [[ -z "$src_data_dir" ]]; then
        # I assume that you have cd src_data_dir already!
        dataset="./$dataset"
    else
        dataset="$src_data_dir/$dataset"
    fi
    echo "$dataset"
}

ensure_dataset_exists()
{
    local dataset
    dataset=$1
    if [[ -z "$dry_run" ]]; then
        if ! [[ -d "$dataset" ]] ; then
            mkdir "$dataset" || (echo "Couldn't create $dataset!" && exit 1)
        fi
    fi
}

dry_or_real_run()
{
    if [[ "$dry_run" ]]; then               
        echo "$cmd"
    else
        $cmd
    fi
}

# Three main functions for the following:
# o los_files()   : 1KiB <= file size < 1MiB
# o medium_files(): 1MiB <= file size < 1GiB
# o large_files() : 1GiB <= file size <= 1024GiB (i.e. 1TiB)
#
# The applicable elbencho syntaxes for the three ranges are subtly
# different.
#
# Note. The term LOSF stands for "Lots of Small Files", a term that is
# widely used in the U.S. DOE national lab community.  

los_files() {
    local number_of_files
    number_of_files=1048576
    local power_base
    power_base=2
    local file_per_thread
    local threads_less_one
    threads_less_one=$((threads-1))
    local combo
    combo=$((number_of_files+threads_less_one))
    file_per_thread=$((combo/threads))
    local cmd
    local end
    end=10

    for ((i=0;i<end;i++));
    do
        local file_size_multiplier
        file_size_multiplier=$(echo "$power_base^${i}" | bc)
        local dataset
        dataset="${number_of_files}x${file_size_multiplier}KiB"
        dataset=$(set_full_dataset_path "$dataset")
        ensure_dataset_exists "$dataset"
        if [[ "$verbose" ]]; then
            echo "Working on $dataset with $threads threads..."
        fi
        
        cmd="elbencho --dirsharing -$type -t $threads --nolive "
        cmd+="-F -d -n 1 -N $file_per_thread "
        cmd+="-s ${file_size_multiplier}k --trunctosize "
        cmd+="-b $block_size --dropcache --nodelerr "
        
        # Almost all known modern file systems have a default file
        # system block size value 4096 bytes (4 KiB). For a file whose
        # size is smaller than this value, direct IO is not feasible
        # so not specified.
        if [[ "$file_size_multiplier" -lt "$fs_block_size" ]] ||
               [[ "$buffered" ]]; then
            cmd+="$dataset"
        else
            cmd+="--direct $dataset"
        fi
        dry_or_real_run
    done
}

medium_files()
{
    local number_of_files
    number_of_files=1048576
    local power_base
    power_base=2
    #local cmd
    local end
    end=10
    
    for ((i=0;i<end;i++));
    do
        local file_size_multiplier
        file_size_multiplier=$(echo "$power_base^${i}" | bc)
        local dataset
        dataset="${number_of_files}x${file_size_multiplier}MiB"
        dataset=$(set_full_dataset_path "$dataset")
        ensure_dataset_exists "$dataset"
        local file_per_thread
        local threads_less_one
        threads_less_one=$((threads-1))
        local combo
        combo=$((number_of_files+threads_less_one))
        file_per_thread=$((combo/threads))
        if [[ "$verbose" ]]; then
            echo "Working on $dataset with $threads threads..."
        fi

        cmd="elbencho --dirsharing -$type -t $threads --nolive "
        cmd+="-F -d -n 1 -N $file_per_thread "
        cmd+="-s ${file_size_multiplier}m --trunctosize "
        cmd+="-b $block_size --dropcache --nodelerr "
        
        if ! [[ "$buffered" ]]; then
            cmd+="--direct "
        fi
        cmd+="$dataset"
        dry_or_real_run
        number_of_files=$(echo "$number_of_files/2"|bc)
    done
}

large_files()
{
    local number_of_files
    number_of_files=1024
    local power_base
    power_base=2
    local cmd
    local end
    end=11
    
    for ((i=0;i<end;i++));
    do
        local file_size_multiplier
        file_size_multiplier=$(echo "$power_base^${i}" | bc)
        local dataset
        dataset="${number_of_files}x${file_size_multiplier}GiB"
        dataset=$(set_full_dataset_path "$dataset")
        ensure_dataset_exists "$dataset"
        local upper
        upper=$(( number_of_files-1 ))
        if [[ "$verbose" ]]; then
            echo "Working on $dataset with $threads threads..."
        fi
        
        cmd="elbencho -$type -t $threads --nolive -F "
        cmd+="-s ${file_size_multiplier}g --trunctosize "
        cmd+="-b $block_size --dropcache --nodelerr "
        
        if ! [[ "$buffered" ]]; then
            cmd+="--direct "
        fi       
        if [[ "$upper" -eq 1 ]]; then
            cmd+="$dataset/f0 "
        else
            cmd+="$(eval echo "$dataset/f{0..$upper}")"
        fi
        dry_or_real_run     
        number_of_files=$(echo "$number_of_files/2"|bc)
    done
}

# A few more helper functions

run_as_root()
{
    local euid
    euid=$(id -u)
    if [[ "$euid" -ne 0 ]]; then
        local msg
        msg="elbencho --dropcache must be run under root to work. Abort!"
        echo "$msg"
        exit 1
    fi
}

verify_range()
{
    local range_to_sweep
    range_to_sweep=$1
    local msg
    
    if [[ "$range_to_sweep" != [sml] ]]; then
        msg="Only s:LOSF, m:medium files, l:large files "
        msg+="allowed. Abort!"
        echo "$msg"
        exit 1
    fi    
}

verify_threads()
{
    # Note, threads is a global variable :)
    local msg
    
    if ! [[ "$threads" =~ ^[1-9][0-9]*$ ]]; then
        msg="threads must be a positive integer. Abort!"
        echo "$msg"
        exit 1
    fi    
}

verify_src_data_dir()
{
    # For the dry-run mode, skip the src_data_dir check
    [[ "$dry_run" ]] && return 0
    if ! [[ "$(cd "$src_data_dir")" -eq 0 ]]; then
        msg="$src_data_dir does not exist. Abort!"
        echo "$msg"
        exit 1
    fi
}

verify_block_size()
{
    # Note, block_size is a global variable :)
    local msg
    
    if ! [[ "$block_size" =~ ^[1-9][0-9]*$ ]]; then
        msg="block_size must be specified in a positive integer. Abort!"
        echo "$msg"
        exit 1
    else
        block_size="${block_size}m"
    fi
}

is_power_of_two()
{
    if ! ((fs_block_size > 0 &&
                (fs_block_size & (fs_block_size - 1)) == 0 )); then
        msg="fs_block_size must be specified in a positive integer "
        msg+="in 2th power.  Abort!"
        echo "$msg"
        exit 1
    fi
}

show_test_duration()
{
    local total_test_time
    total_test_time=$1
    local t_hsec
    local t_hor
    local t_min
    local t_sec
    
    t_hor=$((total_test_time / 3600))
    if [[ "$t_hor" -gt 0 ]]; then
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

show_option_settings()
{
    echo "range_to_sweep: $range_to_sweep"
    echo "threads       : $threads"
    echo "src_data_dir  : $src_data_dir"
    echo "fs_block_size : $fs_block_size"
    echo "block_size    : $block_size"
    echo "buffered      : $buffered"
    echo "verbose       : $verbose"
    echo "dry_run       : $dry_run"
    echo "cmd           : $cmd"
}

# main()
{
    begin_test=$(date +"%s")
    while getopts ":hr:t:s:S:b:Bvn" opt; do
        case $opt in
            h)  help
                ;;
            r)  range_to_sweep=$OPTARG
                verify_range "$range_to_sweep"
                ;;          
            t)  threads=$OPTARG
                verify_threads
                ;;
            s)  src_data_dir=$OPTARG
                verify_src_data_dir
                ;;
            S)  fs_block_size=$OPTARG
                is_power_of_two
                ;;
            b)  block_size=$OPTARG
                verify_block_size
                ;;
            B)  buffered=1
                ;;          
            v)  verbose=1
                ;;
            n)  dry_run=1
                ;;
            *)  echo "Error: invalid option given!"
                exit 2
                ;;
        esac
    done
    [[ "$verbose" ]] && show_option_settings
    check_space_available
    if ! [[ "$dry_run" ]]; then
        run_as_root
    fi
    if ! [[ "$dry_run" ]]; then
        check_elbencho_version
	set_max_open_file_descriptors
    fi
    if [[ "$range_to_sweep" == 's' ]]; then
        los_files
    elif [[ "$range_to_sweep" == 'm' ]]; then
        medium_files
    elif [[ "$range_to_sweep" == 'l' ]]; then
        large_files
    else
        los_files
        medium_files
        large_files
    fi
    [[ "$verbose" ]] && echo "===> $0 all done :)"
    end_test=$(date +"%s")
    total_test_time=$((end_test-begin_test))
    [[ "$verbose" ]] && show_test_duration "$total_test_time"
    exit 0
} # end of main
