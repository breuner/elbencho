#
# Makefile for the cuObject client shim: libcuobjclient.so with the API of NVIDIA's cuObject
# client library, implemented over Reliable Connections on plain libibverbs (host memory only).
# See README.md. Independent of the main elbencho Makefile; shared code is in ../common.
#

LIB_NAME          ?= libcuobjclient
LIB_SOVERSION     ?= 1
CXX               ?= g++
CXX_FLAVOR        ?= c++17
BUILD_DEBUG       ?= 0

SOURCE_PATH       := source
INCLUDE_PATH      := include
COMMON_PATH       := ../common
EXTERNAL_PATH     := $(COMMON_PATH)/external
BUILD_PATH        := build
LIB_PATH          := lib
LIB_SONAME        := $(LIB_NAME).so.$(LIB_SOVERSION)
LIB               := $(LIB_PATH)/$(LIB_SONAME)
LIB_LINKNAME      := $(LIB_PATH)/$(LIB_NAME).so

.DEFAULT_GOAL := all
include $(COMMON_PATH)/httplib.mk

SOURCES           := $(wildcard $(SOURCE_PATH)/*.cpp) $(COMMON_PATH)/RcDevice.cpp
OBJECTS           := $(patsubst %.cpp,$(BUILD_PATH)/%.o,$(notdir $(SOURCES) ) )
DEPENDENCY_FILES  := $(OBJECTS:.o=.d)

vpath %.cpp $(SOURCE_PATH) $(COMMON_PATH)

CXXFLAGS_COMMON    = -DRC_SUPPORT -I $(INCLUDE_PATH) -I $(COMMON_PATH) -I $(EXTERNAL_PATH) \
	-fPIC -fvisibility=hidden \
	-Wall -Wunused-variable -Woverloaded-virtual -Wextra -Wno-unused-parameter -fmessage-length=0 \
	-pthread -ggdb -std=$(CXX_FLAVOR)
CXXFLAGS_RELEASE   = -O2 -g1
CXXFLAGS_DEBUG     = -O0 -DBUILD_DEBUG
LDFLAGS_COMMON     = -shared -Wl,-soname,$(LIB_SONAME) -pthread -libverbs

ifeq ($(BUILD_DEBUG), 1)
  CXXFLAGS = $(CXXFLAGS_COMMON) $(CXXFLAGS_DEBUG) $(CXXFLAGS_EXTRA)
else
  CXXFLAGS = $(CXXFLAGS_COMMON) $(CXXFLAGS_RELEASE) $(CXXFLAGS_EXTRA)
endif
LDFLAGS = $(LDFLAGS_COMMON) $(LDFLAGS_EXTRA)

ifeq ($(shell uname -s),Darwin)
  $(error The cuObject client shim needs libibverbs and is Linux-only)
endif

ifdef BUILD_VERBOSE
  Q :=
else
  Q := @
endif

.PHONY: all clean clean-all help

all: $(LIB_LINKNAME)

$(LIB): $(OBJECTS)
	@mkdir -p $(LIB_PATH)
	@echo "[LINK] $@"
	$(Q)$(CXX) -o $@ $(OBJECTS) $(LDFLAGS)

$(LIB_LINKNAME): $(LIB)
	$(Q)ln -sf $(LIB_SONAME) $@

# The cuObject API symbols have to be visible, everything else stays hidden.
$(BUILD_PATH)/CuObjClientShim.o: CXXFLAGS += -fvisibility=default

$(BUILD_PATH)/%.o: %.cpp $(HTTPLIB_H)
	@mkdir -p $(BUILD_PATH)
	@echo "[CXX] $@"
	$(Q)$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

clean:
	@echo "[DELETE] OBJECTS, LIBRARY"
	$(Q)rm -rf $(BUILD_PATH) $(LIB_PATH)

clean-all: clean
	@echo "[DELETE] SHARED EXTERNALS"
	$(Q)rm -rf $(EXTERNAL_PATH)

help:
	@echo 'Optional Arguments:'
	@echo '   HTTPLIB_VERSION=<tag>    - cpp-httplib release to download. (Default: $(HTTPLIB_VERSION))'
	@echo '   BUILD_DEBUG=1            - Build with debug symbols and without optimizations.'
	@echo '   BUILD_VERBOSE=1          - Show compiler & linker command lines.'
	@echo '   CXX=<compiler>           - C++ compiler to use. (Default: g++)'
	@echo
	@echo 'Targets:'
	@echo '   all (default)            - Build $(LIB) and the $(LIB_LINKNAME) link'
	@echo '   clean                    - Remove object files and the library'
	@echo '   clean-all                - Also remove the downloaded external files'

-include $(DEPENDENCY_FILES)
