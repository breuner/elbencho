#!/bin/bash

# A terabyte is 1,000,000 megabytes. A megabytes is 1000000 bytes.
# So, a terabyte is 1000000 * 1000000 bytes. We require 
minimal_space=2000000000000
src_data_dir=/data/zettar/zx/src/sweep
#src_data_dir=.
verbose=1

check_space_available()
{
    if ! [[ -d "$src_data_dir" ]]; then
	echo "$src_data_dir doesn't exist! Abort!"
	exit 1
    fi
    local sa
    # The df man page states that SIZE units default to 1024 bytes
    sa=$(df $src_data_dir|tail -1|awk '{ printf "%d", $4*1024 }')
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

# main()
{
    check_space_available
    exit 0
}
