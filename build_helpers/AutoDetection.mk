#
# Auto detection of available system libraries etc.
#

features-detect: externals

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

# Try to auto-detect AMD HIP (ROCm) library and include paths...
# (ROCm installs under /opt/rocm or a versioned dir like /opt/rocm/core-10.2.)
HIP_INCLUDE_PATH         ?= $(patsubst %/hip,%,$(shell find /opt/rocm/ /opt/rocm* \
                            -name hip_runtime_api.h -path '*/include/hip/*' -printf '%h\n' \
                            2>/dev/null | head -n1))
HIP_LIB_PATH             ?= $(shell find /opt/rocm/ /opt/rocm* -name libamdhip64.so \
                            -printf '%h\n' 2>/dev/null | head -n1)
HIPFILE_INCLUDE_PATH     ?= $(shell find /opt/rocm/ /opt/rocm* -name hipfile.h \
                            -printf '%h\n' 2>/dev/null | head -n1)
HIPFILE_LIB_PATH         ?= $(shell find /opt/rocm/ /opt/rocm* -name libhipfile.so \
                            -printf '%h\n' 2>/dev/null | head -n1)

# Prepare HIP compile/link flags...
# (The rpath is needed because distros ship an older libamdhip64 in the default lib path.)
ifneq ($(HIP_INCLUDE_PATH),)
 CXXFLAGS_HIP_SUPPORT    += -I $(HIP_INCLUDE_PATH)
endif
ifneq ($(HIP_LIB_PATH),)
 LDFLAGS_HIP_SUPPORT     += -L $(HIP_LIB_PATH) -Wl,-rpath,$(HIP_LIB_PATH)
endif
CXXFLAGS_HIP_SUPPORT     += -D__HIP_PLATFORM_AMD__ -DHIP_SUPPORT
LDFLAGS_HIP_SUPPORT      += -lamdhip64 -lhiprand
HIP_SUPPORT_DETECT_ARGS   = $(CXXFLAGS_HIP_SUPPORT) $(LDFLAGS_HIP_SUPPORT)

# Prepare HIPFILE compile/link flags...
ifneq ($(HIPFILE_INCLUDE_PATH),)
 CXXFLAGS_HIPFILE_SUPPORT  += -I $(HIPFILE_INCLUDE_PATH)
endif
ifneq ($(HIPFILE_LIB_PATH),)
 LDFLAGS_HIPFILE_SUPPORT   += -L $(HIPFILE_LIB_PATH) -Wl,-rpath,$(HIPFILE_LIB_PATH)
endif
CXXFLAGS_HIPFILE_SUPPORT   += -DHIPFILE_SUPPORT
LDFLAGS_HIPFILE_SUPPORT    += -lhipfile
HIPFILE_SUPPORT_DETECT_ARGS = $(CXXFLAGS_HIPFILE_SUPPORT) $(LDFLAGS_HIPFILE_SUPPORT) \
                              $(HIP_SUPPORT_DETECT_ARGS)

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

# Only one GPU vendor per executable: an explicitly enabled HIP disables the CUDA auto-detection,
# and so does an explicitly disabled CUDA (because CUFILE would force CUDA on).
ifeq ($(CUFILE_SUPPORT),)
 ifneq ($(filter 1,$(HIP_SUPPORT) $(HIPFILE_SUPPORT)),)
  CUFILE_SUPPORT := 0
 endif
 ifeq ($(CUDA_SUPPORT),0)
  CUFILE_SUPPORT := 0
 endif
endif

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
 ifneq ($(filter 1,$(HIP_SUPPORT) $(HIPFILE_SUPPORT)),)
  CUDA_SUPPORT := 0
 endif
endif

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

####### HIPFILE Support ##############
##### (Depends on HIP_SUPPORT) ######

# Only one GPU vendor per executable: CUDA (explicit, forced by a plugin or auto-detected above)
# disables the HIP auto-detection, so CUDA wins when both toolkits are installed. An explicitly
# disabled HIP disables it too (because HIPFILE would force HIP on).
ifeq ($(HIPFILE_SUPPORT),)
 ifeq ($(CUDA_SUPPORT),1)
  HIPFILE_SUPPORT := 0
 endif
 ifeq ($(HIP_SUPPORT),0)
  HIPFILE_SUPPORT := 0
 endif
endif

ifeq ($(HIPFILE_SUPPORT),)
 ifdef BUILD_VERBOSE
  $(info [TEST_HIPFILE] $(CXX) -o $(TEST_OBJ_FILE) $(TEST_C_FILE) $(HIPFILE_SUPPORT_DETECT_ARGS) )
 endif

 HIPFILE_SUPPORT_DETECTED = $(shell \
    if $(CXX) -o $(TEST_OBJ_FILE) $(TEST_C_FILE) $(HIPFILE_SUPPORT_DETECT_ARGS) 1>/dev/null 2>&1; \
    then echo 1; \
    else echo 0; \
    fi)

 HIPFILE_SUPPORT := $(HIPFILE_SUPPORT_DETECTED)
endif

ifeq ($(HIPFILE_SUPPORT),1)
 CXXFLAGS     += $(CXXFLAGS_HIPFILE_SUPPORT)
 LDFLAGS      += $(LDFLAGS_HIPFILE_SUPPORT)
 override HIP_SUPPORT = 1
endif

###### End HIPFILE Support ##########

########## HIP Support ##############

ifeq ($(HIP_SUPPORT),)
 ifeq ($(CUDA_SUPPORT),1)
  HIP_SUPPORT := 0
 endif
endif

ifeq ($(HIP_SUPPORT),)
 ifdef BUILD_VERBOSE
  $(info [TEST_HIP] $(CXX) -o $(TEST_OBJ_FILE) $(TEST_C_FILE) $(HIP_SUPPORT_DETECT_ARGS) )
 endif

 HIP_SUPPORT_DETECTED = $(shell \
    if $(CXX) -o $(TEST_OBJ_FILE) $(TEST_C_FILE) $(HIP_SUPPORT_DETECT_ARGS) 1>/dev/null 2>&1; \
    then echo 1; \
    else echo 0; \
    fi)

 HIP_SUPPORT := $(HIP_SUPPORT_DETECTED)
endif

ifeq ($(HIP_SUPPORT),1)
 CXXFLAGS  += $(CXXFLAGS_HIP_SUPPORT)
 LDFLAGS   += $(LDFLAGS_HIP_SUPPORT)
endif

ifeq ($(CUDA_SUPPORT)$(HIP_SUPPORT),11)
 $(error CUDA_SUPPORT=1 and HIP_SUPPORT=1 are mutually exclusive (one GPU vendor per executable))
endif

########## End HIP Support ##########

####### Vendor-neutral GPU defines #######

# The sources outside source/toolkits/gpu/ only test these two; the vendor defines
# (CUDA_SUPPORT/HIP_SUPPORT, CUFILE_SUPPORT/HIPFILE_SUPPORT) are tested in the GPU toolkit only.

ifneq ($(filter 1,$(CUDA_SUPPORT) $(HIP_SUPPORT)),)
 CXXFLAGS += -DGPU_SUPPORT
endif

ifneq ($(filter 1,$(CUFILE_SUPPORT) $(HIPFILE_SUPPORT)),)
 CXXFLAGS += -DGPUFILE_SUPPORT
endif

####### End vendor-neutral GPU defines #####

clean-buildhelpers:
	$(MSG) "[DELETE] BUILD_HELPERS"
	$(QUIET)rm -f $(TEST_OBJ_FILE)
