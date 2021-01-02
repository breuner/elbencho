#!/bin/bash

src_dir=/var/tmp

cd_to_src_data_dir()
{
    if ! [[ "$(cd "$src_dir")" -eq 0 ]]; then
	msg="$src_dir does not exist. Abort!"
        echo "$msg"
	exit 1
    else
	echo "The src_data_dir is $src_dir"
    fi
}

# main()
{
    src_dir=/var/local
    cd_to_src_data_dir
    exit 0
}
    
