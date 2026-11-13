# elbencho plugin "s3rdma_ddn": S3-over-RDMA via NVIDIA cuObject.
# Enable with ELB_PLUGIN_S3RDMA_DDN=1 (see README.md in this directory).

PLUGIN_NAMES += s3rdma_ddn

define PLUGIN_HELP_s3rdma_ddn
	@echo '   ELB_PLUGIN_S3RDMA_DDN=0|1 - GPU-direct S3-over-RDMA via NVIDIA cuObject'
	@echo '                             (CUDA 13.1+). Requires S3_SUPPORT=1, libcuobjclient,'
	@echo '                             and libcurl. (Default: 0)'
endef

ifeq ($(ELB_PLUGIN_S3RDMA_DDN),1)
  ifneq ($(S3_SUPPORT),1)
    $(error ELB_PLUGIN_S3RDMA_DDN=1 requires S3_SUPPORT=1)
  endif
  ifdef ELB_S3_TRANSPORT_HEADER
    $(error Only one plugin can replace the S3 transport, but ELB_S3_TRANSPORT_HEADER is already \
      set to $(ELB_S3_TRANSPORT_HEADER))
  endif

  ELB_S3_TRANSPORT_HEADER := S3RdmaDdnTransport.h
  ELB_PLUGIN_S3RDMA_DDN_PATH := $(PLUGINS_PATH)/s3rdma_ddn/source

  ifndef CUOBJECT_ROOT_DIR
    CUOBJECT_DEF_ROOT = /usr/local/cuda/targets/
    CUOBJECT_CURRENT_ARCH = $(shell uname -m)

    CUOBJECT_NUM_TARGETS = $(shell find $(CUOBJECT_DEF_ROOT) -maxdepth 1 -mindepth 1 \
                  -type d -name '*-linux' | wc -l)

    ifeq ($(CUOBJECT_NUM_TARGETS),1)
      # select the only present cuda arch
      CUOBJECT_ROOT_DIR = $(shell find $(CUOBJECT_DEF_ROOT) -maxdepth 1 -mindepth 1 \
                -type d -name '*-linux')
    else ifeq ($(shell test $(CUOBJECT_NUM_TARGETS) -gt 1; echo $$?),0)
      # arch-dependent cuda arch lookup
      ifeq ($(shell echo $(CUOBJECT_CURRENT_ARCH) | grep -Eq 'x86_64|AMD64|amd64'; echo $$?),0)
        CUOBJECT_TARGET_ARCH = x86_64-linux
      else ifeq ($(shell echo $(CUOBJECT_CURRENT_ARCH) | grep -Eq ppc64le; echo $$?),0)
        CUOBJECT_TARGET_ARCH = ppc64le-linux
      else ifeq ($(shell echo $(CUOBJECT_CURRENT_ARCH) | grep -Eq 'aarch64|arm64|ARM64'; echo $$?),0)
        ifneq ($(wildcard $(CUOBJECT_DEF_ROOT)sbsa-linux),)
          CUOBJECT_TARGET_ARCH = sbsa-linux
        else
          CUOBJECT_TARGET_ARCH = aarch64-linux
        endif
      endif

      ifdef CUOBJECT_TARGET_ARCH
        CUOBJECT_ROOT_DIR = $(CUOBJECT_DEF_ROOT)$(CUOBJECT_TARGET_ARCH)
      else
	$(warning ELB_PLUGIN_S3RDMA_DDN=1: can't select cuobject target arch in $(CUOBJECT_DEF_ROOT))
      endif
    else
      $(warning ELB_PLUGIN_S3RDMA_DDN=1: No valid cuda targets in $(CUOBJECT_DEF_ROOT))
    endif
  endif

  ifndef CUOBJECT_ROOT_DIR
    $(error ELB_PLUGIN_S3RDMA_DDN=1: Can't detect valid cuobject root dir.)
  else ifeq ($(wildcard $(CUOBJECT_ROOT_DIR)),)
    $(error ELB_PLUGIN_S3RDMA_DDN=1: cuobject root dir $(CUOBJECT_ROOT_DIR) is not valid path.)
  else
    $(info ELB_PLUGIN_S3RDMA_DDN=1: cuobject target root dir is $(CUOBJECT_ROOT_DIR))
    CUOBJECT_LIB_DIR = $(CUOBJECT_ROOT_DIR)/lib
    CUOBJECT_INCLUDE_DIR = $(CUOBJECT_ROOT_DIR)/include
  endif

  SOURCES  += $(shell find $(ELB_PLUGIN_S3RDMA_DDN_PATH) -name '*.cpp')
  CXXFLAGS += -DELB_PLUGIN_S3RDMA_DDN -DELB_S3_TRANSPORT_HEADER='"$(ELB_S3_TRANSPORT_HEADER)"' \
              -I $(ELB_PLUGIN_S3RDMA_DDN_PATH) -I $(CUOBJECT_INCLUDE_DIR)
  LDFLAGS  += -L $(CUOBJECT_LIB_DIR) -lcurl -lcuobjclient -lcufile

  PLUGINS_ENABLED += s3rdma_ddn
endif

