#!/bin/bash
#
# Build static executable in Alpine container and prune container after build.
# Call this script from the repository root dir.
#
# Note: "--privileged" is required to enable user change on RHEL7.

CONTAINER_NAME="elbencho-static"
IMAGE_NAME="alpine:3"
ELBENCHO_VERSION=$(make version)

rm -f packaging/elbencho-\${ELBENCHO_VERSION}-static-$(uname -m).tar.gz

docker rm $CONTAINER_NAME

docker pull $IMAGE_NAME && \
docker run --name $CONTAINER_NAME --privileged -it -v $PWD:$PWD -w $PWD $IMAGE_NAME \
    sh -c "\
    apk add bash boost-dev build-base gcc g++ git libaio-dev libexecinfo-dev make numactl-dev \
        cmake curl-dev curl-static openssl-libs-static ncurses-static libexecinfo-static \
        boost-static ncurses zlib-static libretls-static nghttp2-static \
        brotli-static ncurses-dev sudo && \
    adduser -u $UID -D -H builduser && \
    sudo -u builduser make clean-all && \
    sudo -u builduser make -j $(nproc) \
        LDFLAGS_EXTRA='-lexecinfo' S3_SUPPORT=1 USE_MIMALLOC=1 BUILD_STATIC=1" && \
docker rm $CONTAINER_NAME && \
cd bin/ && \
cp elbencho elbencho-${ELBENCHO_VERSION}-static-$(uname -m) && \
tar -czf ../packaging/elbencho-${ELBENCHO_VERSION}-static-$(uname -m).tar.gz elbencho && \
echo All Done && \
exit 0

echo CANCELLED DUE TO ERROR && \
exit 1
