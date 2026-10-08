#
# Makefile for s3rdma-client, the minimalistic S3-over-RDMA test client for tools/s3rdma/server.
#
# Independent of the main elbencho Makefile. The client uses the cuObject client API; by default
# it is built against the RC shim in ../cuobjclient-shim (built first), NVIDIA's libcuobjclient
# can be selected via CUOBJ_INCLUDE_PATH / CUOBJ_LIB_PATH. Shared code is in ../common.
#

EXE_NAME          ?= s3rdma-client
CXX               ?= g++
CXX_FLAVOR        ?= c++17
BUILD_DEBUG       ?= 0

# The cuObject client library to use: dirs of cuobjclient.h and libcuobjclient.so, plus extra
# libraries that it needs (NVIDIA's needs -lcufile). Default: the RC shim.
SHIM_PATH         := ../cuobjclient-shim
CUOBJ_INCLUDE_PATH ?= $(SHIM_PATH)/include
CUOBJ_LIB_PATH    ?= $(SHIM_PATH)/lib
CUOBJ_EXTRA_LIBS  ?=

SOURCE_PATH       := source
COMMON_PATH       := ../common
EXTERNAL_PATH     := $(COMMON_PATH)/external
BUILD_PATH        := build
BIN_PATH          := bin
EXE               := $(BIN_PATH)/$(EXE_NAME)

.DEFAULT_GOAL := all
include $(COMMON_PATH)/httplib.mk

SOURCES           := $(wildcard $(SOURCE_PATH)/*.cpp)
OBJECTS           := $(patsubst %.cpp,$(BUILD_PATH)/%.o,$(notdir $(SOURCES) ) )
DEPENDENCY_FILES  := $(OBJECTS:.o=.d)

vpath %.cpp $(SOURCE_PATH)

CXXFLAGS_COMMON    = -DEXE_NAME=\"$(EXE_NAME)\" -I $(SOURCE_PATH) -I $(COMMON_PATH) \
	-I $(EXTERNAL_PATH) -I $(CUOBJ_INCLUDE_PATH) \
	-Wall -Wunused-variable -Woverloaded-virtual -Wextra -Wno-unused-parameter -fmessage-length=0 \
	-pthread -ggdb -std=$(CXX_FLAVOR)
CXXFLAGS_RELEASE   = -O2 -g1
CXXFLAGS_DEBUG     = -O0 -DBUILD_DEBUG
LDFLAGS_COMMON     = -pthread -L $(CUOBJ_LIB_PATH) -Wl,-rpath,$(CUOBJ_RPATH) \
	-lcuobjclient $(CUOBJ_EXTRA_LIBS)

ifeq ($(BUILD_DEBUG), 1)
  CXXFLAGS = $(CXXFLAGS_COMMON) $(CXXFLAGS_DEBUG) $(CXXFLAGS_EXTRA)
else
  CXXFLAGS = $(CXXFLAGS_COMMON) $(CXXFLAGS_RELEASE) $(CXXFLAGS_EXTRA)
endif
LDFLAGS = $(LDFLAGS_COMMON) $(LDFLAGS_EXTRA)

# Build the shim first when it is the library in use, and find it relative to the executable at
# run time, so that a copied or moved tree keeps working (the soname is the same as NVIDIA's)
ifeq ($(CUOBJ_LIB_PATH),$(SHIM_PATH)/lib)
  SHIM_TARGET := shim
  CUOBJ_RPATH := '$$ORIGIN/../../cuobjclient-shim/lib'
else
  CUOBJ_RPATH := $(abspath $(CUOBJ_LIB_PATH))
endif

ifdef BUILD_VERBOSE
  Q :=
else
  Q := @
endif

.PHONY: all shim clean clean-all help

all: $(EXE)
	@echo "[OPT] cuObject client library: $(CUOBJ_LIB_PATH)"

shim:
	$(Q)$(MAKE) -C $(SHIM_PATH)

$(EXE): $(OBJECTS) | $(SHIM_TARGET)
	@mkdir -p $(BIN_PATH)
	@echo "[LINK] $@"
	$(Q)$(CXX) -o $@ $(OBJECTS) $(LDFLAGS)

$(BUILD_PATH)/%.o: %.cpp $(HTTPLIB_H)
	@mkdir -p $(BUILD_PATH)
	@echo "[CXX] $@"
	$(Q)$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

clean:
	@echo "[DELETE] OBJECTS, EXECUTABLE"
	$(Q)rm -rf $(BUILD_PATH) $(BIN_PATH)

clean-all: clean
	@echo "[DELETE] SHARED EXTERNALS"
	$(Q)rm -rf $(EXTERNAL_PATH)

help:
	@echo 'Optional Arguments:'
	@echo '   CUOBJ_INCLUDE_PATH=<path> - Dir containing cuobjclient.h. (Default: $(SHIM_PATH)/include)'
	@echo '   CUOBJ_LIB_PATH=<path>    - Dir containing libcuobjclient.so. (Default: $(SHIM_PATH)/lib)'
	@echo '   CUOBJ_EXTRA_LIBS=<libs>  - Extra link flags for that library, e.g. -lcufile for NVIDIA'"'"'s.'
	@echo '   HTTPLIB_VERSION=<tag>    - cpp-httplib release to download. (Default: $(HTTPLIB_VERSION))'
	@echo '   BUILD_DEBUG=1            - Build with debug symbols and without optimizations.'
	@echo '   BUILD_VERBOSE=1          - Show compiler & linker command lines.'
	@echo '   CXX=<compiler>           - C++ compiler to use. (Default: g++)'
	@echo
	@echo 'Targets:'
	@echo '   all (default)            - Build $(EXE) (and the shim, if in use)'
	@echo '   clean                    - Remove object files and the executable'
	@echo '   clean-all                - Also remove the downloaded external files'

-include $(DEPENDENCY_FILES)
