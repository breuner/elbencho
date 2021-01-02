#!/bin/bash

dry_run=
output_dir=/var/tmp

ensure_file_exists()
{
    local f
    f=$1
    if [[ -z "$dry_run" ]]; then
	if [[ -f "$f" ]] && [[ -s "$f" ]]; then 
	    echo "$f exists and not empty"
	else 
	    echo "$f dose not exist or empty";
	    exit 1
	fi	
    fi
}
    
# main()
{
    i=0
    for f in "$output_dir"/p[0-4];
    do
	echo "Now checking $f ..."
	echo "Iteration $i ..."
	ensure_file_exists "$f"
	((++i))
    done
    exit 0
}
