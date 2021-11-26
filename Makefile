# 
# Use "make help" to find out about configuration options.
#

EXE_NAME           ?= elbencho
EXE_VER_MAJOR      ?= 2
EXE_VER_MINOR      ?= 0
EXE_VER_PATCHLEVEL ?= 3
EXE_VERSION        ?= $(EXE_VER_MAJOR).$(EXE_VER_MINOR)-$(EXE_VER_PATCHLEVEL)
EXE                ?= $(BIN_PATH)/$(EXE_NAME)
EXE_UNSTRIPPED     ?= $(EXE)-unstripped

SOURCE_PATH        ?= ./source
BIN_PATH           ?= ./bin
EXTERNAL_PATH      ?= ./external
PACKAGING_PATH     ?= ./packaging
BUILD_HELPERS_PATH ?= ./build_helpers

INST_PATH          ?= /usr/local/bin
PKG_INST_PATH      ?= /usr/bin

CXX                ?= g++
STRIP              ?= strip

CXXFLAGS_BOOST     ?= -DBOOST_SPIRIT_THREADSAFE
LDFLAGS_BOOST      ?= -lboost_program_options -lboost_system -lboost_thread

NO_BACKTRACE       ?= 0
S3_SUPPORT         ?= 0

CXXFLAGS_COMMON  = -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 $(CXXFLAGS_BOOST) \
	-DNCURSES_NOMACROS -DEXE_NAME=\"$(EXE_NAME)\" -DEXE_VERSION=\"$(EXE_VERSION)\" \
	-DNO_BACKTRACE=$(NO_BACKTRACE) -I $(SOURCE_PATH) -I $(EXTERNAL_PATH)/Simple-Web-Server \
	-Wall -Wunused-variable -Woverloaded-virtual -Wextra -Wno-unused-parameter -fmessage-length=0 \
	-fno-strict-aliasing -pthread -ggdb -std=c++14
CXXFLAGS_RELEASE = -O3 -Wuninitialized
CXXFLAGS_DEBUG   = -O0 -D_FORTIFY_SOURCE=2 -DBUILD_DEBUG

LDFLAGS_COMMON   = -rdynamic -pthread -lrt -lnuma -laio -lncurses $(LDFLAGS_BOOST)
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

# Compiler and linker flags for S3 support
# "-Wno-overloaded-virtual" because AWS SDK shows a lot of warnings about this otherwise
ifeq ($(S3_SUPPORT), 1)
CXXFLAGS += -DS3_SUPPORT -I $(EXTERNAL_PATH)/aws-sdk-cpp_install/include -Wno-overloaded-virtual
LDFLAGS  += -pthread -L $(EXTERNAL_PATH)/aws-sdk-cpp_install/lib* -l:libaws-sdk-all.a \
	-l ssl -l crypto -l curl -l dl
endif

# Include build helpers for auto detection
include build_helpers/AutoDetection.mk


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

$(OBJECTS): | externals features-info

externals:
ifdef BUILD_VERBOSE
	PREP_AWS_SDK=$(S3_SUPPORT) $(EXTERNAL_PATH)/prepare-external.sh
else
	@PREP_AWS_SDK=$(S3_SUPPORT) $(EXTERNAL_PATH)/prepare-external.sh
endif

features-info:
ifeq ($(CUFILE_SUPPORT),1)
 ifdef BUILD_VERBOSE
	$(info [OPT] CUFILE (GDS) support enabled (CUFILE_INCLUDE_PATH: $(CUFILE_INCLUDE_PATH)))
 else
	$(info [OPT] CUFILE (GDS) support enabled)
 endif
else
	$(info [OPT] CUFILE (GDS) support disabled)
endif
ifeq ($(CUDA_SUPPORT),1)
 ifdef BUILD_VERBOSE
	$(info [OPT] CUDA support enabled (CUDA_INCLUDE_PATH: $(CUDA_INCLUDE_PATH)))
 else
	$(info [OPT] CUDA support enabled)
 endif
else
	$(info [OPT] CUDA support disabled)
endif
ifeq ($(NO_BACKTRACE),1)
	$(info [OPT] Backtrace support disabled)
else
	$(info [OPT] Backtrace support enabled)
endif
ifeq ($(S3_SUPPORT),1)
	$(info [OPT] S3 support enabled)
else
	$(info [OPT] S3 support disabled. (Enable via S3_SUPPORT=1))
endif

