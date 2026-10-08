#
# Makefile for s3rdma-server, the minimalistic S3-over-RDMA test server for elbencho.
#
# This is deliberately independent of the main elbencho Makefile. The server is only used by the
# test suite (tests/tests_plugin_s3rdma/) and for manual testing of the s3rdma_* plugins. Code
# shared with the other S3-over-RDMA test tools lives in ../common.
#

EXE_NAME          ?= s3rdma-server
CXX               ?= g++
CXX_FLAVOR        ?= c++17
BUILD_DEBUG       ?= 0

# cuObject server library (NVIDIA libcuobjserver) for the DC transport of the cuObject protocol.
# Auto-detected when unset.
CUOBJ_SUPPORT     ?=
# CUOBJ_INCLUDE_PATH and CUOBJ_LIB_PATH: dirs of cuobjserver.h and libcuobjserver.so, auto-detected

# RC transport via plain libibverbs, for AMD hipObject's hipobj-rc-v2 protocol and for the
# cuObject-style protocol with the RC client shim (../cuobjclient-shim). Auto-detected when unset.
RC_SUPPORT        ?=

# AMD hipObject (MIT) sources, pinned to a commit. Only its RDMA test client gets built from them
# (bin/hipobj-v2-data-client), as the test suite's client for the hipobj-rc-v2 protocol. The
# hipObject library itself needs the ROCm toolchain and is not built. Set HIPOBJ_CLIENT=0 to skip.
HIPOBJ_CLIENT     ?=
HIPOBJ_COMMIT     ?= abc2dbe14324da55a9383a7425e7726b5bab36ec
HIPOBJ_URL        ?= https://github.com/ROCm/hipObject/archive/$(HIPOBJ_COMMIT).tar.gz

SOURCE_PATH       := source
COMMON_PATH       := ../common
EXTERNAL_PATH     := $(COMMON_PATH)/external
BUILD_PATH        := build
BIN_PATH          := bin
EXE               := $(BIN_PATH)/$(EXE_NAME)

HIPOBJ_PATH       := $(BUILD_PATH)/hipObject
HIPOBJ_CLIENT_EXE := $(BIN_PATH)/hipobj-v2-data-client
HIPOBJ_CLIENT_SRC := test/integration/rdma-test-server/v2_data_client.cpp \
                     test/integration/rdma-test-server/v2_sigv4.cpp src/rdma/token.cpp

.DEFAULT_GOAL := all
include $(COMMON_PATH)/httplib.mk

