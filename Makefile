# 
# Use "make help" to find out about configuration options.
#

EXE_NAME           ?= elbencho
EXE_VER_MAJOR      ?= 1
EXE_VER_MINOR      ?= 1
EXE_VER_PATCHLEVEL ?= 0
EXE_VERSION        ?= $(EXE_VER_MAJOR).$(EXE_VER_MINOR)-$(EXE_VER_PATCHLEVEL)
EXE                ?= $(BIN_PATH)/$(EXE_NAME)
EXE_UNSTRIPPED     ?= $(EXE)-unstripped

SOURCE_PATH        ?= ./source
BIN_PATH           ?= ./bin
EXTERNAL_PATH      ?= ./external
PACKAGING_PATH     ?= ./packaging

INST_PATH          ?= /usr/local/bin
PKG_INST_PATH      ?= /usr/bin

CXX                ?= g++
STRIP              ?= strip

CXXFLAGS_COMMON  = -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 -DBOOST_SPIRIT_THREADSAFE \
	-DNCURSES_NOMACROS -DEXE_NAME=\"$(EXE_NAME)\" -DEXE_VERSION=\"$(EXE_VERSION)\" \
	-I $(SOURCE_PATH) -I $(EXTERNAL_PATH)/Simple-Web-Server -Wall \
	-Wunused-variable -Woverloaded-virtual -Wextra -Wno-unused-parameter -fmessage-length=0 \
	-fno-strict-aliasing -pthread -ggdb -std=c++14
CXXFLAGS_RELEASE = -O3 -Wuninitialized
CXXFLAGS_DEBUG   = -O0 -D_FORTIFY_SOURCE=2 -DBUILD_DEBUG

LDFLAGS_COMMON   = -rdynamic -pthread -lrt -lnuma -laio -lncurses -lboost_program_options \
	-lboost_system  -lboost_thread
LDFLAGS_RELASE   = -O3
LDFLAGS_DEBUG    = -O0

SOURCES          = $(shell find $(SOURCE_PATH) -name '*.cpp')
OBJECTS          = $(SOURCES:.cpp=.o)
OBJECTS_CLEANUP  = $(shell find $(SOURCE_PATH) -name '*.o') # separate to clean after C file rename
DEPENDENCY_FILES = $(shell find $(SOURCE_PATH) -name '*.d')

# Release & debug flags for compiler and linker
ifeq ($(BUILD_DEBUG),)
CXXFLAGS = $(CXXFLAGS_COMMON) $(CXXFLAGS_RELEASE) $(CXXFLAGS_EXTRA)
LDFLAGS  = $(LDFLAGS_COMMON) $(LDFLAGS_RELASE) $(LDFLAGS_EXTRA)
else
CXXFLAGS = $(CXXFLAGS_COMMON) $(CXXFLAGS_DEBUG) $(CXXFLAGS_EXTRA)
LDFLAGS  = $(LDFLAGS_COMMON) $(LDFLAGS_DEBUG) $(LDFLAGS_EXTRA)
endif

# CUDA includes and paths
ifeq ($(CUDA_SUPPORT),1)
CUDA_PATH   ?= /usr/local/cuda
CXXFLAGS    += -I $(CUDA_PATH)/include/ -DCUDA_SUPPORT
CUDA_LIB    := -L $(CUDA_PATH)/lib64/ -lcuda -L $(CUDA_PATH)/lib64/ -lcudart
LDFLAGS     +=  $(CUDA_LIB) -ldl
endif


all: $(SOURCES) $(EXE)

debug:
	@$(MAKE) BUILD_DEBUG=1 all

$(EXE): $(EXE_UNSTRIPPED)
ifdef BUILD_VERBOSE
	$(STRIP) --strip-debug $(EXE_UNSTRIPPED) -o $(EXE)
else
	@echo [STRIP] $@ 
	@$(STRIP) --strip-debug $(EXE_UNSTRIPPED) -o $(EXE)
endif

$(EXE_UNSTRIPPED): $(OBJECTS)
ifdef BUILD_VERBOSE
	$(CXX) -o $(EXE_UNSTRIPPED) $(OBJECTS) $(LDFLAGS)
else
	@echo [LINK] $@
	@$(CXX) -o $(EXE_UNSTRIPPED) $(OBJECTS) $(LDFLAGS)
endif

.cpp.o:
ifdef BUILD_VERBOSE
	$(CXX) $(CXXFLAGS) -c $(@:.o=.cpp) -E -MMD -MF $(@:.o=.d) -MT $(@) -o /dev/null
	$(CXX) $(CXXFLAGS) -c $(@:.o=.cpp) -o $(@)
else
	@echo [DEP] $(@:.o=.d)
	@$(CXX) $(CXXFLAGS) -c $(@:.o=.cpp) -E -MMD -MF $(@:.o=.d) -MT $(@) -o /dev/null
	@echo [CXX] $@
	@$(CXX) $(CXXFLAGS) -c $(@:.o=.cpp) -o $(@)
endif

$(OBJECTS): | externals

externals:
ifdef BUILD_VERBOSE
	$(EXTERNAL_PATH)/prepare-external.sh
else
	@$(EXTERNAL_PATH)/prepare-external.sh
endif

