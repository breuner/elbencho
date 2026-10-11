#
# Convenience Makefile for the S3-over-RDMA test tools: builds all of the tool projects in the
# subdirs, each with its own Makefile. "clean" and "clean-all" are forwarded to them as well.
# The main elbencho Makefile forwards its "clean-all" here.
#
# With "make -j N" the tools build in parallel. The shared download of cpp-httplib into
# common/external happens here first, so that the sub-makes never download at the same time;
# the client's object files compile while the shim that it links against gets built.
#

SUBDIRS       := cuobjclient-shim server client

EXTERNAL_PATH := common/external

.DEFAULT_GOAL := all
include common/httplib.mk

.PHONY: all clean clean-all help $(SUBDIRS) client-objects

all: $(SUBDIRS)

cuobjclient-shim server: $(HTTPLIB_H)
	$(MAKE) -C $@

client-objects: $(HTTPLIB_H)
	$(MAKE) -C client objects

client: client-objects cuobjclient-shim
	$(MAKE) -C $@

clean clean-all:
	$(foreach dir,$(SUBDIRS),$(MAKE) -C $(dir) $@;)

help:
	@echo 'Targets:'
	@echo '   all (default)            - Build the tools in all subdirs: $(SUBDIRS)'
	@echo '   clean                    - Forward "clean" to all subdirs'
	@echo '   clean-all                - Forward "clean-all" to all subdirs (also removes downloads)'
	@echo
	@echo 'See "make -C <subdir> help" for the options of each tool.'