SOURCES           := $(wildcard $(SOURCE_PATH)/*.cpp) $(COMMON_PATH)/RcDevice.cpp
OBJECTS           := $(patsubst %.cpp,$(BUILD_PATH)/%.o,$(notdir $(SOURCES) ) )
DEPENDENCY_FILES  := $(OBJECTS:.o=.d)

vpath %.cpp $(SOURCE_PATH) $(COMMON_PATH)

CXXFLAGS_COMMON    = -DEXE_NAME=\"$(EXE_NAME)\" -I $(SOURCE_PATH) -I $(COMMON_PATH) \
	-I $(EXTERNAL_PATH) \
	-Wall -Wunused-variable -Woverloaded-virtual -Wextra -Wno-unused-parameter -fmessage-length=0 \
	-pthread -ggdb -std=$(CXX_FLAVOR)
CXXFLAGS_RELEASE   = -O2 -g1
CXXFLAGS_DEBUG     = -O0 -DBUILD_DEBUG
LDFLAGS_COMMON     = -pthread

ifeq ($(BUILD_DEBUG), 1)
  CXXFLAGS = $(CXXFLAGS_COMMON) $(CXXFLAGS_DEBUG) $(CXXFLAGS_EXTRA)
else
  CXXFLAGS = $(CXXFLAGS_COMMON) $(CXXFLAGS_RELEASE) $(CXXFLAGS_EXTRA)
endif
LDFLAGS = $(LDFLAGS_COMMON) $(LDFLAGS_EXTRA)

# libcuobjserver and libibverbs are Linux-only
ifeq ($(shell uname -s),Darwin)
  CUOBJ_SUPPORT := 0
  RC_SUPPORT    := 0
  HIPOBJ_CLIENT := 0
endif

# Probe for a header with the compiler, e.g. $(call have_header,infiniband/verbs.h) => 1 or 0
have_header = $(shell echo | $(CXX) -x c++ -fsyntax-only -include $(1) - >/dev/null 2>&1 \
                && echo 1 || echo 0)

# Auto-detect cuObject server header & library. (The pkg-config module "cuobjserver-*" of the
# NVIDIA package points to non-existent paths, so we search the usual dirs instead.)
ifneq ($(CUOBJ_SUPPORT),0)
  ifeq ($(CUOBJ_INCLUDE_PATH),)
    CUOBJ_INCLUDE_PATH := $(shell find /usr/include /usr/local/include /usr/local/cuda* \
                            -maxdepth 4 -name cuobjserver.h -printf '%h\n' 2>/dev/null | head -n1)
  endif
  ifeq ($(CUOBJ_LIB_PATH),)
    CUOBJ_LIB_PATH     := $(shell find /usr/lib /usr/lib64 /usr/local/lib /usr/local/cuda* \
                            -maxdepth 4 -name libcuobjserver.so -printf '%h\n' 2>/dev/null | head -n1)
  endif

  ifeq ($(CUOBJ_SUPPORT),1)
    ifeq ($(CUOBJ_INCLUDE_PATH),)
      $(error CUOBJ_SUPPORT=1: cuobjserver.h not found, set CUOBJ_INCLUDE_PATH)
    endif
  else
    ifneq ($(CUOBJ_INCLUDE_PATH),)
      CUOBJ_SUPPORT := 1
    else
      CUOBJ_SUPPORT := 0
    endif
  endif
endif

ifeq ($(CUOBJ_SUPPORT),1)
  CXXFLAGS += -DCUOBJ_SUPPORT -I $(CUOBJ_INCLUDE_PATH)
  ifneq ($(CUOBJ_LIB_PATH),)
    LDFLAGS += -L $(CUOBJ_LIB_PATH)
  endif
  LDFLAGS += -lcuobjserver -libverbs -lrdmacm
endif

ifeq ($(RC_SUPPORT),)
  RC_SUPPORT := $(call have_header,infiniband/verbs.h)
endif

ifeq ($(RC_SUPPORT),1)
  CXXFLAGS += -DRC_SUPPORT
  LDFLAGS  += -libverbs
endif

# The hipObject test client needs libibverbs and OpenSSL (for its request signatures)
ifeq ($(HIPOBJ_CLIENT),)
  ifeq ($(RC_SUPPORT)$(call have_header,openssl/hmac.h),11)
    HIPOBJ_CLIENT := 1
  else
    HIPOBJ_CLIENT := 0
  endif
endif

ifeq ($(HIPOBJ_CLIENT),1)
  HIPOBJ_CLIENT_TARGET := $(HIPOBJ_CLIENT_EXE)
endif

ifdef BUILD_VERBOSE
  Q :=
else
  Q := @
endif

.PHONY: all clean clean-all help

all: $(EXE) $(HIPOBJ_CLIENT_TARGET)
	@echo "[OPT] cuObject (DC) RDMA support: $(CUOBJ_SUPPORT)"
	@echo "[OPT] RC RDMA support (hipobj-rc-v2, RC client shim): $(RC_SUPPORT)"
	@echo "[OPT] hipObject test client: $(HIPOBJ_CLIENT)"

$(EXE): $(OBJECTS)
	@mkdir -p $(BIN_PATH)
	@echo "[LINK] $@"
	$(Q)$(CXX) -o $@ $(OBJECTS) $(LDFLAGS)

$(BUILD_PATH)/%.o: %.cpp $(HTTPLIB_H)
	@mkdir -p $(BUILD_PATH)
	@echo "[CXX] $@"
	$(Q)$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# The hipObject sources: download the pinned commit and unpack it under a stable dir name
$(HIPOBJ_PATH)/.unpacked:
	@echo "[DOWNLOAD] $(HIPOBJ_URL)"
	@mkdir -p $(BUILD_PATH)
	$(Q)if command -v curl >/dev/null 2>&1; then \
	    curl -fLsS -o $(HIPOBJ_PATH).tar.gz "$(HIPOBJ_URL)"; \
	elif command -v wget >/dev/null 2>&1; then \
	    wget -q -O $(HIPOBJ_PATH).tar.gz "$(HIPOBJ_URL)"; \
	else \
	    echo "ERROR: Neither curl nor wget found to download hipObject" >&2; exit 1; \
	fi
	$(Q)rm -rf $(HIPOBJ_PATH) $(HIPOBJ_PATH).tmp && mkdir -p $(HIPOBJ_PATH).tmp
	$(Q)tar -xzf $(HIPOBJ_PATH).tar.gz -C $(HIPOBJ_PATH).tmp --strip-components=1
	$(Q)rm -f $(HIPOBJ_PATH).tar.gz && mv $(HIPOBJ_PATH).tmp $(HIPOBJ_PATH)
	$(Q)touch $@

# hipObject's own RC test client (plain libibverbs and OpenSSL, no ROCm needed)
$(HIPOBJ_CLIENT_EXE): $(HIPOBJ_PATH)/.unpacked
	@mkdir -p $(BIN_PATH)
	@echo "[CXX] $@"
	$(Q)$(CXX) -std=$(CXX_FLAVOR) -O2 -DHIPOBJECT_V2_API -I $(HIPOBJ_PATH)/src/common \
	    -I $(HIPOBJ_PATH)/src/rdma -I $(HIPOBJ_PATH)/test/integration/rdma-test-server \
	    -o $@ $(addprefix $(HIPOBJ_PATH)/,$(HIPOBJ_CLIENT_SRC) ) -lcrypto -libverbs

clean:
	@echo "[DELETE] OBJECTS, EXECUTABLES"
	$(Q)rm -rf $(OBJECTS) $(DEPENDENCY_FILES) $(EXE) $(HIPOBJ_CLIENT_EXE)

clean-all:
	@echo "[DELETE] BUILD DIR (incl. hipObject sources), EXECUTABLES, SHARED EXTERNALS"
	$(Q)rm -rf $(BUILD_PATH) $(BIN_PATH) $(EXTERNAL_PATH)

help:
	@echo 'Optional Arguments:'
	@echo '   CUOBJ_SUPPORT=0|1        - Use NVIDIA libcuobjserver for the DC transport of the cuObject'
	@echo '                              protocol. (Default: auto-detected, 0 on macOS)'
	@echo '   CUOBJ_INCLUDE_PATH=<path> - Dir containing cuobjserver.h. (Default: auto-detected)'
	@echo '   CUOBJ_LIB_PATH=<path>    - Dir containing libcuobjserver.so. (Default: auto-detected)'
	@echo '   RC_SUPPORT=0|1           - RC transport via libibverbs, for the hipobj-rc-v2 protocol of'
	@echo '                              AMD hipObject and for the RC client shim.'
	@echo '                              (Default: auto-detected, 0 on macOS)'
	@echo '   HIPOBJ_CLIENT=0|1        - Also build AMD hipObject'"'"'s RDMA test client from its'
	@echo '                              downloaded sources, for the test suite. Needs libibverbs'
	@echo '                              and OpenSSL. (Default: auto-detected)'
	@echo '   HIPOBJ_COMMIT=<sha>      - hipObject commit to download. (Default: $(HIPOBJ_COMMIT))'
	@echo '   HTTPLIB_VERSION=<tag>    - cpp-httplib release to download. (Default: $(HTTPLIB_VERSION))'
	@echo '   BUILD_DEBUG=1            - Build with debug symbols and without optimizations.'
	@echo '   BUILD_VERBOSE=1          - Show compiler & linker command lines.'
	@echo '   CXX=<compiler>           - C++ compiler to use. (Default: g++)'
	@echo
	@echo 'Targets:'
	@echo '   all (default)            - Build $(EXE) (and the hipObject test client)'
	@echo '   clean                    - Remove object files and the executables'
	@echo '   clean-all                - Also remove the downloaded external files'

-include $(DEPENDENCY_FILES)
