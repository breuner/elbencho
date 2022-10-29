#!/bin/bash
#
# GNU General Public License v3.0; it is intended to be always in sync
# with the license term employed by the elbencho itself.
#
# Name :   graph_sweep.sh - a bash wrapper for mtelbencho.sh and gnuplot
#
# Author : Chin Fang <fangchin@zettar.com>
#
# Purpose: Make the graphing of storage sweep results a push-button
#          operation.
#
# Remarks: 0. In the following, it is assumed that both mtelbencho.sh and
#             gnuplot are in the $PATH.
#          1. The wrapper enables you to select
#             1.0 The range to sweep (LOSF, Medium, Large, or Full. Default
#                 is a full sweep from 1KiB to 1TiB, as this provides the
#                 most comprehensive view of the storage system being
#                 benchmarked.
#             1.1 The number of times to repeat a desired sweep (default 3).
#          2. The format of the graph is set to svg. Nevertheless,
#             it's very easy to regenerate a plot based on your
#             preferences. See the descriptions for the two global
#             variable output and plot_file below.  The reason of the
#             choice is that very likely on a headless server, there
#             are no TrueType fonts installed, so svg is a safer
#             choice.  As an example, if the gnuplot term is set to
#             png, a popular graphics format, when running on a
#             server, the following warnings may appear:
#             gdImageStringFT: Could not find/open font while printing \
#             string 1048576x1KiB with font Verdana [...]
#          3. Since it runs mtelbencho.sh, so most options will be the
#             same as mtelbencho.sh's.  Please run mtelbencho.sh -h
#             and review the embedded Remarks in mtelbencho.sh for
#             more info.
#          4. As running mtelbencho.sh needs root privileges, so does
#             graph_sweep.sh. Run in a root shell or via sudo, except
#             in dry-run mode.
#          5. It uses the paste(1) command to form a result file
#             as the input to gnuplot.
#
# Usage : Assuming graph_sweep.sh is available from the $PATH,
#
#         cd src_data_dir;\
#         sudo graph_sweep.sh [-r s|m|l][-t N][-S fs_block_size]\
#         [-b block_size][-B][-N num_sweep][-o output_dir][-v][-T][-p][-n]
#
#         where src_data_dir is the directory that holds all test
#         datasets, N is the desired thread number to run elbencho,
#         num_sweep is the number of times to repeat a sweep,
#         output_dir is where the mtelbencho.sh's output will be
#         stored. Furthermore, four subdirectories, 'losf', 'medium',
#         'large', and 'full' are pre-created to prevent users from
#         accidentally having their results overwritten by different
#         runs for different ranges.  For other usage tips and
#         examples, on the CLI, type graph_sweep.sh -h for more info.
#
# References:
# 0. https://github.com/breuner/elbencho/tree/master/contrib/storage_sweep
# 1. http://www.gnuplot.info
# 2. https://man7.org/linux/man-pages/man1/paste.1.html
#
# A few global variables. Contrary to a myth popular among newbie
# programmers, if judiciously employed, global variables simplify
# programming :-S
#
# A terabyte is 1,000,000 megabytes. A megabytes is 1000000 bytes.
# So, a terabyte is 1000000 * 1000000 bytes. We require 
minimal_space=2000000000000
# preferred file descriptor limit for both unlimit -Hn and -Sn
fdlimit=262144
range_to_sweep=
threads=$(nproc)
src_data_dir=/var/tmp
fs_block_size=4
block_size=1
buffered=
num_sweep=3
output_dir=/var/tmp
traditional=
push_button_plot=
#
# The following should be defined right at the beginning of the script
# so that for long duration sweeps that cross the midnight, the file
# names remain correct for post-sweep processing such as graphing.
# The global variable today is defined here for similar reasons.
#
today=$(date +%F)
out_file_base=$(hostname)_tests_"$today"
verbose=
dry_run=
#
# The two tools that this wrapper is used for.
#
mtelbencho=$(command -v mtelbencho.sh)
if [[ -z "$mtelbencho" ]]; then
    echo "mtelbencho.sh could not be found. Aborting!"
    exit
