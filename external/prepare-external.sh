#!/bin/bash
#
# Prepare git clones and checkout the required tags of external sources.
# Simple-Web-Server will always be prepared when this is called.
# AWS SDK CPP will only be prepared when PREP_AWS_SDK=1 is set.
# Mimalloc will only be prepared when PREP_MIMALLOC=1 is set.

EXTERNAL_BASE_DIR="$(pwd)/$(dirname $0)"

# Clone Simple-Web-Server git repo and switch to required tag. Nothing to configure/build/install
# here.
prepare_webserver()
{
	local REQUIRED_TAG="v3.1.1"
	local CURRENT_TAG
	local CLONE_DIR="${EXTERNAL_BASE_DIR}/Simple-Web-Server"
	
	# change to external subdir if we were called from somewhere else
	cd "$EXTERNAL_BASE_DIR" || exit 1
	
	# clone if directory does not exist yet
	if [ ! -d "$CLONE_DIR" ]; then
	   echo "Cloning http server git repo..."
	   git clone https://gitlab.com/eidheim/Simple-Web-Server.git $CLONE_DIR
	   if [ $? -ne 0 ]; then
	      exit 1
	   fi
	fi
	
	# directory exists, check if we already have the right tag.
	# (this is the fast path for dependency calls from Makefile)
	cd "$CLONE_DIR" && \
	   CURRENT_TAG="$(git describe --tags)" && \
	   if [ "$CURRENT_TAG" = "$REQUIRED_TAG" ]; then
	      # All good, nothing to do
	      return 0;
	   fi && \
	   cd "$EXTERNAL_BASE_DIR"
	
	# we are not at the right tag, so try to check it out.
	# (fetching is relevant in case we update to a new required tag.)
	echo "Checking out http server tag ${REQUIRED_TAG}..."
	
	cd "$CLONE_DIR" && \
	   git fetch -q --all && \
	   git checkout -q ${REQUIRED_TAG} && \
	   echo "DONE: HTTP server sources prepared." && \
	   cd "$EXTERNAL_BASE_DIR" && \
	   return 0
	
	# something went wrong if we got here
	echo "ERROR: HTTP server source preparation failed."
	exit 1
}

# Prepare the AWS SDK static lib based on user-provided pre-built libs.
prepare_awssdk_prebuilt_libs()
{
	echo "Using pre-built AWS SDK libs..."

	local INSTALL_DIR="${EXTERNAL_BASE_DIR}/aws-sdk-cpp_install"
	local INSTALL_LIB_DIR="${EXTERNAL_BASE_DIR}/aws-sdk-cpp_install/lib"
	local AWS_LIBS="libaws-c-auth.a libaws-c-compression.a libaws-c-http.a libaws-cpp-sdk-core.a \
		libaws-crt-cpp.a libaws-c-cal.a libaws-c-event-stream.a libaws-c-io.a libaws-cpp-sdk-s3.a \
		libaws-c-s3.a libaws-c-common.a libaws-checksums.a libaws-c-mqtt.a \
		libaws-cpp-sdk-transfer.a"

	echo "Resolving given AWS SDK libs path: $AWS_LIB_DIR"
	
	# Temporarily move to build root
	
	cd "${EXTERNAL_BASE_DIR}/.."
	AWS_LIB_DIR=$(realpath -e "$AWS_LIB_DIR")

	[ $? -ne 0 ] && exit 1

	echo "Resolved AWS SDK libs path: $AWS_LIB_DIR"
	
	# Simple sanity check for provided AWS_LIB_DIR
	if [ ! -e "${AWS_LIB_DIR}/libaws-cpp-sdk-s3.a" ]; then
		echo "AWS_LIB_DIR invalid. File not found: ${AWS_LIB_DIR}/libaws-cpp-sdk-s3.a"
		exit 1
	fi

	# Simple sanity check for provided AWS_INCLUDE_DIR
	if [ -n "$AWS_INCLUDE_DIR" ] && [ ! -e "${AWS_INCLUDE_DIR}/aws/s3/S3Client.h" ]; then
		echo "AWS_INCLUDE_DIR invalid. File not found:" \
			"${AWS_INCLUDE_DIR}/aws/s3/S3Client.h"
		exit 1
	fi

	# Move back to external base dir

	cd "${EXTERNAL_BASE_DIR}"

	[ $? -ne 0 ] && exit 1

	mkdir -p "$INSTALL_LIB_DIR"

	[ $? -ne 0 ] && exit 1

	echo "Preparing static AWS SDK lib..."

	local MRI_FILE="${INSTALL_LIB_DIR}/libaws-sdk-all.mri"
	local LIB_FILE="${INSTALL_LIB_DIR}/libaws-sdk-all.a"

	# delete old mri file and old lib
	rm -f "$LIB_FILE" "$MRI_FILE"

	# create mri file for "ar"
	echo "create $LIB_FILE" > "$MRI_FILE"
	for lib in $AWS_LIBS; do 
		echo addlib ${AWS_LIB_DIR}/${lib} >> "$MRI_FILE"
	done
	echo "save" >> "$MRI_FILE"
	echo "end" >> "$MRI_FILE"

	# create static lib containing all AWS SDK static libs
	ar -M < "$MRI_FILE" && \
		echo "Created $(basename "${LIB_FILE}")" && \
		echo "DONE: AWS SDK prepared." && \
		return 0

	[ $? -ne 0 ] && exit 1
}

