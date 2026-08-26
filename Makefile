
PLV8_VERSION = 3.2.4
V8_CMAKE_DIR := deps/v8-cmake
V8_BUILD_DIR := $(V8_CMAKE_DIR)/build

CP := cp
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
PG_CPPFLAGS := -fPIC -Wall -Wno-register -xc++

SRCS = plv8.cc plv8_type.cc plv8_func.cc plv8_param.cc plv8_allocator.cc plv8_guc.cc
OBJS = $(SRCS:.cc=.o)
MODULE_big = plv8-$(PLV8_VERSION)
EXTENSION = plv8
PLV8_DATA = plv8.control plv8--$(PLV8_VERSION).sql

ifeq ($(OS),Windows_NT)
	# noop for now
else
	SHLIB_LINK += -L$(V8_BUILD_DIR)
	ABSL_LIB_DIRS = $(sort $(dir $(wildcard $(V8_BUILD_DIR)/v8/third_party/abseil-cpp/absl/*/libabsl_*.a)))
	SHLIB_LINK += $(addprefix -L,$(ABSL_LIB_DIRS))
	UNAME_S := $(shell uname -s)
	ifeq ($(UNAME_S),Darwin)
		CCFLAGS += -stdlib=libc++
		SHLIB_LINK += -stdlib=libc++ -lc++
		NUMPROC := $(shell sysctl hw.ncpu | awk '{print $$2}')
		PATCH_V8 := patches/v8-cmake/macos-build.patch
	endif
	ifeq ($(UNAME_S),Linux)
		SHLIB_LINK += -lrt
		NUMPROC := $(shell grep -c ^processor /proc/cpuinfo)
	endif
endif

ifeq ($(NUMPROC),0)
	NUMPROC = 1
endif

all: v8 $(OBJS)

# For some reason, this solves parallel make dependency.
plv8_config.h plv8.so: v8

$(V8_CMAKE_DIR)/README.md:
	@git submodule update --init --recursive
	$(foreach patch,$(PATCH_V8),cd $(V8_CMAKE_DIR) && patch -p1 <../../$(patch);)

$(V8_BUILD_DIR)/libv8_snapshot.a: $(V8_CMAKE_DIR)/README.md
	@cmake -S $(V8_CMAKE_DIR) -B $(V8_BUILD_DIR) -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_BUILD_TYPE=Release && cmake --build $(V8_BUILD_DIR) -j$(NUMPROC) --target v8_snapshot

v8: $(V8_BUILD_DIR)/libv8_snapshot.a

# enable direct jsonb conversion by default
CCFLAGS += -DJSONB_DIRECT_CONVERSION

CCFLAGS += -I$(V8_CMAKE_DIR)/v8/include -Wno-pointer-arith -Wno-comment

ifdef EXECUTION_TIMEOUT
	CCFLAGS += -DEXECUTION_TIMEOUT
endif

ifdef BIGINT_GRACEFUL
	CCFLAGS += -DBIGINT_GRACEFUL
endif

DATA = $(PLV8_DATA)
DATA_built = plv8.sql
REGRESS = init-extension plv8 plv8-errors scalar_args inline json startup_pre startup varparam json_conv \
		  jsonb_conv window guc es6 arraybuffer composites currentresource startup_perms bytea find_function_perms \
		  memory_limits reset show array_spread regression procedure

ifndef BIGINT_GRACEFUL
	REGRESS += bigint
else
	REGRESS += bigint_graceful
endif

SHLIB_LINK += -Wl,--exclude-libs,ALL,--start-group \
	-labsl_base \
	-labsl_city \
	-labsl_civil_time \
	-labsl_debugging_internal \
	-labsl_hash \
	-labsl_hashtablez_sampler \
	-labsl_kernel_timeout_internal \
	-labsl_low_level_hash \
	-labsl_malloc_internal \
	-labsl_raw_hash_set \
	-labsl_raw_logging_internal \
	-labsl_spinlock_wait \
	-labsl_stacktrace \
	-labsl_strings \
	-labsl_synchronization \
	-labsl_throw_delegate \
	-labsl_time_zone \
	-lv8_base_without_compiler \
	-lv8_compiler \
	-lv8_libbase \
	-lv8_libplatform \
	-lv8_libsampler \
	-lv8_simdutf \
	-lv8_snapshot \
	-lv8_torque_generated \
	-Wl,--whole-archive -labsl_time -Wl,--no-whole-archive \
	-Wl,--end-group

OPTFLAGS = -std=c++20 -fno-rtti -O2
CCFLAGS += -Wall $(OPTFLAGS)

generate_upgrades:
	@mkdir -p upgrade
	@./generate_upgrade.sh $(PLV8_VERSION)
	$(eval PLV8_DATA +=  $(wildcard upgrade/*.sql))

all: generate_upgrades

plv8_config.h: plv8_config.h.in Makefile
	sed -e 's/^#undef PLV8_VERSION/#define PLV8_VERSION "$(PLV8_VERSION)"/' $< > $@

%.o : %.cc plv8_config.h plv8.h
	$(CXX) $(CCFLAGS) $(CPPFLAGS) -fconcepts -fPIC -c -o $@ $<

COMPILE.cxx.bc = $(CLANG) -xc++ -Wno-ignored-attributes $(BITCODE_CXXFLAGS) $(CCFLAGS) $(CPPFLAGS) -emit-llvm -c

%.bc : %.cc
	$(COMPILE.cxx.bc) $(CCFLAGS) $(CPPFLAGS) -fPIC -c -o $@ $<
	$(LLVM_BINPATH)/opt -module-summary -f $@ -o $@

DATA_built =

all: $(DATA)

%--$(PLV8_VERSION).sql: plv8.sql.common
	sed -e 's/@LANG_NAME@/$*/g' $< | sed -e 's/@PLV8_VERSION@/$(PLV8_VERSION)/g' | $(CC) -E -P $(CPPFLAGS) -DLANG_$* - > $@

%.control: plv8.control.common
	sed -e 's/@PLV8_VERSION@/$(PLV8_VERSION)/g' $< | $(CXX) -E -P -DLANG_$* - > $@

subclean:
	rm -f plv8_config.h $(DATA)

clean: subclean

distclean: clean
	@cd $(V8_BUILD_DIR) && make clean

.PHONY: subclean all clean installcheck

include $(PGXS)
CC=$(CXX)