clean: clean-packaging clean-buildhelpers
ifdef BUILD_VERBOSE
	rm -rf $(OBJECTS_CLEANUP) $(DEPENDENCY_FILES) $(EXE) $(EXE_UNSTRIPPED)
else
	@echo "[DELETE] OBJECTS, DEPENDENCY_FILES, EXECUTABLES"
	@rm -rf $(OBJECTS_CLEANUP) $(DEPENDENCY_FILES) $(EXE) $(EXE_UNSTRIPPED)
endif

clean-externals:
ifdef BUILD_VERBOSE
	rm -rf $(EXTERNAL_PATH)/Simple-Web-Server 
	rm -rf $(EXTERNAL_PATH)/aws-sdk-cpp $(EXTERNAL_PATH)/aws-sdk-cpp_install
else
	@echo "[DELETE] EXTERNALS"
	@rm -rf $(EXTERNAL_PATH)/Simple-Web-Server
	@rm -rf $(EXTERNAL_PATH)/aws-sdk-cpp $(EXTERNAL_PATH)/aws-sdk-cpp_install
endif

clean-packaging:
ifdef BUILD_VERBOSE
	rm -rf \
		$(PACKAGING_PATH)/BUILDROOT \
		$(PACKAGING_PATH)/RPMS/* $(PACKAGING_PATH)/SPECS/rpm.spec
	bash -c "rm -rf $(PACKAGING_PATH)/$(EXE_NAME)*.{deb,ddeb,build,buildinfo,changes}"
else
	@echo "[DELETE] PACKAGING_FILES"
	@rm -rf \
		$(PACKAGING_PATH)/BUILDROOT \
		$(PACKAGING_PATH)/RPMS/* $(PACKAGING_PATH)/SPECS/rpm.spec
	@bash -c "rm -rf $(PACKAGING_PATH)/$(EXE_NAME)*.{deb,ddeb,build,buildinfo,changes}"
endif

clean-all: clean clean-externals clean-packaging clean-buildhelpers

install: all
	@echo "Installing elbencho..."
	
	install -p -m u=rwx,g=rx,o=rx $(EXE) $(INST_PATH)/
	install -p -m u=rwx,g=rx,o=rx dist/usr/bin/$(EXE_NAME)-chart $(INST_PATH)/
	install -p -m u=rwx,g=rx,o=rx dist/usr/bin/$(EXE_NAME)-scan-path $(INST_PATH)/
	
	install -p -m u=rwx,g=rx,o=rx -D dist/etc/bash_completion.d/$(EXE_NAME) \
		/etc/bash_completion.d/$(EXE_NAME)
	install -p -m u=rwx,g=rx,o=rx -D dist/etc/bash_completion.d/$(EXE_NAME)-chart \
		/etc/bash_completion.d/$(EXE_NAME)-chart

	@echo
	@echo "Installing contributed tools..."
	
	install -p -m u=rwx,g=rx,o=rx contrib/storage_sweep/mtelbencho.sh $(INST_PATH)/
	install -p -m u=rwx,g=rx,o=rx contrib/storage_sweep/graph_sweep.sh $(INST_PATH)/
	
	@echo
	@echo "NOTE: The $(EXE_NAME) executable was installed to $(INST_PATH). The sudo"
	@echo "  command might drop $(INST_PATH) from PATH. In case sudo is needed, the"
	@echo "  absolute path can be used. Or alternatively use a rpm/deb package."

uninstall:
	rm -f $(INST_PATH)/$(EXE_NAME)
	rm -f $(INST_PATH)/$(EXE_NAME)-chart
	rm -f $(INST_PATH)/$(EXE_NAME)-scan-path
	rm -f $(INST_PATH)/mtelbencho.sh
	rm -f $(INST_PATH)/graph_sweep.sh
	rm -f /etc/bash_completion.d/$(EXE_NAME)
	rm -f /etc/bash_completion.d/$(EXE_NAME)-chart

# prepare generic part of build-root (not the .rpm or .deb specific part)
prepare-buildroot: | all clean-packaging
	@echo "[PACKAGING] PREPARE BUILDROOT"

	mkdir -p $(PACKAGING_PATH)/BUILDROOT/$(PKG_INST_PATH)

	# copy main executable
	cp --preserve $(EXE) $(PACKAGING_PATH)/BUILDROOT/$(PKG_INST_PATH)

	# copy contents of dist subdir
	for dir in $(shell find dist/ -mindepth 1 -type d -printf "%P\n"); do \
		mkdir -p $(PACKAGING_PATH)/BUILDROOT/$$dir; \
	done

	for file in $(shell find dist/ -mindepth 1 -type f -printf "%P\n"); do \
		cp --preserve dist/$$file $(PACKAGING_PATH)/BUILDROOT/$$file; \
	done
	
	# copy contents of contrib subdir
	cp --preserve contrib/storage_sweep/mtelbencho.sh $(PACKAGING_PATH)/BUILDROOT/$(PKG_INST_PATH)
	cp --preserve contrib/storage_sweep/graph_sweep.sh $(PACKAGING_PATH)/BUILDROOT/$(PKG_INST_PATH)

rpm: | prepare-buildroot
	@echo "[PACKAGING] PREPARE RPM PACKAGE"

	cp $(PACKAGING_PATH)/SPECS/rpm.spec.template $(PACKAGING_PATH)/SPECS/rpm.spec
	sed -i "s/__NAME__/$(EXE_NAME)/" $(PACKAGING_PATH)/SPECS/rpm.spec
	sed -i "s/__VERSION__/$(EXE_VER_MAJOR).$(EXE_VER_MINOR).$(EXE_VER_PATCHLEVEL)/" \
		$(PACKAGING_PATH)/SPECS/rpm.spec
	
	rpmbuild $(PACKAGING_PATH)/SPECS/rpm.spec --bb --define "_topdir $(PWD)/$(PACKAGING_PATH)" \
		--define "__spec_install_pre /bin/true" --buildroot=$(PWD)/$(PACKAGING_PATH)/BUILDROOT
	
	@echo
	@echo "All done. Your package is here:"
	@find $(PACKAGING_PATH)/RPMS -name $(EXE_NAME)*.rpm

deb: | prepare-buildroot
	@echo "[PACKAGING] PREPARE DEB PACKAGE"

	cp -r $(PACKAGING_PATH)/debian $(PACKAGING_PATH)/BUILDROOT
	
	cp $(PACKAGING_PATH)/BUILDROOT/debian/control.template \
		$(PACKAGING_PATH)/BUILDROOT/debian/control

	sed -i "s/__NAME__/$(EXE_NAME)/" $(PACKAGING_PATH)/BUILDROOT/debian/control
	
	cd $(PACKAGING_PATH)/BUILDROOT && \
		EDITOR=/bin/true VISUAL=/bin/true debchange --create --package elbencho --urgency low \
			--noquery --newversion "$(EXE_VER_MAJOR).$(EXE_VER_MINOR).$(EXE_VER_PATCHLEVEL)" \
			"Custom package build."
	
	cd $(PACKAGING_PATH)/BUILDROOT && \
		debuild -b -us -uc
	
	@echo
	@echo "All done. Your package is here:"
	@find $(PACKAGING_PATH) -name $(EXE_NAME)*.deb

help:
	@echo 'Building Optional Features:'
	@echo '   S3_SUPPORT=1            - Build with S3 support. (Note: This will fetch a'
	@echo '                             AWS SDK git repo of over 1GB size.)'
	@echo '   CUDA_SUPPORT=0|1        - Manually enable (=1) or disable (=0) support for'
	@echo '                             CUDA to work with GPU memory. By default, CUDA'
	@echo '                             support will be enabled when CUDA development files'
	@echo '                             are found. (cuda_runtime.h and libcudart.so)'
	@echo '   CUFILE_SUPPORT=0|1      - Manually enable (=1) or disable (=0) support for'
	@echo '                             GPUDirect Storage (GDS) through the cuFile API.'
	@echo '                             By default, GDS support will be enabled when GDS'
	@echo '                             development files are found. (cufile.h and'
	@echo '                             libcufile.so)'
	@echo '   NO_BACKTRACE=1          - Build without backtrace support for musl-libc.'
	@echo
	@echo 'Optional Compile/Link Arguments:'
	@echo '   CXX=<string>            - Path to alternative C++ compiler. (Default: g++)'
	@echo '   CXXFLAGS_EXTRA=<string> - Additional C++ compiler flags.'
	@echo '   LDFLAGS_EXTRA=<string>  - Additional linker flags.'
	@echo '   BUILD_VERBOSE=1         - Enable verbose build output.'
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
	@echo
	@echo 'Note: Use "make clean" when changing any optional build features.'

.PHONY: clean clean-all clean-externals clean-packaging clean-buildhelpers deb externals \
features-info help prepare-buildroot rpm

.DEFAULT_GOAL := all

# Include dependency files
ifneq ($(DEPENDENCY_FILES),)
include $(DEPENDENCY_FILES)
endif