# If user did not provide pre-built AWS SDK libs, then prepare git clone and required tag. If clone
# didn't exist yet or if tag changed then configure build, install libs and build a single static 
# lib containing all static AWS SDK libs. If clone exitsted and tag changed, then clean/uninstall 
# previous build before switching tag.
prepare_awssdk()
{
	# check if user provides pre-built libs
	if [ -n "$AWS_LIB_DIR" ]; then
		prepare_awssdk_prebuilt_libs
		return 0; # AWS SDK build not required, so we're done here
	fi

	local REQUIRED_TAG="1.9.228-elbencho-tag"
	local CURRENT_TAG
	local CLONE_DIR="${EXTERNAL_BASE_DIR}/aws-sdk-cpp"
	local INSTALL_DIR="${EXTERNAL_BASE_DIR}/aws-sdk-cpp_install"
	
	# change to external subdir if we were called from somewhere else
	cd "$EXTERNAL_BASE_DIR" || exit 1
	
	# clone if directory does not exist yet
	if [ ! -d "$CLONE_DIR" ]; then
		echo "Cloning AWS SDK git repo..."
		git clone --recursive https://github.com/breuner/aws-sdk-cpp.git $CLONE_DIR
		if [ $? -ne 0 ]; then
			exit 1
		fi
	fi
	
	# directory exists, check if we already have the right tag.
	# (this is the fast path for dependency calls from Makefile)
	cd "$CLONE_DIR" && \
		CURRENT_TAG="$(git describe --tags)" && \
		if [ "$CURRENT_TAG" = "$REQUIRED_TAG" ]; then
			# Already at the right tag, so nothing to do
			return 0;
		fi && \
		cd "$EXTERNAL_BASE_DIR"

	# we need to change tag...
	
	# clean up and uninstall any previous build before we switch to new tag
	if [ -f "$CLONE_DIR/Makefile" ]; then
		echo "Cleaning up previous build..."
		cd "$CLONE_DIR" && \
		make uninstall && \
		make clean
		
		[ $? -ne 0 ] && exit 1
	fi
	
	# check out required tag
	# (fetching is relevant in case we update to a new required tag.)
	echo "Checking out AWS SDK tag ${REQUIRED_TAG}..."
	
	cd "$CLONE_DIR" && \
		git fetch --recurse-submodules -q --all && \
		git checkout -q ${REQUIRED_TAG} && \
		git submodule -q update --recursive && \
		cd "$EXTERNAL_BASE_DIR"
	
	[ $? -ne 0 ] && exit 1
	
	echo "Configuring build and running install..."
	cd "$CLONE_DIR" && \
		cmake . -DBUILD_ONLY="s3;transfer" -DBUILD_SHARED_LIBS=OFF -DCPP_STANDARD=14 \
			-DAUTORUN_UNIT_TESTS=OFF -DENABLE_TESTING=OFF \
			-DCMAKE_BUILD_TYPE=Release -DBYO_CRYPTO=ON \
			"-DCMAKE_INSTALL_PREFIX=$INSTALL_DIR" && \
		make -j $(nproc) install && \
		cd "$EXTERNAL_BASE_DIR"

	[ $? -ne 0 ] && exit 1
	
	echo "Preparing static AWS SDK lib..."
	
	local AWS_LIB_DIR="$(echo ${INSTALL_DIR}/lib*)"
	local MRI_FILE="${AWS_LIB_DIR}/libaws-sdk-all.mri"
	local LIB_FILE="${AWS_LIB_DIR}/libaws-sdk-all.a"
	
	# delete old mri file and old lib
	rm -f "$LIB_FILE" "$MRI_FILE"
	
	# create mri file for "ar"
	echo "create $LIB_FILE" > "$MRI_FILE"
	for lib in $(ls ${AWS_LIB_DIR}/*.a); do 
		echo addlib $lib >> "$MRI_FILE"
	done
	echo "save" >> "$MRI_FILE"
	echo "end" >> "$MRI_FILE"
	
	# create static lib containing all AWS SDK static libs
	ar -M < "$MRI_FILE" && \
		echo "Created $(basename "${LIB_FILE}")" && \
		echo "DONE: AWS SDK prepared." && \
		return 0
	
	[ $? -ne 0 ] && exit 1
}

# Prepare git clone and required tag.
prepare_mimalloc()
{
	local REQUIRED_TAG="v1.7.3"
	local CURRENT_TAG
	local CLONE_DIR="${EXTERNAL_BASE_DIR}/mimalloc"
	local INSTALL_DIR="${EXTERNAL_BASE_DIR}/mimalloc/build"
	
	# change to external subdir if we were called from somewhere else
	cd "$EXTERNAL_BASE_DIR" || exit 1
	
	# clone if directory does not exist yet
	if [ ! -d "$CLONE_DIR" ]; then
		echo "Cloning mimalloc git repo..."
		git clone https://github.com/microsoft/mimalloc.git $CLONE_DIR
		if [ $? -ne 0 ]; then
			exit 1
		fi
	fi
	
	# directory exists, check if we already have the right tag.
	# (this is the fast path for dependency calls from Makefile)
	cd "$CLONE_DIR" && \
		CURRENT_TAG="$(git describe --tags)" && \
		if [ "$CURRENT_TAG" = "$REQUIRED_TAG" ]; then
			# Already at the right tag, so nothing to do
			return 0;
		fi && \
		cd "$EXTERNAL_BASE_DIR"

	# we need to change tag...
	
	# clean up and uninstall any previous build before we switch to new tag
	if [ -f "$INSTALL_DIR/Makefile" ]; then
		echo "Cleaning up previous build..."
		cd "$INSTALL_DIR" && \
		make clean
		
		[ $? -ne 0 ] && exit 1
	fi
	
	# check out required tag
	# (fetching is relevant in case we update to a new required tag.)
	echo "Checking out mimalloc tag ${REQUIRED_TAG}..."
	
	cd "$CLONE_DIR" && \
		git fetch --recurse-submodules -q --all && \
		git checkout -q ${REQUIRED_TAG} && \
		git submodule -q update --recursive && \
		cd "$EXTERNAL_BASE_DIR"
	
	[ $? -ne 0 ] && exit 1
	
	echo "Configuring build and running install..."
	mkdir -p "$INSTALL_DIR" && \
		cd "$INSTALL_DIR" && \
		cmake .. && \
		make -j $(nproc) && \
		cd "$EXTERNAL_BASE_DIR" && \
		return 0

	[ $? -ne 0 ] && exit 1
}

########### End of function definitions ############

prepare_webserver

if [ "$PREP_AWS_SDK" = "1" ]; then
	prepare_awssdk
fi

if [ "$PREP_MIMALLOC" = "1" ]; then
	prepare_mimalloc
fi
