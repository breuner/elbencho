#!/bin/bash

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

#main()
{
    check_elbencho_version
    exit 0
}
