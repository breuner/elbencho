#!/bin/bash

echo "A. Use the structure of the wrapper ..."
cmd="elbencho --dirsharing -w -t 56 --nolive "
cmd+="-F -d -n 1 -N 585 "
cmd+="-s 32m --trunctosize "
cmd+="-b 16m --direct --dropcache --nodelerr ./32768x32MiB"
$cmd

echo "B. Run the command directly ..."
echo "The command to execute..."
echo "elbencho  --dirsharing -w -t 56 --nolive -F -d -n 1 -N 585 -s 32m --trunctosize -b 16m --direct --dropcache --nodelerr  ./32768x32MiB"
elbencho  --dirsharing -w -t 56 --nolive -F -d -n 1 -N 585 -s 32m --trunctosize -b 16m --direct --dropcache --nodelerr  ./32768x32MiB
