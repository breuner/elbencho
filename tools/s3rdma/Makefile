#
# Convenience Makefile for the S3-over-RDMA test tools: builds all of the tool projects in the
# subdirs, each with its own Makefile. "clean" and "clean-all" are forwarded to them as well.
# The main elbencho Makefile forwards its "clean-all" here.
#

# the shim first, because the client links against it by default
SUBDIRS := cuobjclient-shim server client

# one tool after the other (each one may still compile in parallel): they share the download of
# cpp-httplib into common/external, which must not happen several times at once
.NOTPARALLEL:

.PHONY: all clean clean-all help $(SUBDIRS)

all: $(SUBDIRS)

$(SUBDIRS):
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
