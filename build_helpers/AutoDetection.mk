#
# Auto detection of available system libraries etc.
#

TEST_C_FILE    = $(BUILD_HELPERS_PATH)/AutoDetection.c
TEST_OBJ_FILE  = $(BUILD_HELPERS_PATH)/AutoDetection.obj

CUDA_PATH                ?= /usr/local/cuda
CXXFLAGS_CUDA_SUPPORT     = -I $(CUDA_PATH)/include/ -DCUDA_SUPPORT
LDFLAGS_CUDA_SUPPORT      = -L $(CUDA_PATH)/lib64/ -lcudart -ldl
CUDA_SUPPORT_DETECT_ARGS  = $(CXXFLAGS_CUDA_SUPPORT) $(LDFLAGS_CUDA_SUPPORT)

########## CUDA Support #############

ifeq ($(CUDA_SUPPORT),)
CUDA_SUPPORT_DETECTED = $(shell \
	if $(CXX) -o $(TEST_OBJ_FILE) $(TEST_C_FILE) $(CUDA_SUPPORT_DETECT_ARGS) 1>/dev/null 2>&1; \
	then echo 1; \
	else echo 0; \
	fi)
CUDA_SUPPORT := $(CUDA_SUPPORT_DETECTED)
endif

ifeq ($(CUDA_SUPPORT),1)
CXXFLAGS  += $(CXXFLAGS_CUDA_SUPPORT)
LDFLAGS   += $(LDFLAGS_CUDA_SUPPORT)
endif

########## End CUDA Support #########

clean-buildhelpers:
ifdef BUILD_VERBOSE
	rm -f $(TEST_OBJ_FILE)
else
	@echo "[DELETE] BUILD_HELPERS"
	@rm -f $(TEST_OBJ_FILE)
endif
