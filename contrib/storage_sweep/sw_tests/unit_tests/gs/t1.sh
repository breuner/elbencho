#!/bin/bash
num_sweep=3
today=$(date +%F)

output_dir=/var/tmp
range_to_sweep='s'
sweep_csv="$output_dir"/sweep.csv
#
sweep_gplt="$output_dir"/sweep.gplt
#
# The final png file for the sweep plot
#
sweep_svg="$output_dir"/sweep.svg
#

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
    read -r -d '' gnuplot_settings <<EOF
#set terminal eps          # tested
#set terminal pdfcairo     # tested
set terminal svg enhanced background rgb 'white' # look bad if no background
set title "Storage sweep for $range over $num_sweep runs on $today"
set title font "Times Bold, 14"
set xlabel 'Datasets'
set xlabel font "Times Bold,10"
set ylabel 'Mean throughput (Gbps)'
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
    # if the push_button_plot global variable is true, then the script will
    # also generate plot directly on the server to achieve a true push
    # button operation :) Otherwise, the user have the option to generate
    # a plot easily with gnuplot or another app, such as a spreadsheet.
    if [[ "$push_button_plot" ]]; then
        gnuplot "$sweep_gplt"
    fi
}

# main()
{
    run_gnuplot
    exit 0
}
