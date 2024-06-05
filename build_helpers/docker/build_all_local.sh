#!/bin/bash
#
# Build all Dockerfile.*.local from local repository and prune containers/images after each build.
# Call this script from the repository root dir.

NUM_DOCKERFILES_TOTAL=$(ls build_helpers/docker/Dockerfile.*.local | wc -l)
current_file_idx=1


for dockerfile in $(ls build_helpers/docker/Dockerfile.*.local); do
    echo
    echo "*** Building $(( current_file_idx++ ))/$NUM_DOCKERFILES_TOTAL: $dockerfile"
    echo
    
    echo "*** Cleaning up build artifacts..."
    make clean-all
    
    echo
    
    docker build --progress plain -t elbencho-local -f $dockerfile .
    if [ $? -ne 0 ]; then
        echo "ERROR: Docker build failed: $dockerfile"
        exit 1
    fi
    
    echo "*** Pruning docker containers and images..."
    docker container prune -f && docker image prune -fa && docker builder prune -af
done

echo
echo "*** All done."