fi
gnuplot=/usr/bin/gnuplot
#
# The sweep data file to be plotted using gnuplot. The CSV format is
# chosen such that people can still use a spreadsheet app should they
# choose :-S
#
sweep_csv="$output_dir"/sweep.csv
#
# You can customize the following and run gnuplot "$sweep_gplt" to
# regenerate a plot to your liking, with your choice of terminal type,
# font, font size, color, line width, line color and so on. It is a
# simple, almost self-explanatory text file. Much simpler than a
# spreadsheet application :-S
#
sweep_gplt="$output_dir"/sweep.gplt
#
# The final svg file for the sweep plot
#
sweep_svg="$output_dir"/sweep.svg
#
# All labels used for the X-axis of a plot are stored in the following
# array.
#
declare -a xlabels=('1048576x1KiB'   '1048576x2KiB'   '1048576x4KiB'
                    '1048576x8KiB'   '1048576x16KiB'  '1048576x32KiB'
                    '1048576x64KiB'  '1048576x128KiB' '1048576x256KiB'
                    '1048576x512KiB' '1048576x1MiB'   '524288x2MiB'
                    '262144x4MiB'    '131072x8MiB'    '65535x16MiB'
                    '32768x32MiB'    '16384x64MiB'    '8192x128MiB'
                    '4096x256MiB'    '2048x512MiB'    '1024x1GiB'
                    '512x2GiB'       '256x4GiB'       '128x8GiB'
                    '64x16GiB'       '32x32GiB'       '16x64GiB'
                    '8x128GiB'       '4x256GiB'       '2x512GiB'
                    '1x1TiB'
                   )

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

check_app()
{
    local app=$1
    local msg
    
    if ! [[ -x "$app" ]]; then
        msg="$app is not installed."
        msg+="Please install it and then re-run this wizard. "
        msg+="Aborting now."
        echo "$msg"
        exit 1
    fi
}

check_dependencies()
{
    check_app "$mtelbencho"
    if [[ "$push_button_plot" ]]; then
        check_app "$gnuplot"
    fi
}

help()
{
    local help_str=''
    read -r -d '' help_str <<'EOF'
This script is a bash wrapper for mtelbencho.sh and gnuplot.

Its purpose is to make the graphing of storage sweep results a
push-button operation.

Command line options:
    -h show this help
    -r the range to sweep, one of s: LOSF, m: medium, l: large.  If not
       specified, a full sweep is carried out to be consistent with 
       mtelbencho.sh's usage
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
    -N number of interations for a sweep (default: 3)
    -o the path to the directory where the results are stored
    -T tranditional output (use GB/s instead of Gbps; default Gbps)
    -p generate plot directly on the server; this may be not
       acceptable at some sites which run the Minimal Install OS, or
       simply prohibited due to policy reasons
    -v verbose mode; to print out the test dataset, number of threads, and
       other settings
    -n dry-run; to print out tests that would have run

The usage: 
$ sudo graph_sweep.sh [-h][-r range][-t numthreads][-s dirpath]\
  [-S fs_block_size][-b block_size][-B][-N num_sweep][-o output_dir]\
  [-v][-T][-p][-n]

Examples: 0. $ graph_sweep.sh -r s -t 56 -s /data/zettar/zx/src/sweep -b 16 \
               -o /var/tmp -p -v -n
          1. # graph_sweep.sh -r s -s /var/local/zettar/sweep -b 16 -p -v -N 1
          2. # graph_sweep.sh -r m -s /var/local/zettar/sweep -b 16 -p -v -N 1
          3. # graph_sweep.sh -r l -s /var/local/zettar/sweep -b 16 -p -v -N 1
          4. # graph_sweep.sh -s /var/local/zettar/sweep -b 16 -p -v -N 1

In the above examples 
0. shows a dry run
1. shows to graph a single-run sweep for LOSF in one shot
2. shows to graph a single-run sweep for medium files in one shot
3. shows to graph a single-run sweep for large files in one shot
4. shows to graph a single-run full sweep for the full 1KiB-1TiB range in one shot
EOF
    echo "$help_str"
    exit 0
}

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

