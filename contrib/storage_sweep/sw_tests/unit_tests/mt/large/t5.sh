#!/bin/bash

echo "***** A. Use the eval technique ..."
end=127
cmd="elbencho -w -t 56 --nolive -F "
cmd+="-s 1g --trunctosize -b 16m --direct --dropcache --nodelerr "
cmd+="$(eval echo "./128x8GiB/f{0..$end}")"
echo "This is the command to execute..."
echo "$cmd"
$cmd

echo "**** B. Use the direct bash brace expansion ..."
cmd="elbencho -w -t 56 --nolive -F "
cmd+="-s 1g --trunctosize -b 16m --direct --dropcache --nodelerr "
# Never threat the braces as a string directly!
echo "This is the command to execute..."
echo "$cmd ./128x8GiB/f{0..127}"
$cmd ./128x8GiB/f{0..127}
