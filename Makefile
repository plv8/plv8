#-----------------------------------------------------------------------------#
#
# Makefile for v8 static link
#
# 'make' will download the v8 source and build it, then build plv8
# with statically link to v8 with snapshot.  This assumes certain directory
# structure in v8 which may be different from version to another, but user
# can specify the v8 version by AUTOV8_VERSION, too.
#-----------------------------------------------------------------------------#

# Platform detection
ifeq ($(OS),Windows_NT)
	# noop for now
else
	UNAME_S := $(shell uname -s)
	ifeq ($(UNAME_S),Darwin)
		PLATFORM = x64.release
	endif
	ifeq ($(UNAME_S),Linux)
		ifeq ($(shell uname -m | grep -o arm),arm)
			PLATFORM = arm64.release
		endif
		ifeq ($(shell uname -m),aarch64)
			PLATFORM = arm64.release
		endif
		ifeq ($(shell uname -m),x86_64)
			PLATFORM = x64.release
		endif
	endif
endif

AUTOV8_VERSION = 8.8.278.14
AUTOV8_DIR = build/v8
AUTOV8_OUT = build/v8/out.gn/$(PLATFORM)/obj
AUTOV8_DEPOT_TOOLS = build/depot_tools
AUTOV8_LIB = $(AUTOV8_OUT)/libv8_snapshot.a
AUTOV8_STATIC_LIBS = -lv8_base_without_compiler -lv8_compiler -lv8_base_without_compiler -lchrome_zlib -lcompression_utils_portable -ltorque_generated_definitions -lcppgc_base -lv8_cppgc_shared -lv8_snapshot -lv8_libplatform -lv8_libbase -lv8_libsampler -lchrome_zlib
ZLIB_DIR = $(AUTOV8_OUT)/third_party/zlib
ZLIB_OBJS = $(ZLIB_DIR)/zlib_adler32_simd/adler32_simd.o $(ZLIB_DIR)/zlib_inflate_chunk_simd/inffast_chunk.o $(ZLIB_DIR)/zlib_inflate_chunk_simd/inflate.o $(ZLIB_DIR)/zlib_crc32_simd/crc32_simd.o $(ZLIB_DIR)/zlib_x86_simd/crc_folding.o $(ZLIB_DIR)/zlib_x86_simd/fill_window_sse.o
export PATH := $(abspath $(AUTOV8_DEPOT_TOOLS)):$(PATH)

SHLIB_LINK += $(ZLIB_OBJS) -L$(AUTOV8_OUT) -L$(AUTOV8_OUT)/third_party/icu -L$(ZLIB_DIR) -L$(AUTOV8_OUT)/third_party/zlib/google $(AUTOV8_STATIC_LIBS)
V8_OPTIONS = is_component_build=false v8_static_library=true v8_use_external_startup_data=false use_custom_libcxx=false

ifndef USE_ICU
	V8_OPTIONS += v8_enable_i18n_support=false
endif

all: v8

# For some reason, this solves parallel make dependency.
plv8_config.h plv8.so: v8

$(AUTOV8_DEPOT_TOOLS):
	mkdir -p build
	cd build \
	&& git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git

ifeq ($(PLATFORM),arm64.release)
$(AUTOV8_DIR): $(AUTOV8_DEPOT_TOOLS)
	# patch with system-installed ninja
	cp /usr/bin/ninja build/depot_tools/

	# get ARM64 clang binaries
	cd build; wget -nc http://releases.llvm.org/7.0.1/clang+llvm-7.0.1-aarch64-linux-gnu.tar.xz; tar xf clang+llvm-7.0.1-aarch64-linux-gnu.tar.xz

	# Get and build the plugin
	cd build; export PATH=$$PATH:`pwd`/clang+llvm-7.0.1-aarch64-linux-gnu/bin; wget -nc https://chromium.googlesource.com/chromium/src/+archive/lkgr/tools/clang/plugins.tar.gz; mkdir -p plugin; cd plugin; tar xf ../plugins.tar.gz; clang++ *.cpp -c -I ../clang+llvm-7.0.1-aarch64-linux-gnu/include/ -fPIC -Wall -std=c++14 -fno-rtti -fno-omit-frame-pointer; clang -shared *.o -o libFindBadConstructs.so

	cp build/plugin/libFindBadConstructs.so build/clang+llvm-7.0.1-aarch64-linux-gnu/lib/

	# Build an ARM64 binary of gn
	cd build; export PATH=$$PATH:`pwd`/clang+llvm-7.0.1-aarch64-linux-gnu/bin; rm -rf gn; git clone https://gn.googlesource.com/gn; cd gn; git checkout 6ae63300be3e9865a72772e4cb6e1f8f667624c4; sed -i -e "s/-Wl,--icf=all//" build/gen.py; python build/gen.py; ninja -C out

	# clone v8
	cd build; fetch v8; cd v8; git checkout $(AUTOV8_VERSION); gclient sync

	# patch v8 with our clang and gn
	cd build/v8; rm -r third_party/llvm-build/Release+Asserts/; mv ../clang+llvm-7.0.1-aarch64-linux-gnu third_party/llvm-build/Release+Asserts; cp ../gn/out/gn buildtools/linux64/gn; 

	cd build/v8; sed -i -e "s/target_cpu=\"x64\" v8_target_cpu=\"arm64/target_cpu=\"arm64\" v8_target_cpu=\"arm64/" infra/mb/mb_config.pyl; tools/dev/v8gen.py $(PLATFORM) -- $(V8_OPTIONS)
else
$(AUTOV8_DIR): $(AUTOV8_DEPOT_TOOLS)
	cd build \
	&& fetch v8 \
	&& cd v8 \
	&& git checkout $(AUTOV8_VERSION) \
	&& gclient sync \
	&& git apply ../../big_int_128.patch \
	&& git apply ../../string_to_array.patch \
	&& tools/dev/v8gen.py $(PLATFORM) -- $(V8_OPTIONS)
endif

$(AUTOV8_OUT)/third_party/icu/common/icudtb.dat:

$(AUTOV8_OUT)/third_party/icu/common/icudtl.dat:

v8: $(AUTOV8_DIR)
	cd $(AUTOV8_DIR) \
	&& env CXXFLAGS=-fPIC CFLAGS=-fPIC ninja -C out.gn/$(PLATFORM) d8

include Makefile.shared

ifdef EXECUTION_TIMEOUT
	CCFLAGS += -DEXECUTION_TIMEOUT
endif

ifdef BIGINT_GRACEFUL
	CCFLAGS += -DBIGINT_GRACEFUL
endif

# enable direct jsonb conversion by default
CCFLAGS += -DJSONB_DIRECT_CONVERSION

# enable pointer compression support (default for v8 > 8.x)
CCFLAGS += -DV8_COMPRESS_POINTERS -DV8_31BIT_SMIS_ON_64BIT_ARCH

CCFLAGS += -I$(AUTOV8_DIR)/include -I$(AUTOV8_DIR)
# We're gonna build static link.  Rip it out after include Makefile
SHLIB_LINK := $(filter-out -lv8, $(SHLIB_LINK))

ifeq ($(OS),Windows_NT)
	# noop for now
else
	SHLIB_LINK += -L$(AUTOV8_OUT)
	UNAME_S := $(shell uname -s)
	ifeq ($(UNAME_S),Darwin)
		CCFLAGS += -stdlib=libc++ -std=c++14
		SHLIB_LINK += -stdlib=libc++
	endif
	ifeq ($(UNAME_S),Linux)
		CCFLAGS += -std=c++14
		SHLIB_LINK += -lrt -std=c++14 -lc++
	endif
endif