print_iter()
{
    local i
    i=$1
    if [[ "$i" -eq "0" ]]; then
        echo "1st"
    elif [[ "$i" -eq "1" ]]; then
        echo "2nd"
    elif [[ "$i" -eq "2" ]]; then
        echo "3rd"
    else
        echo "$((i+1))th"
    fi
}

ensure_file_exists()
{
    local f
    f=$1
    if [[ -z "$dry_run" ]]; then
        if [[ -f "$f" ]] && [[ -s "$f" ]]; then 
            :
        else 
            echo "$f dose not exist or empty";
            exit 1
        fi      
    fi
}
    
run_mtelbencho()
{
    local out_file
    local cmd
    for ((i=0;i<num_sweep;i++));
    do
        echo "***> This is $(print_iter "$i") sweep"
        out_file="$out_file_base"_"$i".txt
        out_file="$output_dir/$out_file"
        cmd="$mtelbencho -t $threads -b $block_size -s $src_data_dir "
        [[ "$fs_block_size" -ne "4" ]] && cmd+="-S $fs_block_size "
        [[ "$buffered" ]] && cmd+="-B "
        [[ "$verbose" ]] && cmd+="-v "
        if [[ -n "$range_to_sweep" ]]; then
            cmd+="-r $range_to_sweep "
        fi
        if [[ "$dry_run" ]]; then
            echo "$cmd|tee $out_file"
        else
            $cmd|tee "$out_file"
        fi
    done
}

extract_results_for_plotting()
{
    local out_files
    out_files="${out_file_base}_*.txt"
    out_files="$output_dir/$out_files"
    local pasted_file
    pasted_file="$output_dir"/plot.dat
    local i=0
    local pfile
    # If Gbps is preferred, use the following conversion:
    # 0.008388608 = 1024*1024*8/(1000*1000*1000.),
    # i.e. Mebibyte*8bit/byte)/Gigabyte
    # If GB/s is preferred, use the following conversion:
    # 0.00104858 = 1024x1024/(1000*1000*1000).
    # i.e. Mebibytes/Gigabyte
    local conv
    if [[ "$traditional" ]]; then
	conv=0.00104858
    else
	conv=0.008388608
    fi
    for f in $out_files
    do
        pfile="$output_dir"/p"$i"
        grep Throughput < "$f"\
            |awk -v cf="$conv" '{printf "%.3f\n", (($4 + $5)*cf/2)}' \
                 > "$pfile"
        ensure_file_exists "$pfile"
        ((i++))
    done
    # Join the p"$i" files into a single output file, using the default \t
    paste "$output_dir"/p* > "$pasted_file"
    ensure_file_exists "$pasted_file"
    # Create the final input file for gnuplot, stored in $sweep_csv, a global
    # variable :)
    local input
    input="$pasted_file"
    local i
    if [[ -z "$range_to_sweep" ]] || [[ "$range_to_sweep" == 's' ]] ; then
        i=0
    elif [[ "$range_to_sweep" == 'm' ]]; then
        i=10
    elif [[ "$range_to_sweep" == 'l' ]]; then
        i=20
    fi
    # The ending index doesn't pose a problem. The number of lines in
    # the input file automatically determines that.
    local mean_value 
    echo -e "Dataset,Mean-value" > "$sweep_csv"
    while IFS= read -r line
    do
        mean_value=$( echo "$line"|\
                          awk '{sum = 0; for (i = 1; i <= NF; i++) 
                          sum += $i; sum /= NF; print sum}')
        echo "${xlabels[$i]},$mean_value"
        ((i++))
    done < "$input" >>"$sweep_csv"
    ensure_file_exists "$sweep_csv"
    # Now we are ready to generate plot, either directly or optionally
    # retrieve the plot data thus generated to a workstation.
}

