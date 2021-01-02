#!/bin/bash

echo "***** A. Use the eval technique ..."
end=1
cmd="elbencho -w -t 56 --nolive -F "
cmd+="-s 512g --trunctosize -b 16m --direct --dropcache --nodelerr "
cmd+="$(eval echo "./2x512GiB/f{0..$end}")"
echo "This is the command to execute..."
echo "$cmd"
$cmd

echo "B. Use the bash brace expansion directly ..."
echo "The command to execute..."
echo "elbencho -w -t 56 --nolive -F -s 512g --trunctosize -b 16m --direct --dropcache --nodelerr ./2x512GiB/f{0..1}"
elbencho -w -t 56 --nolive -F -s 512g --trunctosize -b 16m --direct --dropcache --nodelerr ./2x512GiB/f{0..1}
