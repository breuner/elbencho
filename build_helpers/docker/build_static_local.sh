#!/bin/bash
#
# Build static executable in Alpine container and prune container after build.
# Call this script from the repository root dir.
#
# Note: "--privileged" is required to enable user change on RHEL7.
# Note: For podman's docker interface (e.g. RHEL8) remove "sudo -u builduser",
#       because it already runs under the calling UID.

# NOTE on Alpine v3.17:
# * Alpine v3.18-3.20 fail on ARM64 with git clone error "bad address".
# * Alpine v3.20+ requires "apk add c-ares-static zstd-static";
#   see Makefile alpine 3.20 hint for linker flags.
CONTAINER_NAME="elbencho-static"
IMAGE_NAME="alpine:3.17"            # see note on alpine 3.17 above
ELBENCHO_VERSION=$(make version)

ALTHTTPSVC_SUPPORT="${OVERRIDE_ALTHTTPSVC_SUPPORT:-1}"
S3_SUPPORT="${OVERRIDE_S3_SUPPORT:-1}"
USE_MIMALLOC="${OVERRIDE_USE_MIMALLOC:-1}"

rm -f packaging/elbencho-\${ELBENCHO_VERSION}-static-$(uname -m).tar.gz

docker rm $CONTAINER_NAME

docker pull $IMAGE_NAME && \
docker run --name $CONTAINER_NAME --privileged -i -v $PWD:$PWD -w $PWD $IMAGE_NAME \
    sh -c "\
    apk add bash boost-dev build-base gcc g++ git libaio-dev make numactl-dev \
        cmake curl-dev curl-static openssl-libs-static ncurses-static \
        boost-static ncurses zlib-static libretls-static nghttp2-static \
        brotli-static ncurses-dev sudo tar libidn2-static libunistring-static \
        libpsl-static && \
    apk update && apk upgrade && \
    adduser -u $UID -D builduser && \
    sudo -u builduser git config --global submodule.fetchJobs 0 && \
    sudo -u builduser git config --global fetch.parallel 0 && \
    sudo -u builduser make clean-all && \
    sudo -u builduser make -j $(nproc) \
        BACKTRACE_SUPPORT=0 ALTHTTPSVC_SUPPORT=$ALTHTTPSVC_SUPPORT \
        S3_SUPPORT=$S3_SUPPORT USE_MIMALLOC=$USE_MIMALLOC BUILD_STATIC=1" && \
docker rm $CONTAINER_NAME && \
cd bin/ && \
./elbencho --version && \
sh -c "file elbencho | grep static" && \
cp -v elbencho elbencho-${ELBENCHO_VERSION}-static-$(uname -m) && \
tar -czf ../packaging/elbencho-${ELBENCHO_VERSION}-static-$(uname -m).tar.gz elbencho && \
cd .. && \
echo "All Done. Your tar file is here:" && \
find ./packaging -maxdepth 1 -name "*-static-*.tar.gz" && \
exit 0

echo CANCELLED DUE TO ERROR && \
exit 1
