#!/bin/bash

fs_block_size=8

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

# main()
{
    is_power_of_two
    exit 0
}
