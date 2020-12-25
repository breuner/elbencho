#!/bin/bash
#
# GNU General Public License v3.0; it is intended to be always in sync
# with the license term employed by the elbencho itself.
#
# Name   : mtelbencho.sh (mt: multiple test)
#
# Authors: Chin Fang <fangchin@zettar.com>
#
# Purpose: A simple bash wrapper of the elbencho program by Sven Breuner.
#          The purpose of this bash wrapper is to:
#
#          0. Facilitate the conduction of a storage "sweep" in a
#             client of a POSIX compliant storage service, e.g. BeeGFS, NFS.
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
#             by a user with sudo priviledge.
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
#          5. mtelbencho.sh sweeps from 1KiB in 2th power increment
#             all the way up to 1TiB, over three file size ranges:
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
#             each file is of the common size 512GiB
#          8. The wrapper must be used with elbencho version 1.6.1 or
#             later! It will enforce this restriction within the
#             script!
#          9. Any user can run this script's help or dry-run mode. But to
#             use it for actual tests, one must be either root or have
#             sudo privilege.
#         10. The script has been statically checked with shellcheck
#             without any errors/warnings.
#
# Usage  : Assuming mtelbencho.sh is available from the $PATH,
#          cd src_data_dir; \
#          sudo mtelbencho.sh [-t N] [-r s|m|l] -b arg [-v] [-n],
#          where N is the desired thread number to run elbencho.
#          Do a mtelbencho.sh -h for more info.
#
# Reference: 0. https://github.com/breuner/elbencho/blob/master/README.md
#            1. mtelbencho.md
#            2. https://www.es.net/assets/Uploads/zettar-zx-dtn-report.pdf
#               Fig. 3 for an actual application of this bash wrapper

# A few global variables. Contrary to a myth popular among newbie
# programmers, if judiciously employed, global variables simplify
# programming :D
type='w'
verbose=
block_size=1m
no_opt=
buffered=
threads=$(nproc)

#
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
    vs=$(elbencho --version|grep ^Version|cut -d ':' -f 2)
    if ! [[ $vs == *"$mandatory"* ]]; then 
        echo "Installed elbencho too old. Abort!"
        exit 1
    fi
}

help()
{
    local help_str=''
    read -r -d '' help_str <<EOF
This script is a simple bash wrapper for the elbencho program by Sven Breuner.
Its purpose is to 
0. Facilitate the conduction of a storage "sweep" in a client of a POSIX 
   compliant storage service, e.g. BeeGFS, NFS.
1. Make it easy to select
   1.1 A distributed POSIX compliant file system 
   1.2 Storage devices (e.g. NVMe SSDS) based on actual test results obtained
       in the user's environment
2. Show some best practices for using elbencho

Please see https://github.com/breuner/elbencho

Command line options:
    -h show this help
    -r the range to sweep, one of s: LOSF, m: medium, l: large
    -t the number of threads to run elbencho. Default is auto-detect
    -s the path to the directory holding test datasets
    -b block size. Number of mebibytes to read/write in a single operation. 
       Default is 1M as used by elbencho
    -B use buffered I/O
    -v verbose mode; to print out the test dataset and number of threads
    -n dry run; to print out tests that would have run

The usage: 
$ sudo mtelbencho [-h][-n][-v][-r range][-t numthreads][-s dirpath][-b block_size][-B][-v][n]
Examples: 0. $ mtelbencho.sh -r s -t 56 -b 16 -v -n
          1. $ sudo ./mtelbencho.sh -r s -t 12 -b 16 -s /var/tmp -v
EOF
    echo "$help_str"
    exit 0
}

