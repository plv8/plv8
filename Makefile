#-----------------------------------------------------------------------------#
#
# Makefile for v8 static link
#
# 'make' will download the v8 source and build it, then build plv8
# with statically link to v8 with snapshot.  This assumes certain directory
# structure in v8 which may be different from version to another, but user
# can specify the v8 version by AUTOV8_VERSION, too.
#-----------------------------------------------------------------------------#
AUTOV8_VERSION = 7.0.276.20

# https://stackoverflow.com/a/23324703
ROOT_DIR:=$(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))
BUILD_DIR = $(ROOT_DIR)/build
AUTOV8_DIR = $(BUILD_DIR)/v8
AUTOV8_OUT = $(BUILD_DIR)/v8/out.gn/x64.release/obj
AUTOV8_DEPOT_TOOLS = $(BUILD_DIR)/depot_tools
AUTOV8_LIB = $(AUTOV8_OUT)/libv8_snapshot.a
AUTOV8_STATIC_LIBS = -lv8_base -lv8_snapshot -lv8_libplatform -lv8_libbase -lv8_libsampler

export PATH := $(abspath $(AUTOV8_DEPOT_TOOLS)):$(PATH)

SHLIB_LINK += -L$(AUTOV8_OUT) -L$(AUTOV8_OUT)/third_party/icu $(AUTOV8_STATIC_LIBS)
V8_OPTIONS = is_component_build=false v8_static_library=true v8_use_snapshot=true v8_use_external_startup_data=false

ifndef USE_ICU
	V8_OPTIONS += v8_enable_i18n_support=false
endif

all: check-tools v8

# For some reason, this solves parallel make dependency.
plv8_config.h plv8.so: v8

$(AUTOV8_DEPOT_TOOLS):
	mkdir -p build
	cd build; git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git
	rm -fr $(BUILD_DIR)/.gclient   # so fetch is clean

$(BUILD_DIR)/autov8_fetched: $(AUTOV8_DEPOT_TOOLS)
	rm -fr $(AUTOV8_DIR) $(BUILD_DIR)/.gclient
	cd $(BUILD_DIR); fetch $(notdir $(AUTOV8_DIR)) && \
		rm -f $(BUILD_DIR)/autov8_fetched; echo $(shell date) > $(BUILD_DIR)/autov8_fetched

$(BUILD_DIR)/autov8_gitcheckedout: $(BUILD_DIR)/autov8_fetched
	cd $(AUTOV8_DIR); git checkout $(AUTOV8_VERSION) && \
		rm -f $(BUILD_DIR)/autov8_gitcheckedout; echo $(shell date) > $(BUILD_DIR)/autov8_gitcheckedout

$(BUILD_DIR)/autov8_gclient_synced: $(BUILD_DIR)/autov8_gitcheckedout
	cd $(AUTOV8_DIR); gclient sync && \
		rm -f $(BUILD_DIR)/autov8_gclient_synced; echo $(shell date) > $(BUILD_DIR)/autov8_gclient_synced

$(BUILD_DIR)/autov8_cherry_picked: $(BUILD_DIR)/autov8_gclient_synced
	cd $(AUTOV8_DIR)/build/config; git cherry-pick 4287a0d364541583a50cc91465330251460d489a && \
		rm -f $(BUILD_DIR)/autov8_cherry_picked; echo $(shell date) > $(BUILD_DIR)/autov8_cherry_picked

$(BUILD_DIR)/autov8_v8gen: $(BUILD_DIR)/autov8_cherry_picked
	rm -fr $(AUTOV8_DIR)/out.gn
	cd $(AUTOV8_DIR); tools/dev/v8gen.py $(PLATFORM) -- $(V8_OPTIONS) && \
		rm -f $(BUILD_DIR)/autov8_v8gen; echo $(shell date) > $(BUILD_DIR)/autov8_v8gen

$(AUTOV8_OUT)/third_party/icu/common/icudtb.dat:

$(AUTOV8_OUT)/third_party/icu/common/icudtl.dat:

v8: $(BUILD_DIR)/autov8_v8gen
	cd $(AUTOV8_DIR) ; env CXXFLAGS=-fPIC CFLAGS=-fPIC ninja -C out.gn/$(PLATFORM) d8

include Makefile.shared

ifdef EXECUTION_TIMEOUT
	CCFLAGS += -DEXECUTION_TIMEOUT
endif

ifdef BIGINT_GRACEFUL
	CCFLAGS += -DBIGINT_GRACEFUL
endif

CCFLAGS += -I$(AUTOV8_DIR)/include -I$(AUTOV8_DIR)
# We're gonna build static link.  Rip it out after include Makefile
SHLIB_LINK := $(filter-out -lv8, $(SHLIB_LINK))
PKGCONFIG := na
PYTHON := na
GIT := na
PYTHON_VERSION := na
ifeq ($(OS),Windows_NT)
	# noop for now
else
	PKGCONFIG := $(shell which pkg-config)
	PYTHON := $(shell which python)
	GIT := $(shell which git)
	SHLIB_LINK += -L$(AUTOV8_OUT)
	UNAME_S := $(shell uname -s)
	ifeq ($(UNAME_S),Darwin)
		CCFLAGS += -stdlib=libc++ -std=c++11
		SHLIB_LINK += -stdlib=libc++
		PLATFORM = x64.release
	endif
	ifeq ($(UNAME_S),Linux)
		ifeq ($(shell uname -m | grep -o arm),arm)
			PLATFORM = arm64.release
		endif
		ifeq ($(shell uname -m),x86_64)
			PLATFORM = x64.release
		endif
		CCFLAGS += -std=c++11
		SHLIB_LINK += -lrt -std=c++11 -lc++
	endif
endif

.PHONY: check-tools
check-tools:
ifeq ($(PKGCONFIG),)
	$(error "(linux) plv8 compilation requires pkg-config(1) - please install.")
endif
ifeq ($(PYTHON),)
	$(error "(linux) plv8 compilation requires python - please install.")
endif
ifeq ($(GIT),)
	$(error "(linux) plv8 compilation requires git - please install.")
endif
ifneq ($(PYTHON),)
ifneq ($(shell $(PYTHON) -c 'import platform;print(platform.python_version_tuple()[0])'),2)
	$(error "(linux) plv8 compilation requires python2 (e.g. not python 3) - please install.")
endif
endif

