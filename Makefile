V8DIR = ../v8

SRCS = plv8.cc plv8_type.cc
OBJS = $(SRCS:.cc=.o)
DATA_built = plv8.sql
DATA = uninstall_plv8.sql
SHLIB_LINK += -lv8
MODULE_big = plv8
REGRESS = plv8

CCFLAGS := $(filter-out -Wmissing-prototypes, $(CFLAGS))
CCFLAGS := $(filter-out -Wdeclaration-after-statement, $(CCFLAGS))

%.o : %.cc
	g++ $(CCFLAGS) $(CPPFLAGS) -I $(V8DIR)/include -fPIC -c -o $@ $<

ifdef USE_PGXS
PGXS := $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/plv8
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

# remove dependency to libxml2 and libxslt
LIBS := $(filter-out -lxml2, $(LIBS))
LIBS := $(filter-out -lxslt, $(LIBS))

