#-----------------------------------------------------------------------------#
#
# Makefile for v8 static link
#
# 'make' will download the v8 source and build it, then build plv8
# with statically link to v8 with snapshot.  This assumes certain directory
# structure in v8 which may be different from version to another, but user
# can specify the v8 version by AUTOV8_VERSION, too.
#-----------------------------------------------------------------------------#

PLV8_VERSION = 3.1.9

# set your custom C++ compler
CUSTOM_CC = g++
JSS  = coffee-script.js livescript.js
# .cc created from .js
JSCS = $(JSS:.js=.cc)
SRCS = plv8.cc plv8_type.cc plv8_func.cc plv8_param.cc plv8_allocator.cc $(JSCS)
OBJS = $(SRCS:.cc=.o)
MODULE_big = plv8-$(PLV8_VERSION)
EXTENSION = plv8
PLV8_DATA = plv8.control plv8--$(PLV8_VERSION).sql


# Platform detection
ifeq ($(OS),Windows_NT)
	# noop for now
else
	UNAME_S := $(shell uname -s)
	ifeq ($(UNAME_S),Darwin)
		PLATFORM = macos
	endif
	ifeq ($(UNAME_S),Linux)
		PLATFORM = linux
	endif
endif

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)

PG_VERSION_NUM := $(shell cat `$(PG_CONFIG) --includedir-server`/pg_config*.h \
		   | perl -ne 'print $$1 and exit if /PG_VERSION_NUM\s+(\d+)/')

AUTOV8_DIR = build/v8
AUTOV8_OUT = build/v8/out.gn/obj
AUTOV8_STATIC_LIBS = -lv8_libplatform -lv8_libbase

SHLIB_LINK += -L$(AUTOV8_OUT) $(AUTOV8_STATIC_LIBS)

all: v8

# For some reason, this solves parallel make dependency.
plv8_config.h plv8.so: v8

ifdef DOCKER
v8:
	make -f Makefiles/Makefile.docker v8
else
ifeq ($(PLATFORM),linux)
v8:
	make -f Makefiles/Makefile.linux v8
endif

ifeq ($(PLATFORM),macos)
v8:
	make -f Makefiles/Makefile.macos v8
endif
endif

# enable direct jsonb conversion by default
CCFLAGS += -DJSONB_DIRECT_CONVERSION

CCFLAGS += -DV8_COMPRESS_POINTERS=1 -DV8_31BIT_SMIS_ON_64BIT_ARCH=1 -DNDEBUG

CCFLAGS += -I$(AUTOV8_DIR)/include -I$(AUTOV8_DIR)

ifdef EXECUTION_TIMEOUT
	CCFLAGS += -DEXECUTION_TIMEOUT
endif

ifdef BIGINT_GRACEFUL
	CCFLAGS += -DBIGINT_GRACEFUL
endif


# We're gonna build static link.  Rip it out after include Makefile
SHLIB_LINK := $(filter-out -lv8, $(SHLIB_LINK))

CCFLAGS += -std=c++14 -Ibuild/v8/include

ifeq ($(OS),Windows_NT)
	# noop for now
else
	SHLIB_LINK += -L$(AUTOV8_OUT)
	UNAME_S := $(shell uname -s)
	ifeq ($(UNAME_S),Darwin)
		CCFLAGS += -stdlib=libc++
		SHLIB_LINK += -stdlib=libc++ -std=c++14
	endif
	ifeq ($(UNAME_S),Linux)
		SHLIB_LINK += -lrt -std=c++14 
	endif
endif

DATA = $(PLV8_DATA)
ifndef DISABLE_DIALECT
DATA += plcoffee.control plcoffee--$(PLV8_VERSION).sql \
		plls.control plls--$(PLV8_VERSION).sql
endif
DATA_built = plv8.sql
REGRESS = init-extension plv8 plv8-errors scalar_args inline json startup_pre startup varparam json_conv \
		  jsonb_conv window guc es6 arraybuffer composites currentresource startup_perms bytea find_function_perms \
		  memory_limits reset show array_spread regression
ifndef DISABLE_DIALECT
REGRESS += dialect
endif

ifndef BIGINT_GRACEFUL
	REGRESS += bigint
else
	REGRESS += bigint_graceful
endif

ifeq ($(shell test $(PG_VERSION_NUM) -ge 110000 && echo yes), yes)
	REGRESS += procedure
endif

