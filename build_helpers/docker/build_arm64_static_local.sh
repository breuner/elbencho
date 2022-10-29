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
IMAGE_NAME="alpine:3.14"
ELBENCHO_VERSION=$(make version)

docker rm $CONTAINER_NAME

#docker pull $IMAGE_NAME && \
docker run --platform linux/arm64 --name $CONTAINER_NAME --privileged -it -v $PWD:$PWD -w $PWD $IMAGE_NAME \
    sh -c "\
    apk add bash boost-dev build-base gcc g++ git libaio-dev libexecinfo-dev make numactl-dev \
        cmake curl-dev curl-static openssl-libs-static ncurses-static libexecinfo-static \
        boost-static ncurses zlib-static libretls-static nghttp2-static \
        brotli-static ncurses-dev sudo tar && \
    adduser -u $UID -D -H builduser && \
    sudo -u builduser make clean-all && \
    sudo -u builduser make -j $(nproc) \
        LDFLAGS_EXTRA='-lexecinfo' S3_SUPPORT=1 USE_MIMALLOC=1 BUILD_STATIC=1 && \
    cd bin/ && \
    sudo -u builduser ./elbencho --version && \
    sudo -u builduser cp elbencho elbencho-${ELBENCHO_VERSION}-static-\$(uname -m) && \
    sudo -u builduser tar -czf ../packaging/elbencho-${ELBENCHO_VERSION}-static-\$(uname -m).tar.gz elbencho" && \
docker rm $CONTAINER_NAME && \
echo "All Done. Your tar file is here:" && \
find ./packaging -maxdepth 1 -name "*-static-*.tar.gz" && \
exit 0

echo CANCELLED DUE TO ERROR && \
exit 1
