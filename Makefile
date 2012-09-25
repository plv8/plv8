#-----------------------------------------------------------------------------#
#
# Makefile for plv8
#
# @param V8_SRCDIR path to V8 source directory, used for include files
# @param V8_OUTDIR path to V8 output directory, used for library files
# @param V8_STATIC_SNAPSHOT if defined, statically link to v8 with snapshot
# @param V8_STATIC_NOSNAPSHOT if defined, statically link to v8 w/o snapshot
# @param DISABLE_DIALECT if defined, not build dialects (i.e. plcoffee, etc)
#
# There are three ways to build plv8.
# 1. Dynamic link to v8 (default)
# 2. Static link to v8 with snapshot, if V8_STATIC_SNAPSHOT is defined
# 3. Static link to v8 w/o snapshot, if V8_STATIC_NOSNAPSHOT is defined
# In either case, V8_OUTDIR should point to the v8 output directory (such as
# $(HOME)/v8/out/native) if linker doesn't find it automatically.
#
#-----------------------------------------------------------------------------#
PLV8_VERSION = 1.3.0devel

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
REGRESS = init-extension plv8 inline json startup_pre startup varparam
ifndef DISABLE_DIALECT
REGRESS += dialect
endif

# V8 build options.  See the top comment.
V8_STATIC_SNAPSHOT_LIBS = libv8_base.a libv8_snapshot.a
V8_STATIC_NOSNAPSHOT_LIBS = libv8_base.a libv8_nosnapshot.a
ifdef V8_STATIC_SNAPSHOT
  ifdef V8_OUTDIR
SHLIB_LINK += $(addprefix $(V8_OUTDIR)/, $(V8_STATIC_SNAPSHOT_LIBS))
  else
SHLIB_LINK += $(V8_STATIC_SNAPSHOT_LIBS)
  endif
else
  ifdef V8_STATIC_NOSNAPSHOT
    ifdef V8_OUTDIR
SHLIB_LINK += $(addprefix $(V8_OUTDIR)/, $(V8_STATIC_NOSNAPSHOT_LIBS))
    else
SHLIB_LINK += $(V8_STATIC_NOSNAPSHOT_LIBS)
    endif
  else
SHLIB_LINK += -lv8
    ifdef V8_OUTDIR
SHLIB_LINK += -L$(V8_OUTDIR)
    endif
  endif
endif

OPTFLAGS = -O2
CCFLAGS = -Wall $(OPTFLAGS)
ifdef V8_SRCDIR
override CPPFLAGS += -I$(V8_SRCDIR)/include
endif

all:

plv8_config.h: plv8_config.h.in Makefile
	sed -e 's/^#undef PLV8_VERSION/#define PLV8_VERSION "$(PLV8_VERSION)"/' $< > $@

%.o : %.cc plv8_config.h
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
	sed -e 's/@LANG_NAME@/$*/g' $< | $(CC) -E -P $(CPPFLAGS) - > $@
%.control: plv8.control.common
	sed -e 's/@PLV8_VERSION@/$(PLV8_VERSION)/g' $< | $(CC) -E -P -DLANG_$* - > $@
subclean:
	rm -f plv8_config.h $(DATA) $(JSCS)

else # < 9.1

ifeq ($(shell test $(PG_VERSION_NUM) -ge 90000 && echo yes), yes)
REGRESS := init $(filter-out init-extension dialect, $(REGRESS))

else # < 9.0

REGRESS := init $(filter-out init-extension inline startup varparam dialect, $(REGRESS))

endif

DATA = uninstall_plv8.sql
%.sql.in: plv8.sql.common
	sed -e 's/@LANG_NAME@/$*/g' $< | $(CC) -E -P $(CPPFLAGS) - > $@
subclean:
	rm -f plv8_config.h *.sql.in $(JSCS)

endif

clean: subclean

# Check if META.json.version and PLV8_VERSION is equal.
# Ideally we want to have only one place for this number, but parsing META.json
# is a bit challenging; at one time we had v8/d8 parsing META.json to pass
# this value to source file, but it turned out those utilities may not be
# available everywhere.  Since this integrity matters only developers,
# we just check it if they are available.  We may come up with a better
# solution to have it in one place in the future.
META_VER := $(shell v8 -e 'print(JSON.parse(read("META.json")).version)' 2>/dev/null)
ifndef META_VER
META_VER := $(shell d8 -e 'print(JSON.parse(read("META.json")).version)' 2>/dev/null)
endif

integritycheck:
ifneq ($(META_VER),)
	test "$(META_VER)" = "$(PLV8_VERSION)"
endif

installcheck: integritycheck

.PHONY: subclean integritycheck
include $(PGXS)

# remove dependency to libxml2 and libxslt (should be after include PGXS)
LIBS := $(filter-out -lxml2, $(LIBS))
LIBS := $(filter-out -lxslt, $(LIBS))