ifdef V8_OUTDIR
SHLIB_LINK += -L$(V8_OUTDIR)
else
SHLIB_LINK += -lv8_libplatform -lv8_libbase -lv8_monolith
endif

OPTFLAGS = -std=c++14 -fno-rtti -O2

CCFLAGS += -Wall $(OPTFLAGS)

ifdef V8_SRCDIR
override CPPFLAGS += -I$(V8_SRCDIR) -I$(V8_SRCDIR)/include
endif

ifeq ($(OS),Windows_NT)
	# noop for now, it could come in handy later
else
	UNAME_S := $(shell uname -s)
	ifeq ($(UNAME_S),Darwin)
		# nothing to do anymore, setting -stdlib=libstdc++ breaks things
	endif
endif

generate_upgrades:
	@mkdir -p upgrade
	@./generate_upgrade.sh $(PLV8_VERSION)
	$(eval PLV8_DATA +=  $(wildcard upgrade/*.sql))

all: generate_upgrades

plv8_config.h: plv8_config.h.in Makefile
	sed -e 's/^#undef PLV8_VERSION/#define PLV8_VERSION "$(PLV8_VERSION)"/' $< > $@

%.o : %.cc plv8_config.h plv8.h
	$(CUSTOM_CC) $(CCFLAGS) $(CPPFLAGS) -fPIC -c -o $@ $<

# Convert .js to .cc
$(JSCS): %.cc: %.js
	echo "extern const unsigned char $(subst -,_,$(basename $@))_binary_data[] = {" >$@
ifndef DISABLE_DIALECT
	(od -txC -v $< | \
	sed -e "s/^[0-9]*//" -e s"/ \([0-9a-f][0-9a-f]\)/0x\1,/g" -e"\$$d" ) >>$@
endif
	echo "0x00};" >>$@

# VERSION specific definitions
ifeq ($(shell test $(PG_VERSION_NUM) -ge 90100 && echo yes), yes)

COMPILE.cxx.bc = $(CLANG) -xc++ -Wno-ignored-attributes $(BITCODE_CXXFLAGS) $(CCFLAGS) $(CPPFLAGS) -emit-llvm -c

%.bc : %.cc
	$(COMPILE.cxx.bc) $(CCFLAGS) $(CPPFLAGS) -fPIC -c -o $@ $<
	$(LLVM_BINPATH)/opt -module-summary -f $@ -o $@

DATA_built =
all: $(DATA)
%--$(PLV8_VERSION).sql: plv8.sql.common
	sed -e 's/@LANG_NAME@/$*/g' $< | sed -e 's/@PLV8_VERSION@/$(PLV8_VERSION)/g' | $(CC) -E -P $(CPPFLAGS) -DLANG_$* - > $@
%.control: plv8.control.common
	sed -e 's/@PLV8_VERSION@/$(PLV8_VERSION)/g' $< | $(CC) -E -P -DLANG_$* - > $@
subclean:
	rm -f plv8_config.h $(DATA) $(JSCS)

else

DATA = uninstall_plv8.sql
%.sql.in: plv8.sql.common
	sed -e 's/@LANG_NAME@/$*/g' $< | $(CC) -E -P $(CPPFLAGS) -DLANG_$* - > $@
subclean:
	rm -f plv8_config.h *.sql.in $(JSCS)

endif

# < 9.4, drop jsonb_conv
ifeq ($(shell test $(PG_VERSION_NUM) -lt 90400 && echo yes), yes)
REGRESS := $(filter-out jsonb_conv, $(REGRESS))
endif

# < 9.2, drop json_conv
ifeq ($(shell test $(PG_VERSION_NUM) -lt 90200 && echo yes), yes)
REGRESS := $(filter-out json_conv, $(REGRESS))
endif

# < 9.1, drop init-extension and dialect, add init at the beginning
ifeq ($(shell test $(PG_VERSION_NUM) -lt 90100 && echo yes), yes)
REGRESS := init $(filter-out init-extension dialect, $(REGRESS))
endif

# < 9.0, drop inline, startup, varparam and window
ifeq ($(shell test $(PG_VERSION_NUM) -lt 90000 && echo yes), yes)
REGRESS := $(filter-out inline startup varparam window, $(REGRESS))
endif

clean: subclean

# build will be created by Makefile.v8
distclean:
	rm -rf build

static: all

integritycheck:
ifneq ($(META_VER),)
	test "$(META_VER)" = "$(PLV8_VERSION)"
endif

installcheck: integritycheck

.PHONY: subclean integritycheck all clean installcheck static

include $(PGXS)