# A helper function to the following main functions
ensure_dataset_exists()
{
    local dataset
    dataset=$1
    if ! [[ -d "$dataset" ]] && ! [[ "$no_opt" ]]; then
	mkdir "$dataset" || echo "Couldn't create $dataset!" && exit 1
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
    end=9
    
    for ((i=0;i<=end;i++));
    do
	local file_size_multiplier
	file_size_multiplier=$(echo "$power_base^${i}" | bc)
	local dataset
	dataset="${number_of_files}x${file_size_multiplier}KiB"
	ensure_dataset_exists "$dataset"
	if [[ $verbose ]]; then
	    echo "Working on $dataset with $threads threads..."
	fi
	if [[ "$no_opt" ]]; then
	    if [[ "$file_size_multiplier" -lt 4 ]]; then
		cmd="elbencho --dirsharing -$type -t $threads --nolive "
		cmd+="-F -d -n 1 -N $file_per_thread "
		cmd+="-s ${file_size_multiplier}k --trunctosize "
		cmd+="-b $block_size --dropcache --nodelerr ./$dataset"
		echo "$cmd"
	    else
		if [[ "$buffered" ]]; then
		    cmd="elbencho --dirsharing -$type -t $threads --nolive "
		    cmd+="-F -d -n 1 -N $file_per_thread "
		    cmd+="-s ${file_size_multiplier}k --trunctosize "
		    cmd+="-b $block_size --dropcache --nodelerr ./$dataset"
		    echo "$cmd"
		else
		    cmd="elbencho --dirsharing -$type -t $threads --nolive "
		    cmd+="-F -d -n 1 -N $file_per_thread "
		    cmd+="-s ${file_size_multiplier}k --trunctosize "
		    cmd+="-b $block_size --direct --dropcache "
		    cmd+="--nodelerr  ./$dataset"
		    echo "$cmd"
		fi
            fi	
	else
	    if [[ "$file_size_multiplier" -lt 4 ]]; then
		cmd="elbencho --dirsharing -$type -t $threads --nolive "
		cmd+="-F -d -n 1 -N $file_per_thread "
		cmd+="-s ${file_size_multiplier}k --trunctosize "
		cmd+="-b $block_size --dropcache --nodelerr ./$dataset"
		$cmd
	    else
		if [[ "$buffered" ]]; then
		    cmd="elbencho  --dirsharing -$type -t $threads --nolive "
		    cmd+="-F -d -n 1 -N $file_per_thread -s "
		    cmd+="${file_size_multiplier}k --trunctosize "
		    cmd+="-b $block_size --dropcache --nodelerr ./$dataset"
		    $cmd
		else
		    cmd="elbencho --dirsharing -$type -t $threads --nolive -F "
		    cmd+="-d -n 1 -N $file_per_thread "
		    cmd+="-s ${file_size_multiplier}k --trunctosize "
		    cmd+="-b $block_size --direct --dropcache "
		    cmd+="--nodelerr  ./$dataset"
		    $cmd
		fi
            fi	
	fi
    done
}

