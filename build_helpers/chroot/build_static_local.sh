#!/bin/bash
#
# Build static arm64 executable in Alpine chroot.
# This script bind-mounts the current dir into the chroot. Call it from the
# the repository root dir.
#
# For cross-compile on Ubuntu x86_64:
#  * Install binfmt support and qemu to enable running non-native executables:
#    $ sudo apt-get install qemu binfmt-support qemu-user-static
#  * Register multiarch support:
#    $ docker run --rm --privileged multiarch/qemu-user-static --reset -p yes 
#  * Try running an arm64 executable:
#    $ docker run --rm -t arm64v8/ubuntu uname -m 


CHROOT_PATH=${CHROOT_PATH:="/var/tmp/elbencho_chroot_$(whoami)"}
CHROOT_VERSION="v3.21"
ELBENCHO_VERSION=$(make version)
BUILD_ARCH=${BUILD_ARCH:="$(uname -m)"}                             # e.g. aarch64, x86_64
ALPINE_SCRIPT_PATH=${ALPINE_SCRIPT_PATH:="external/alpine-chroot-install"}

ALTHTTPSVC_SUPPORT="${OVERRIDE_ALTHTTPSVC_SUPPORT:-0}"
S3_SUPPORT="${OVERRIDE_S3_SUPPORT:-1}"
S3_AWSCRT="${OVERRIDE_S3_AWSCRT:-0}"
USE_MIMALLOC="${OVERRIDE_USE_MIMALLOC:-1}"

# Cross-compile is mem intensive, so limit to one proc per GB RAM
RAM_GB=$(grep MemTotal /proc/meminfo | awk '{printf "%."d"f\n", $2 / 1024 / 1024}')
RAM_GB=$((RAM_GB > 1 ? RAM_GB : 1))
NUM_JOBS=${NUM_JOBS:="$(( $(nproc) > RAM_GB ? RAM_GB : $(nproc) ))"}


################## START OF FUNCTION DEFINITIONS #################

# call chroot destroy script if it exists and exit with error.
cleanup_chroot_and_exit()
{
  if [ -f "$CHROOT_PATH/destroy" ]; then
  echo
  echo "*** Cleaning up chroot..."
    sudo "$CHROOT_PATH/destroy" --remove
  fi

  exit 1
}

################## END OF FUNCTION DEFINITIONS ###################


echo "Building static executable..."

echo
echo "BUILD_ARCH: $BUILD_ARCH; CHROOT_VERSION: $CHROOT_VERSION; NUM_JOBS: $NUM_JOBS"
echo "(RAM_GB: $RAM_GB; nproc: $(nproc))"

if [ -f "$CHROOT_PATH/destroy" ]; then
  echo
  echo "*** Cleaning up old chroot..."
  sudo "$CHROOT_PATH/destroy" --remove
fi

echo
echo "*** Cleaning build dir..."

# NOTE: clean-all has to come before chroot install script download, because clean-all will
#       delete the script, thus we run this outside of the chroot.
make clean-all

if [ $? -ne 0 ]; then
  echo "ERROR: Cleaning build dir failed."
  cleanup_chroot_and_exit
  exit 1
fi

echo
echo "*** Preparing chroot setup..."

wget https://raw.githubusercontent.com/alpinelinux/alpine-chroot-install/v0.14.0/alpine-chroot-install \
  -O "$ALPINE_SCRIPT_PATH"
echo "ccbf65f85cdc351851f8ad025bb3e65bae4d5b06  $ALPINE_SCRIPT_PATH" | sha1sum -c
chmod +x "$ALPINE_SCRIPT_PATH"

# note: "-i $(pwd)" bind-mounts the current dir into the chroot under the same path
sudo "$ALPINE_SCRIPT_PATH" -b "$CHROOT_VERSION" -a "$BUILD_ARCH" \
  -d "$(realpath "$CHROOT_PATH")" -i $(pwd) -p \
  "bash boost-dev build-base gcc g++ git libaio-dev make numactl-dev \
        cmake curl-dev curl-static openssl-libs-static ncurses-static \
        boost-static ncurses zlib-static libretls-static nghttp2-static \
        brotli-static ncurses-dev sudo tar libidn2-static libunistring-static \
        libpsl-static c-ares-dev zstd-static"

if [ $? -ne 0 ]; then
  echo "ERROR: Preparation of chroot failed."
  cleanup_chroot_and_exit
  exit 1 
fi

echo
echo "*** Building in chroot..."

sudo "$CHROOT_PATH/enter-chroot" -u $(whoami) make -C "$(pwd)" -j $NUM_JOBS \
        BACKTRACE_SUPPORT=0 ALTHTTPSVC_SUPPORT=$ALTHTTPSVC_SUPPORT S3_SUPPORT=$S3_SUPPORT \
        S3_AWSCRT=$S3_AWSCRT USE_MIMALLOC=$USE_MIMALLOC BUILD_STATIC=1

if [ $? -ne 0 ]; then
  echo "ERROR: Build failed."
  cleanup_chroot_and_exit
  exit 1 
fi

bin/elbencho --version && \
    file bin/elbencho | grep static && \
    cp -v bin/elbencho bin/elbencho-${ELBENCHO_VERSION}-static-"$BUILD_ARCH" && \
    tar -czf ./packaging/elbencho-${ELBENCHO_VERSION}-static-"$BUILD_ARCH".tar.gz -C bin ./elbencho

if [ $? -ne 0 ]; then
  echo "ERROR: Executable packaging failed."
  cleanup_chroot_and_exit
  exit 1 
fi

echo
echo "All Done. Your tar file is here:" && \
find ./packaging -maxdepth 1 -name "*-static-*.tar.gz" && \


if [ -f "$CHROOT_PATH/destroy" ]; then
  echo
  echo "*** Cleaning up chroot..."
  sudo "$CHROOT_PATH/destroy" --remove
fi

exit 0
