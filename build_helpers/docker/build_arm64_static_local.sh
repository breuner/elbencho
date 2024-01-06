#!/bin/bash
#
# Build static arm64 executable in Alpine container and prune container after build.
# Call this script from the repository root dir.
#
# Note: "--privileged" is required to enable user change on RHEL7.
#
# For cross-compile on Ubuntu x86_64:
#  * Install binfmt support and qemu to enable running non-native executables:
#    $ sudo apt-get install qemu binfmt-support qemu-user-static
#  * Register multiarch support:
#    $ docker run --rm --privileged multiarch/qemu-user-static --reset -p yes 
#  * Try running an arm64 executable:
#    $ docker run --rm -t arm64v8/ubuntu uname -m 

CONTAINER_NAME="elbencho-static"
IMAGE_NAME="alpine:3"
ELBENCHO_VERSION=$(make version)

ALTHTTPSVC_SUPPORT="${OVERRIDE_ALTHTTPSVC_SUPPORT:-0}"
S3_SUPPORT="${OVERRIDE_S3_SUPPORT:-1}"
USE_MIMALLOC="${OVERRIDE_USE_MIMALLOC:-1}"

# Cross-compile is mem intensive, so limit to one proc per GB RAM
RAM_GB=$(grep MemTotal /proc/meminfo | awk '{printf "%."d"f\n", $2 / 1024 / 1024}')
RAM_GB=$((RAM_GB > 1 ? RAM_GB : 1))
NUM_JOBS=$(( $(nproc) > RAM_GB ? RAM_GB : $(nproc) ))

echo "Calculated NUM_JOBS: $NUM_JOBS (RAM_GB: $RAM_GB, nproc: $(nproc))"

docker rm $CONTAINER_NAME

#docker pull $IMAGE_NAME && \
docker run --platform linux/arm64 --name $CONTAINER_NAME --privileged -it -v $PWD:$PWD -w $PWD $IMAGE_NAME \
    sh -c "\
    apk add bash boost-dev build-base gcc g++ git libaio-dev make numactl-dev \
        c-ares-static cmake curl-dev curl-static openssl-libs-static ncurses-static \
        boost-static ncurses zlib-static libretls-static nghttp2-static \
        brotli-static ncurses-dev sudo tar libidn2-static libunistring-static && \
    apk update && apk upgrade && \
    adduser -u $UID -D builduser && \
    sudo -u builduser git config --global submodule.fetchJobs $NUM_JOBS && \
    sudo -u builduser git config --global fetch.parallel $NUM_JOBS && \
    sudo -u builduser make clean-all && \
    sudo -u builduser make -j $NUM_JOBS \
        BACKTRACE_SUPPORT=0 ALTHTTPSVC_SUPPORT=$ALTHTTPSVC_SUPPORT \
        S3_SUPPORT=$S3_SUPPORT USE_MIMALLOC=$USE_MIMALLOC BUILD_STATIC=1 && \
    cd bin/ && \
    sudo -u builduser ./elbencho --version && \
    sudo -u builduser sh -c \"file elbencho | grep static\" && \
    sudo -u builduser cp -v elbencho elbencho-${ELBENCHO_VERSION}-static-\$(uname -m) && \
    sudo -u builduser tar -czf ../packaging/elbencho-${ELBENCHO_VERSION}-static-\$(uname -m).tar.gz elbencho" && \
docker rm $CONTAINER_NAME && \
echo "All Done. Your tar file is here:" && \
find ./packaging -maxdepth 1 -name "*-static-*.tar.gz" && \
exit 0

echo CANCELLED DUE TO ERROR && \
exit 1
