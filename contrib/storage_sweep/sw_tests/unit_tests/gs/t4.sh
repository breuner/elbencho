#!/bin/bash
num_sweep=20
##
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

# main()
{
    for ((i=0;i<num_sweep;i++));
    do
	echo "This is $(print_iter $i) iteration."
    done
    exit 0
}