medium_files()
{
    local number_of_files
    number_of_files=1048576
    local power_base
    power_base=2
    local cmd
    local end
    end=9
    
    for ((i=0;i<=end;i++));
    do
	local file_size_multiplier
	file_size_multiplier=$(echo "$power_base^${i}" | bc)
	local dataset
	dataset="${number_of_files}x${file_size_multiplier}MiB"
	ensure_dataset_exists "$dataset"
	local file_per_thread
	local threads_less_one
	threads_less_one=$((threads-1))
	local combo
	combo=$((number_of_files+threads_less_one))
	file_per_thread=$((combo/threads))
	if [[ $verbose ]]; then
	    echo "Working on $dataset with $threads threads..."
	fi
	if [[ "$no_opt" ]]; then
	    if [[ "$buffered" ]]; then		
		cmd="elbencho --dirsharing -$type -t $threads --nolive "
		cmd+="-F -d -n 1 -N $file_per_thread "
		cmd+="-s ${file_size_multiplier}m --trunctosize "
		cmd+="-b $block_size --dropcache --nodelerr ./$dataset"
		echo "$cmd"
	    else
		cmd="elbencho --dirsharing -$type -t $threads --nolive "
		cmd+="-F -d -n 1 -N $file_per_thread "
		cmd+="-s ${file_size_multiplier}m --trunctosize "
		cmd+="-b $block_size --direct --dropcache "
		cmd+="--nodelerr ./$dataset"
		echo "$cmd"
	    fi
	else
	    if [[ "$buffered" ]]; then		
		cmd="elbencho --dirsharing -$type -t $threads --nolive "
		cmd+="-F -d -n 1 -N $file_per_thread"
		cmd+="-s ${file_size_multiplier}m --trunctosize "
		cmd+="-b $block_size --dropcache --nodelerr ./$dataset"
		$cmd
	    else
		cmd="elbencho --dirsharing -$type -t $threads --nolive "
		cmd+="-F -d -n 1 -N $file_per_thread "
		cmd+="-s ${file_size_multiplier}m --trunctosize "
		cmd+="-b $block_size --direct --dropcache "
		cmd+="--nodelerr ./$dataset"
		$cmd
	    fi
	fi
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
    end=10
    
    for ((i=0;i<=end;i++));
    do
	local file_size_multiplier
	file_size_multiplier=$(echo "$power_base^${i}" | bc)
	local dataset
	dataset="${number_of_files}x${file_size_multiplier}GiB"
	ensure_dataset_exists "$dataset"
	local upper
	upper=$(( number_of_files-1 ))
	if [[ $verbose ]]; then
	    echo "Working on $dataset with $threads threads..."
	fi
	if [[ "$upper" -eq 0 ]]; then
	    if [[ "$no_opt" ]]; then
		if [[ "$buffered" ]]; then
		    cmd="elbencho -$type -t $threads --nolive -F "
		    cmd+="-s ${file_size_multiplier}g --trunctosize "
		    cmd+="-b $block_size --dropcache "
		    cmd+="--nodelerr ./$dataset/f0"
		    echo "$cmd"
		else
		    cmd="elbencho -$type -t $threads --nolive -F "
		    cmd+="-s ${file_size_multiplier}g --trunctosize "
		    cmd+="-b $block_size --direct --dropcache "
		    cmd+="--nodelerr ./$dataset/f0"
		    echo "$cmd"
		fi
	    fi
	else
	    if [[ "$no_opt" ]]; then
		if [[ "$buffered" ]]; then		    
		    cmd="elbencho -$type -t $threads --nolive -F "
		    cmd+="-s ${file_size_multiplier}g --trunctosize "
		    cmd+="-b $block_size --dropcache --nodelerr "
		    cmd+="$(eval echo "./$dataset/f{0..$upper}")"
		    echo "$cmd"
		else
		    cmd="elbencho -$type -t $threads --nolive -F "
		    cmd+="-s ${file_size_multiplier}g --trunctosize "
		    cmd+="-b $block_size --direct --dropcache --nodelerr "
		    cmd+="$(eval echo "./$dataset/f{0..$upper}")"
		    echo "$cmd"
		fi
	    else
		if [[ "$buffered" ]]; then		    
		    cmd="elbencho -$type -t $threads --nolive -F "
		    cmd+="-s ${file_size_multiplier}g --trunctosize "
		    cmd+="-b $block_size --dropcache --nodelerr "
		    cmd+="$(eval echo "./$dataset/f{0..$upper}")"
		    $cmd
		else
		    cmd="elbencho -$type -t $threads --nolive -F "
		    cmd+="-s ${file_size_multiplier}g --trunctosize "
		    cmd+="-b $block_size --direct --dropcache --nodelerr "
		    cmd+="$(eval echo "./$dataset/f{0..$upper}")"
		    $cmd
		fi	    
	    fi
	fi
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

# main()
{
    while getopts ":hr:t:s:b:Bvn" opt; do
        case $opt in
            h)
                help
                ;;
            r)
                range_to_sweep=$OPTARG
		verify_range "$range_to_sweep"
                ;;          
            t)  threads=$OPTARG
		verify_threads
                ;;
            s)  src_dir=$OPTARG
		if ! [[ "$(cd $src_dir)" -eq 0 ]]; then
		    msg="$src_dir does not exist. Abort!"
                    echo "$msg"
		    exit 1
		fi
                ;;   		
            b)  block_size=$OPTARG
		verify_block_size
                ;;
            B)  buffered=1
                ;;	    
            v)  verbose=1
                ;;
            n)  no_opt=1
		;;
            *)  echo "Error: invalid option given!"
                exit 2
                ;;
        esac
    done
    run_as_root
    if ! [[ "$no_opt" ]]; then
	check_elbencho_version
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
    exit 0
} # end of main
