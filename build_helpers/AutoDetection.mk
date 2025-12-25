#
# Auto detection of available system libraries etc.
#

TEST_C_FILE    = $(BUILD_HELPERS_PATH)/AutoDetection.c
TEST_OBJ_FILE  = $(BUILD_HELPERS_PATH)/AutoDetection.obj

LDFLAGS_BOOST_SYSTEM     ?= -lboost_system

CXXFLAGS_LIBBACKTRACE    ?= -DBACKTRACE_SUPPORT
LDFLAGS_LIBBACKTRACE     ?= -lbacktrace


# Try to auto-detect CUDA library and inlcude paths...
CUDA_INCLUDE_PATH        ?= $(shell find /usr/local/cuda/ /usr/local/cuda* -name cuda_runtime.h \
                            -printf '%h\n' 2>/dev/null | head -n1)
CUDA_LIB_PATH            ?= $(shell find /usr/local/cuda/ /usr/local/cuda* -name libcudart.so \
                            -printf '%h\n' 2>/dev/null | head -n1)
CUFILE_INCLUDE_PATH      ?= $(shell find /usr/local/cuda/ /usr/local/cuda* -name cufile.h \
                            -printf '%h\n' 2>/dev/null | head -n1)
CUFILE_LIB_PATH          ?= $(shell find /usr/local/cuda/ /usr/local/cuda* -name libcufile.so \
                            -printf '%h\n' 2>/dev/null | head -n1)

# Prepare CUDA compile/link flags...
ifneq ($(CUDA_INCLUDE_PATH),)
 CXXFLAGS_CUDA_SUPPORT   += -I $(CUDA_INCLUDE_PATH)
endif
ifneq ($(CUDA_LIB_PATH),)
 LDFLAGS_CUDA_SUPPORT    += -L $(CUDA_LIB_PATH)
endif
CXXFLAGS_CUDA_SUPPORT    += -DCUDA_SUPPORT
LDFLAGS_CUDA_SUPPORT     += -lcudart -lcurand -ldl
CUDA_SUPPORT_DETECT_ARGS  = $(CXXFLAGS_CUDA_SUPPORT) $(LDFLAGS_CUDA_SUPPORT)

# Prepare CUFILE (GDS) compile/link flags...
ifneq ($(CUFILE_INCLUDE_PATH),)
 CXXFLAGS_CUFILE_SUPPORT   += -I $(CUFILE_INCLUDE_PATH)
endif
ifneq ($(CUFILE_LIB_PATH),)
 LDFLAGS_CUFILE_SUPPORT    += -L $(CUFILE_LIB_PATH)
endif
CXXFLAGS_CUFILE_SUPPORT    += -DCUFILE_SUPPORT
LDFLAGS_CUFILE_SUPPORT     += -lcufile
CUFILE_SUPPORT_DETECT_ARGS  = $(CXXFLAGS_CUFILE_SUPPORT) $(LDFLAGS_CUFILE_SUPPORT) \
                              $(CUDA_SUPPORT_DETECT_ARGS)

####### LIB "backtrace" ########

# Detect backtrace library to support backtraces with line numbers. If the lib is not available
# (e.g. on Alpine Linux) then we build our own copy.

ifdef BUILD_VERBOSE
  $(info [TEST_LIBBACKTRACE] $(CXX) -o $(TEST_OBJ_FILE) $(TEST_C_FILE) $(CXXFLAGS_LIBBACKTRACE) \
    $(LDFLAGS_LIBBACKTRACE) $(CXXFLAGS) $(LDFLAGS) )
endif

LIB_BACKTRACE_DETECTED = $(shell \
    if $(CXX) -o $(TEST_OBJ_FILE) $(TEST_C_FILE) $(CXXFLAGS_LIBBACKTRACE) $(LDFLAGS_LIBBACKTRACE) \
      $(CXXFLAGS) $(LDFLAGS) 1>/dev/null 2>&1; \
    then echo 1; \
    fi)

