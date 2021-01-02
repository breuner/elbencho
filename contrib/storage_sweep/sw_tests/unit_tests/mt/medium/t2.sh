#!/bin/bash

echo "A. Use the structure of the wrapper ..."
cmd="elbencho --dirsharing -w -t 56 --nolive "
cmd+="-F -d -n 1 -N 37 "
cmd+="-s 512m --trunctosize "
cmd+="-b 16m --direct --dropcache --nodelerr ./2048x512MiB"
$cmd

echo "B. Run the command directly ..."
echo "The command to execute..."
echo "elbencho  --dirsharing -w -t 56 --nolive -F -d -n 1 -N 37 -s 512m --trunctosize -b 16m --direct --dropcache --nodelerr  ./2048x512MiB"
elbencho  --dirsharing -w -t 56 --nolive -F -d -n 1 -N 37 -s 512m --trunctosize -b 16m --direct --dropcache --nodelerr  ./2048x512MiB
