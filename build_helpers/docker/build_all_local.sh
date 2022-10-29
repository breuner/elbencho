#!/bin/bash
#
# Build all Dockerfile.*.local from local repository and prune containers/images after each build.
# Call this script from the repository root dir.

for dockerfile in $(ls build_helpers/docker/Dockerfile.*.local); do
    echo
    echo "Building: $dockerfile"
    echo
    
    echo "Cleaning up build artifacts..."
    make clean-all
    
    echo
    
    docker build -t elbencho-local -f $dockerfile .
    if [ $? -ne 0 ]; then
        echo "ERROR: Docker build failed: $dockerfile"
        exit 1
    fi
    
    echo "Pruning docker containers and images..."
    docker container prune -f && docker image prune -fa
done

echo
echo "All done."
