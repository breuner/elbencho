#
# Shared download of cpp-httplib (MIT), the single-header HTTP library used by the S3-over-RDMA
# test tools. Include this after setting EXTERNAL_PATH; it defines HTTPLIB_H and its download rule.
#

HTTPLIB_VERSION   ?= v0.58.0
HTTPLIB_URL       ?= https://raw.githubusercontent.com/yhirose/cpp-httplib/$(HTTPLIB_VERSION)/httplib.h
HTTPLIB_H         := $(EXTERNAL_PATH)/httplib.h

$(HTTPLIB_H):
	@echo "[DOWNLOAD] $(HTTPLIB_URL)"
	@mkdir -p $(EXTERNAL_PATH)
	@if command -v curl >/dev/null 2>&1; then \
	    curl -fLsS -o $@.tmp "$(HTTPLIB_URL)"; \
	elif command -v wget >/dev/null 2>&1; then \
	    wget -q -O $@.tmp "$(HTTPLIB_URL)"; \
	else \
	    echo "ERROR: Neither curl nor wget found to download httplib.h" >&2; exit 1; \
	fi
	@mv $@.tmp $@
