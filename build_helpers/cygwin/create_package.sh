#!/bin/bash
#
# This script creates a .zip package containing the .exe and cygwin .DLL files.
#
# Call this script from the elbencho repo root dir in a cygwin shell. The created package can run
# in a normal Windows command-prompt and does not require cygwin to be installed on the host.
#
# These are the required cygwin packages for the build process:
#   $ setup-x86_64.exe -q \
#     --packages=bash,vim,cmake,gcc-g++,git,libboost-devel,libncurses-devel,make,procps-ng,zip

PACKAGING_PATH="./packaging/cygwin"
PACKAGE_NAME="elbencho-windows.zip"

make clean-all || exit 1

rm -rf "$PACKAGING_PATH" || exit 1

mkdir -p "$PACKAGING_PATH" || exit 1

make CYGWIN_SUPPORT=1 -j $(nproc) || exit 1

cp -v bin/elbencho.exe "${PACKAGING_PATH}/" || exit 1

for i in $(ldd bin/elbencho.exe | grep -o '\/usr\/bin\/.*dll'); do
    cp -v $i "${PACKAGING_PATH}/" || exit 1
done

cd "$PACKAGING_PATH" || exit 1

zip -v "$PACKAGE_NAME" *.exe *.dll || exit 1

echo "ALL DONE. Your package is here: ${PACKAGING_PATH}/${PACKAGE_NAME}"