run_gnuplot()
{
    local gnuplot_settings=''
    local range
    if [[ "$range_to_sweep" == 's' ]]; then
        range='LOSF'
    elif [[ "$range_to_sweep" == 'm' ]]; then
        range='medium files'
    elif [[ "$range_to_sweep" == 'l' ]]; then
        range='large files'
    else
        range='full range'
    fi
    local speed
    if [[ "$traditional" ]]; then
	speed='GB/s'
    else
	speed='Gbps'
    fi
    read -r -d '' gnuplot_settings <<EOF
#set terminal eps          # tested
#set terminal pdfcairo     # tested
set terminal svg enhanced background rgb 'white' # look bad if no background
set title "Storage sweep for $range over $num_sweep runs on $today"
set title font "Times Bold, 14"
set xlabel 'Datasets'
set xlabel font "Times Bold,10"
set ylabel "Mean throughput ($speed)"
set ylabel font "Times Bold,10"
set key autotitle columnhead
set yrange [0:]
#
# Use the line plot and enable grid lines in both X and Y
# directions.
#
set style data line
set grid ytics xtics
set grid
#
# Set the linestyle (ls) for the grid and enable grid lines with specific
# linetype (lt) linecolor (lc).
#
set style line 100 lt 1 lc rgb "grey" lw 0.5 
set grid ls 100 
#
# Predefine three linewidth (lw) and linetype color, sequentially
# numbered.  You can add more should you wish. Uncomment one to select, 
# and set it below in the plot command.
#
set style line 101 lw 3 lt rgb "#FF8C00" # line color in drak orange
#set style line 102 lw 3 lt rgb "#0000FF" # line color in blue
#set style line 103 lw 4 lt rgb "#228B22" # line color in forestgreen
#
# Rotate the xtics 90 degrees to avoid clutter. Also make sure the
# font choice and size are good for viewing. The same for the legend.
#
set xtics rotate # rotate labels on the x axis
set xtics font "Verdana,8"

set key left top # legend placement
set key font "Verdana,8"
set key Left     # note the cap 'L'!  Left justify the key text
set key width -3 # shift the legend to the left

#
set output "$sweep_svg"
set datafile separator ','  # so that gnuplot understand we use a csv file
plot "$sweep_csv" using 2:xtic(1) ls 101
EOF
    echo "$gnuplot_settings" > "$sweep_gplt"
    ensure_file_exists "$sweep_gplt" 
    # if the push_button_plot global variable is true, then the script will
    # also generate plot directly on the server to achieve a true push
    # button operation :) Otherwise, the user have the option to generate
    # a plot easily with gnuplot or another app, such as a spreadsheet.
    if [[ "$push_button_plot" ]]; then
        gnuplot "$sweep_gplt"
    fi
}

# A few more helper functions