clean:
ifdef BUILD_VERBOSE
	rm -rf $(OBJECTS_CLEANUP) $(DEPENDENCY_FILES) $(EXE) $(EXE_UNSTRIPPED) \
		$(PACKAGING_PATH)/$(PKG_INST_PATH) $(PACKAGING_PATH)/DEBIAN/control \
		$(PACKAGING_PATH)/$(EXE_NAME)_$(EXE_VERSION).deb $(PACKAGING_PATH)/RPMS/* \
		$(PACKAGING_PATH)/SPECS/rpm.spec
else
	@echo "[DELETE] OBJECTS, DEPENDENCY_FILES, EXECUTABLES, PACKAGES"
	@rm -rf $(OBJECTS_CLEANUP) $(DEPENDENCY_FILES) $(EXE) $(EXE_UNSTRIPPED) \
		$(PACKAGING_PATH)/BUILDROOT/* $(PACKAGING_PATH)/$(EXE_NAME)_$(EXE_VERSION).deb \
		$(PACKAGING_PATH)/RPMS/* $(PACKAGING_PATH)/SPECS/rpm.spec 
endif

clean-all: clean
ifdef BUILD_VERBOSE
	rm -rf $(EXTERNAL_PATH)/Simple-Web-Server
else
	@echo "[DELETE] EXTERNALS"
	@rm -rf $(EXTERNAL_PATH)/Simple-Web-Server
endif
	


install: all
	install -p -m u=rwx,g=rx,o=rx $(EXE) $(INST_PATH)/
	
uninstall:
	rm -f $(INST_PATH)/$(EXE_NAME)

rpm: all
	# remove potential leftovers from previous deb build
	rm -rf $(PACKAGING_PATH)/BUILDROOT/* 
	
	mkdir -p $(PACKAGING_PATH)/BUILDROOT/$(PKG_INST_PATH)

	cp --preserve $(EXE) $(PACKAGING_PATH)/BUILDROOT/$(PKG_INST_PATH)
	
	cp $(PACKAGING_PATH)/SPECS/rpm.spec.template $(PACKAGING_PATH)/SPECS/rpm.spec
	sed -i "s/__NAME__/$(EXE_NAME)/" $(PACKAGING_PATH)/SPECS/rpm.spec
	sed -i "s/__VERSION__/$(EXE_VER_MAJOR).$(EXE_VER_MINOR).$(EXE_VER_PATCHLEVEL)/" \
		$(PACKAGING_PATH)/SPECS/rpm.spec
	
	rpmbuild $(PACKAGING_PATH)/SPECS/rpm.spec --bb --define "_topdir $(PWD)/$(PACKAGING_PATH)" \
		--define "__spec_install_pre /bin/true" --buildroot=$(PWD)/$(PACKAGING_PATH)/BUILDROOT

deb: all
	# remove potential leftovers from previous rpm build
	rm -rf $(PACKAGING_PATH)/BUILDROOT/* 
	
	mkdir -p $(PACKAGING_PATH)/BUILDROOT/$(PKG_INST_PATH)
	
	cp -r $(PACKAGING_PATH)/DEBIAN $(PACKAGING_PATH)/BUILDROOT
	
	cp --preserve $(EXE) $(PACKAGING_PATH)/BUILDROOT/$(PKG_INST_PATH)
	
	cp $(PACKAGING_PATH)/BUILDROOT/DEBIAN/control.template \
		$(PACKAGING_PATH)/BUILDROOT/DEBIAN/control
	sed -i "s/__NAME__/$(EXE_NAME)/" $(PACKAGING_PATH)/BUILDROOT/DEBIAN/control
	sed -i "s/__VERSION__/$(EXE_VERSION)/" $(PACKAGING_PATH)/BUILDROOT/DEBIAN/control
	sed -i "s/__ARCH__/$$(dpkg-architecture -q DEB_HOST_ARCH)/" \
		$(PACKAGING_PATH)/BUILDROOT/DEBIAN/control
	
	dpkg-deb --root-owner-group --build $(PACKAGING_PATH)/BUILDROOT \
		$(PACKAGING_PATH)/$(EXE_NAME)_$(EXE_VERSION).deb

help:
	@echo 'Optional Arguments:'
	@echo '   CXX=<string>            - Path to alternative C++ compiler'
	@echo '   CXXFLAGS_EXTRA=<string> - Additional C++ compiler flags'
	@echo '   LDFLAGS_EXTRA=<string>  - Additional linker flags'
	@echo '   BUILD_VERBOSE=1         - Verbose build output'
	@echo '   CUDA_SUPPORT=1          - Add support for CUDA to work with GPU memory.'
	@echo
	@echo 'Targets:'
	@echo '   all (default)     - Build executable'
	@echo '   debug             - Build executable with debug info'
	@echo '   clean             - Remove build artifacts'
	@echo '   clean-all         - Remove build artifacts and external sources'
	@echo '   install           - Install executable to /usr/local/bin'
	@echo '   uninstall         - Uninstall executable from /usr/local/bin'
	@echo '   rpm               - Create RPM package file'
	@echo '   deb               - Create Debian package file'
	@echo '   help              - Print this help message'

.PHONY: externals clean clean-all rpm deb help


# Include dependency files
ifneq ($(DEPENDENCY_FILES),)
include $(DEPENDENCY_FILES)
endif
