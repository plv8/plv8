V8DIR = ../v8

# set your custom C++ compler
CUSTOM_CC = g++

SRCS = plv8.cc plv8_type.cc plv8_func.cc
OBJS = $(SRCS:.cc=.o)
MODULE_big = plv8
EXTENSION = plv8
DATA = plv8.control plv8--1.0.sql
DATA_built = plv8.sql
REGRESS = init-extension plv8 inline json
override SHLIB_LINK += -lv8

CCFLAGS := $(filter-out -Wmissing-prototypes, $(CFLAGS))
CCFLAGS := $(filter-out -Wdeclaration-after-statement, $(CCFLAGS))

%.o : %.cc
	g++ $(CCFLAGS) $(CPPFLAGS) -I $(V8DIR)/include -fPIC -c -o $@ $<

ifndef USE_PGXS
top_builddir = ../..
makefile_global = $(top_builddir)/src/Makefile.global
ifeq "$(wildcard $(makefile_global))" ""
USE_PGXS = 1	# use pgxs if not in contrib directory
endif
endif

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/$(MODULE_big)
include $(makefile_global)
include $(top_srcdir)/contrib/contrib-global.mk
endif

ifndef MAJORVERSION
MAJORVERSION := $(basename $(VERSION))
endif

# TODO: better idea for "$(MAJORVERSION) >= 9.1" ?
ifeq (,$(findstring $(MAJORVERSION),9.1 9.2))
DATA = uninstall_plv8.sql
REGRESS := init $(filter-out init-extension, $(REGRESS))
plv8.sql.in: plv8.sql.c
	$(CC) -E -P $(CPPFLAGS) $< > $@
subclean:
	rm -f plv8.sql.in
else
plv8.sql:
DATA_built =
install: plv8--1.0.sql
plv8--1.0.sql: plv8.sql.c
	$(CC) -E -P $(CPPFLAGS) $< > $@
subclean:
	rm -f plv8--1.0.sql
endif

ifneq ($(basename $(MAJORVERSION)), 9)
REGRESS := $(filter-out inline, $(REGRESS))
endif

# remove dependency to libxml2 and libxslt
LIBS := $(filter-out -lxml2, $(LIBS))
LIBS := $(filter-out -lxslt, $(LIBS))

.PHONY: subclean
clean: subclean