run_as_root()
{
    local euid
    euid=$(id -u)
    if [[ "$euid" -ne 0 ]]; then
        local msg
        msg="You must be root to run mtelbencho.sh. Abort!"
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

verify_directory_exists()
{
    local dir
    dir=$1
    if [[ "$(cd "$dir")" -ne 0 ]]; then
        msg="$dir does not exist. Abort!"
        echo "$msg"
        exit 1
    fi
}

verify_block_size()
{
    # Note, block_size is a global variable :)  Also, this one differs subtly
    # from its counterpart in mtelbencho.sh!
    local msg
    if ! [[ "$block_size" =~ ^[1-9][0-9]*$ ]]; then
        msg="block_size must be specified in a positive integer. Abort!"
        echo "$msg"
        exit 1
    fi
}

verify_num_sweep()
{
    # Note, num_sweep is a global variable :)
    local msg
    
    if ! [[ "$num_sweep" =~ ^[1-9][0-9]*$ ]]; then
        msg="num_sweep must be specified in a positive integer. Abort!"
        echo "$msg"
        exit 1
    fi
}

mitigate_human_errors()
{
    #
    # It has been observed enough times that some new users run
    # successive sweep runs without putting the output of each sesion
    # into different output directories using the -o option, then
    # wondering why some graphs are missing etc.  OK, we will protect
    # such users by unconditionally pre-create the four subdirectories
    # {losf,medium,large,full} even the -o is used
    # 
    mkdir -p "$output_dir"/{losf,medium,large,full}
    if [[ "$range_to_sweep" == 's' ]]; then
        output_dir="$output_dir/losf"
    elif [[ "$range_to_sweep" == 'm' ]]; then
        output_dir="$output_dir/medium"
    elif [[ "$range_to_sweep" == 'l' ]]; then
        output_dir="$output_dir/large"
    else
        output_dir="$output_dir/full"
    fi
}

show_elbencho_version()
{
    local vs
    if ! command -v elbencho &> /dev/null
    then
        echo "elbencho could not be found. Abort!"
        exit 1
    fi
    vs=$(elbencho --version)
    echo "$vs"
}

show_installed_sweep_tools()
{
    echo "graph_sweep.sh is installed at $(command -v graph_sweep.sh)"
    echo "mtelbencho.sh is installed at $(command -v mtelbencho.sh)"
}

show_option_settings()
{
    echo "range_to_sweep  : $range_to_sweep"
    echo "threads         : $threads"
    echo "src_data_dir    : $src_data_dir"
    echo "fs_block_size   : $fs_block_size"    
    echo "block_size      : $block_size"
    echo "num_sweep       : $num_sweep"
    echo "output_dir      : $output_dir"
    echo "buffered        : $buffered"
    echo "verbose         : $verbose"
    echo "traditional     : $traditional"
    echo "push_button_plot: $push_button_plot"
    echo "dry_run         : $dry_run"
    echo "sweep_csv       : $sweep_csv"
    echo "sweep_gplt      : $sweep_gplt"
    echo "sweep_svg       : $sweep_svg"
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

# main()
{
    begin_test=$(date +"%s")
    while getopts ":hr:t:s:S:b:N:o:BvTnp" opt; do
        case $opt in
            h)  help
                ;;
            r)  range_to_sweep=$OPTARG
                verify_range "$range_to_sweep"
                ;;          
            t)  threads=$OPTARG
                verify_threads
                ;;
            s)  src_data_dir=$OPTARG # dir check to be done later
                ;;
            S)  fs_block_size=$OPTARG
                is_power_of_two
                ;;
            b)  block_size=$OPTARG
                verify_block_size
                ;;
            N)  num_sweep=$OPTARG
                verify_num_sweep
                ;;
            o)  output_dir=$OPTARG   # dir check to be done later
                ;;
            B)  buffered=1
                ;;          
            v)  verbose=1
                ;;
	    T)  traditional=1
		;;
            p)  push_button_plot=1
                ;;
            n)  dry_run=1
                ;;
            *)  echo "Error: invalid option given!"
                exit 2
                ;;
        esac
    done
    mitigate_human_errors
    sweep_csv="$output_dir"/sweep.csv
    sweep_gplt="$output_dir"/sweep.gplt
    sweep_svg="$output_dir"/sweep.svg
    [[ "$verbose" ]] && show_elbencho_version
    [[ "$verbose" ]] && show_installed_sweep_tools
    [[ "$verbose" ]] && show_option_settings
    check_space_available
    [[ -z "$dry_run" ]] && verify_directory_exists "$src_data_dir"
    [[ -z "$dry_run" ]] && verify_directory_exists "$output_dir"
    [[ -z "$dry_run" ]] && set_max_open_file_descriptors
    [[ -z "$dry_run" ]] && check_dependencies
    [[ -z "$dry_run" ]] && run_as_root
    [[ "$verbose" ]] && echo "===> Getting ready to sweep..."
    run_mtelbencho
    [[ "$verbose" ]] && echo "===> Sweeps done. Extracting results..."
    [[ -z "$dry_run" ]] && extract_results_for_plotting
    [[ "$verbose" ]] && echo "===> Results extracted. Plotting..."
    [[ -z "$dry_run" ]] && run_gnuplot
    [[ "$verbose" ]] && echo "===> $0 all done :)"
    end_test=$(date +"%s")
    total_test_time=$((end_test-begin_test))
    [[ "$verbose" ]] && show_test_duration "$total_test_time"
    exit 0
} # end of main