####### END LIB "backtrace" ########


####### BOOST HEADER "stacktrace" ########

# Detect "boost/stacktrace.h" header to support backtraces with line numbers. If the header is not
# available then disable stacktrace feature.
#
# Note the "-c" here to only compile and not link, because we only want to check for the header
# and not whether libbacktrace is available.

ifeq ($(BACKTRACE_SUPPORT),)
  # note: cxxflags_tmp with filter-out is because the use_backtrace define depends on libbacktrace,
  # which might not be available yet, because it might come later from prepare-external.sh).
  CXXFLAGS_TMP := $(filter-out -DBOOST_STACKTRACE_USE_BACKTRACE, $(CXXFLAGS))

  ifdef BUILD_VERBOSE
    $(info [TEST_STACKTRACE_H] $(CXX) -c -o $(TEST_OBJ_FILE) $(TEST_C_FILE) \
      $(CXXFLAGS_LIBBACKTRACE) $(CXXFLAGS_TMP) )
  endif

  STACKTRACE_H_DETECTED = $(shell \
    if $(CXX) -c -o $(TEST_OBJ_FILE) $(TEST_C_FILE) $(CXXFLAGS_LIBBACKTRACE) $(CXXFLAGS_TMP) \
      1>/dev/null 2>&1; \
    then echo 1; \
    else echo 0; \
    fi)

  BACKTRACE_SUPPORT := $(STACKTRACE_H_DETECTED)
endif

####### END BOOST HEADER "stacktrace" ########


####### BOOST LIB "boost_system" ########

# Detect boost_system library. It was removed in recent boost.org versions, but older system still
# need it (e.g. sles15), so we detect if it exists. If it exists we add it to LDFLAGS_BOOST,
# otherwise we skip it.

ifdef BUILD_VERBOSE
  $(info [TEST_BOOST_SYS] $(CXX) -o $(TEST_OBJ_FILE) $(TEST_C_FILE) $(LDFLAGS_BOOST_SYSTEM) )
endif

LIB_BOOST_SYSTEM_DETECTED = $(shell \
    if $(CXX) -o $(TEST_OBJ_FILE) $(TEST_C_FILE) $(LDFLAGS_BOOST_SYSTEM) 1>/dev/null 2>&1; \
    then printf '%s\n' '$(LDFLAGS_BOOST_SYSTEM)'; \
    fi)

LDFLAGS += $(LIB_BOOST_SYSTEM_DETECTED)

####### END BOOST LIB "boost_system" ########


####### CUFILE (GDS) Support ########
##### (Depends on CUDA_SUPPORT) #####

ifeq ($(CUFILE_SUPPORT),)
 ifdef BUILD_VERBOSE
  $(info [TEST_CUFILE] $(CXX) -o $(TEST_OBJ_FILE) $(TEST_C_FILE) $(CUFILE_SUPPORT_DETECT_ARGS) )
 endif

 CUFILE_SUPPORT_DETECTED = $(shell \
    if $(CXX) -o $(TEST_OBJ_FILE) $(TEST_C_FILE) $(CUFILE_SUPPORT_DETECT_ARGS) 1>/dev/null 2>&1; \
    then echo 1; \
    else echo 0; \
    fi)

 CUFILE_SUPPORT := $(CUFILE_SUPPORT_DETECTED)
endif

ifeq ($(CUFILE_SUPPORT),1)
 CXXFLAGS     += $(CXXFLAGS_CUFILE_SUPPORT)
 LDFLAGS      += $(LDFLAGS_CUFILE_SUPPORT)
 override CUDA_SUPPORT = 1
endif

###### End CUFILE (GDS) Support #####

########## CUDA Support #############

ifeq ($(CUDA_SUPPORT),)
 ifdef BUILD_VERBOSE
  $(info [TEST_CUDA] $(CXX) -o $(TEST_OBJ_FILE) $(TEST_C_FILE) $(CUDA_SUPPORT_DETECT_ARGS) )
 endif

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
