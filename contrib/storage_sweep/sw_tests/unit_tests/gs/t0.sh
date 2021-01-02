#!/bin/bash
out_file_base='nersc-tbn-6_tests_2020-12-29'
output_dir=/var/tmp
range_to_sweep='s'
sweep_csv="$output_dir"/sweep.csv
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

extract_results_for_plotting()
{
    local out_files
    out_files="${out_file_base}_*.txt"
    out_files="$output_dir/$out_files"
    local pasted_file
    pasted_file="$output_dir"/plot.dat
    local i=0
    local pfile
    # 0.008388608 = 1024*1024*8/(1000*1000*1000.), i.e. MiB*8(bps)/G(bps)
    for f in $out_files
    do
	pfile="$output_dir"/p"$i"
        grep Throughput < "$f"\
            |awk '{printf "%.3f\n", (($4 + $5)*0.008388608/2)}' \
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

# main()
{
    extract_results_for_plotting
    exit 0
}
