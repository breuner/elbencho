# elbencho plugin "s3rdma_cloudian": S3-over-RDMA via Cloudian's aws-sdk-cpp fork.
# Enable with ELB_PLUGIN_S3RDMA_CLOUDIAN=1 (see README.md in this directory).

PLUGIN_NAMES += s3rdma_cloudian

define PLUGIN_HELP_s3rdma_cloudian
	@echo '   ELB_PLUGIN_S3RDMA_CLOUDIAN=0|1 - S3-over-RDMA via the RDMA-enabled aws-sdk-cpp'
	@echo '                             fork of Cloudian. Requires S3_SUPPORT=1. The fork gets'
	@echo '                             built instead of the AWS SDK, unless AWS_LIB_DIR and'
	@echo '                             AWS_INCLUDE_DIR point to a pre-built fork. (Default: 0)'
endef

ifeq ($(ELB_PLUGIN_S3RDMA_CLOUDIAN),1)
  ifneq ($(S3_SUPPORT),1)
    $(error ELB_PLUGIN_S3RDMA_CLOUDIAN=1 requires S3_SUPPORT=1)
  endif
  ifeq ($(S3_AWSCRT),1)
    $(error ELB_PLUGIN_S3RDMA_CLOUDIAN=1 is not compatible with S3_AWSCRT=1)
  endif
  ifdef ELB_S3_TRANSPORT_HEADER
    $(error Only one plugin can replace the S3 transport, but ELB_S3_TRANSPORT_HEADER is already \
      set to $(ELB_S3_TRANSPORT_HEADER))
  endif

  ELB_S3_TRANSPORT_HEADER := S3RdmaCloudianTransport.h
  ELB_PLUGIN_S3RDMA_CLOUDIAN_PATH := $(PLUGINS_PATH)/s3rdma_cloudian/source

  ifneq ($(AWS_LIB_DIR),)
    # pre-built fork: it has libcloudian-aws-cpp-sdk-s3.a and libs2n.a instead of the stock s3 libs
    ifeq ($(wildcard $(AWS_INCLUDE_DIR)/aws/s3/RdmaPtr.h),)
      $(error ELB_PLUGIN_S3RDMA_CLOUDIAN=1: AWS_INCLUDE_DIR does not contain aws/s3/RdmaPtr.h, so \
        this is not the RDMA-enabled aws-sdk-cpp fork of Cloudian)
    endif

    PLUGINS_EXTERNALS_ENV += AWS_PREBUILT_LIBS="libaws-c-auth.a libaws-c-compression.a \
      libaws-c-http.a libaws-cpp-sdk-core.a libaws-crt-cpp.a libaws-c-cal.a \
      libaws-c-event-stream.a libaws-c-io.a libaws-c-s3.a libaws-c-common.a libaws-checksums.a \
      libaws-c-mqtt.a libaws-c-sdkutils.a libcloudian-aws-cpp-sdk-s3.a libs2n.a"
  else
    # let prepare-external.sh build the fork instead of the stock AWS SDK, with our patches
    PLUGINS_EXTERNALS_ENV += AWS_GIT_REPO=https://github.com/cloudian/aws-sdk-cpp.git \
      AWS_REQUIRED_TAG=1.11.893+rdma \
      AWS_PATCHES="$(abspath $(wildcard $(PLUGINS_PATH)/s3rdma_cloudian/patches/*.patch))"
  endif

  SOURCES  += $(shell find $(ELB_PLUGIN_S3RDMA_CLOUDIAN_PATH) -name '*.cpp')
  CXXFLAGS += -DELB_PLUGIN_S3RDMA_CLOUDIAN \
              -DELB_S3_TRANSPORT_HEADER='"$(ELB_S3_TRANSPORT_HEADER)"' \
              -I $(ELB_PLUGIN_S3RDMA_CLOUDIAN_PATH)

  PLUGINS_ENABLED += s3rdma_cloudian
endif
