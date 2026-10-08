# elbencho plugin "s3rdma_minio": GPU-direct S3-over-RDMA via NVIDIA cuObject.
# Enable with ELB_PLUGIN_S3RDMA_MINIO=1 (see README.md in this directory).

PLUGIN_NAMES += s3rdma_minio

define PLUGIN_HELP_s3rdma_minio
	@echo '   ELB_PLUGIN_S3RDMA_MINIO=0|1 - GPU-direct S3-over-RDMA via NVIDIA cuObject'
	@echo '                             (CUDA 13.1+). Requires S3_SUPPORT=1, libcuobjclient,'
	@echo '                             libibverbs and librdmacm. (Default: 0)'
	@echo '   CUOBJ_INCLUDE_PATH=<path> - Path to directory containing cuobjclient.h.'
	@echo '   CUOBJ_LIB_PATH=<path>     - Path to directory containing libcuobjclient.so.'
	@echo '                             (Default for both: pkg-config cuobjclient-*, else'
	@echo '                             search under /usr/local/cuda*)'
	@echo '   CUOBJ_SHIM=0|1            - Test build against the cuObject client shim in'
	@echo '                             tools/s3rdma/cuobjclient-shim instead of NVIDIA'"'"'s'
	@echo '                             library (RDMA over RC, no GPU or CUDA needed; works'
	@echo '                             only with tools/s3rdma/server). (Default: 0)'
endef

ifeq ($(ELB_PLUGIN_S3RDMA_MINIO),1)
  ifneq ($(S3_SUPPORT),1)
    $(error ELB_PLUGIN_S3RDMA_MINIO=1 requires S3_SUPPORT=1)
  endif
  ifeq ($(S3_AWSCRT),1)
    $(error ELB_PLUGIN_S3RDMA_MINIO=1 is not compatible with S3_AWSCRT=1)
  endif
  ifdef ELB_S3_TRANSPORT_HEADER
    $(error Only one plugin can replace the S3 transport, but ELB_S3_TRANSPORT_HEADER is already \
      set to $(ELB_S3_TRANSPORT_HEADER))
  endif

  ELB_S3_TRANSPORT_HEADER := S3RdmaMinioTransport.h
  ELB_PLUGIN_S3RDMA_MINIO_PATH := $(PLUGINS_PATH)/s3rdma_minio/source

  ifeq ($(CUOBJ_SHIM),1)
    # the shim implements the cuObject client API over RC on plain libibverbs (host memory only)
    CUOBJ_SHIM_PATH    := ./tools/s3rdma/cuobjclient-shim
    CUOBJ_INCLUDE_PATH := $(CUOBJ_SHIM_PATH)/include
    CUOBJ_LIB_PATH     := $(CUOBJ_SHIM_PATH)/lib
    CUOBJ_LIBS         := -lcuobjclient
    # found relative to bin/elbencho at run time, so that a copied tree keeps working
    LDFLAGS            += -Wl,-rpath,'$$ORIGIN/../tools/s3rdma/cuobjclient-shim/lib'

    $(EXE): $(CUOBJ_LIB_PATH)/libcuobjclient.so
    $(CUOBJ_LIB_PATH)/libcuobjclient.so:
	$(MAKE) -C $(CUOBJ_SHIM_PATH)
  else
    # cuObject discovery: versioned pkg-config module, else search under /usr/local/cuda*
    CUOBJ_PKGCONFIG_MODULE ?= $(shell pkg-config --list-all 2>/dev/null | cut -d' ' -f1 | \
                                grep '^cuobjclient-' | sort -V -r | head -n1)
    ifneq ($(CUOBJ_PKGCONFIG_MODULE),)
      CUOBJ_INCLUDE_PATH ?= $(shell pkg-config --variable=includedir $(CUOBJ_PKGCONFIG_MODULE))
      CUOBJ_LIB_PATH     ?= $(shell pkg-config --variable=libdir $(CUOBJ_PKGCONFIG_MODULE))
    endif
    CUOBJ_INCLUDE_PATH ?= $(shell find /usr/local/cuda/ /usr/local/cuda* -name cuobjclient.h \
                            -printf '%h\n' 2>/dev/null | head -n1)
    CUOBJ_LIB_PATH     ?= $(shell find /usr/local/cuda/ /usr/local/cuda* -name libcuobjclient.so \
                            -printf '%h\n' 2>/dev/null | head -n1)
    ifeq ($(CUOBJ_INCLUDE_PATH),)
      $(error ELB_PLUGIN_S3RDMA_MINIO=1: cuobjclient.h not found, set CUOBJ_INCLUDE_PATH)
    endif
    ifeq ($(CUOBJ_LIB_PATH),)
      $(error ELB_PLUGIN_S3RDMA_MINIO=1: libcuobjclient.so not found, set CUOBJ_LIB_PATH)
    endif

    CUOBJ_LIBS := -lcuobjclient -lcufile

    override CUDA_SUPPORT = 1 # cuObject needs the CUDA runtime
  endif

  SOURCES  += $(shell find $(ELB_PLUGIN_S3RDMA_MINIO_PATH) -name '*.cpp')
  CXXFLAGS += -DELB_PLUGIN_S3RDMA_MINIO -DELB_S3_TRANSPORT_HEADER='"$(ELB_S3_TRANSPORT_HEADER)"' \
              -I $(ELB_PLUGIN_S3RDMA_MINIO_PATH) -I $(CUOBJ_INCLUDE_PATH)
  LDFLAGS  += -L $(CUOBJ_LIB_PATH) $(CUOBJ_LIBS) -libverbs -lrdmacm

  PLUGINS_ENABLED += s3rdma_minio
endif
