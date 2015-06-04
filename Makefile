#-----------------------------------------------------------------------------#
#
# Makefile for plv8
#
# @param DISABLE_DIALECT if defined, not build dialects (i.e. plcoffee, etc)
# @param ENABLE_DEBUGGER_SUPPORT enables v8 debugger agent
#
# There are two ways to build plv8.
# 1. Dynamic link to v8 (default)
#   You need to have libv8.so and header file installed.
# 2. Static link to v8 with snapshot
#   'make static' will download v8 and build, then statically link to it.
#
#-----------------------------------------------------------------------------#
PLV8_VERSION = 1.5.0-dev1

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)

PG_VERSION_NUM := $(shell cat `$(PG_CONFIG) --includedir`/pg_config*.h \
		   | perl -ne 'print $$1 and exit if /PG_VERSION_NUM\s+(\d+)/')

# set your custom C++ compler
CUSTOM_CC = g++
JSS  = coffee-script.js livescript.js
# .cc created from .js
JSCS = $(JSS:.js=.cc)
SRCS = plv8.cc plv8_type.cc plv8_func.cc plv8_param.cc $(JSCS)
OBJS = $(SRCS:.cc=.o)
MODULE_big = plv8
EXTENSION = plv8
PLV8_DATA = plv8.control plv8--$(PLV8_VERSION).sql
DATA = $(PLV8_DATA)
ifndef DISABLE_DIALECT
DATA += plcoffee.control plcoffee--$(PLV8_VERSION).sql \
		plls.control plls--$(PLV8_VERSION).sql
endif
DATA_built = plv8.sql
REGRESS = init-extension plv8 inline json startup_pre startup varparam json_conv \
		  jsonb_conv window guc es6
ifndef DISABLE_DIALECT
REGRESS += dialect
endif

SHLIB_LINK += -lv8
ifdef V8_OUTDIR
SHLIB_LINK += -L$(V8_OUTDIR)
endif

# v8's remote debugger is optional at the moment, since we don't know
# how much of the v8 installation is built with debugger enabled.
ifdef ENABLE_DEBUGGER_SUPPORT
OPT_ENABLE_DEBUGGER_SUPPORT = -DENABLE_DEBUGGER_SUPPORT
endif

# for older g++ (e.g. 4.6.x), which do not support c++11
#OPTFLAGS = -O2 -std=gnu++0x -fno-rtti

OPTFLAGS = -O2 -std=c++11 -fno-rtti

CCFLAGS = -Wall $(OPTFLAGS) $(OPT_ENABLE_DEBUGGER_SUPPORT)

ifdef V8_SRCDIR
override CPPFLAGS += -I$(V8_SRCDIR) -I$(V8_SRCDIR)/include
endif

ifeq ($(OS),Windows_NT)
	# noop for now, it could come in handy later
else
	UNAME_S := $(shell uname -s)
	ifeq ($(UNAME_S),Darwin)
		CCFLAGS += -stdlib=libstdc++
		SHLIB_LINK := -stdlib=libstdc++
		SHLIB_LINK += -lv8_base -lv8_libbase -lv8_libplatform -lv8_snapshot
	endif
endif

all:

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

DATA_built =
all: $(DATA)
%--$(PLV8_VERSION).sql: plv8.sql.common
	sed -e 's/@LANG_NAME@/$*/g' $< | $(CC) -E -P $(CPPFLAGS) -DLANG_$* - > $@
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

static:
	$(MAKE) -f Makefile.v8

# Check if META.json.version and PLV8_VERSION is equal.
# Ideally we want to have only one place for this number, but parsing META.json
# is a bit challenging; at one time we had v8/d8 parsing META.json to pass
# this value to source file, but it turned out those utilities may not be
# available everywhere.  Since this integrity matters only developers,
# we just check it if they are available.  We may come up with a better
# solution to have it in one place in the future.
META_VER = $(shell v8 -e 'print(JSON.parse(read("META.json")).version)' 2>/dev/null)
ifndef META_VER
META_VER = $(shell d8 -e 'print(JSON.parse(read("META.json")).version)' 2>/dev/null)
endif
ifndef META_VER
META_VER = $(shell lsc -e 'console.log(JSON.parse(require("fs").readFileSync("META.json")).version)' 2>/dev/null)
endif
ifndef META_VER
META_VER = $(shell coffee -e 'console.log(JSON.parse(require("fs").readFileSync("META.json")).version)' 2>/dev/null)
endif
ifndef META_VER
META_VER = $(shell node -e 'console.log(JSON.parse(require("fs").readFileSync("META.json")).version)' 2>/dev/null)
endif

integritycheck:
ifneq ($(META_VER),)
	test "$(META_VER)" = "$(PLV8_VERSION)"
endif

installcheck: integritycheck

.PHONY: subclean integritycheck
include $(PGXS)
