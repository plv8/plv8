V8DIR = ../v8

# set your custom C++ compler
CUSTOM_CC = g++

JSS  = coffee-script.js
# .cc created from .js
JSCS = $(JSS:.js=.cc)
SRCS = plv8.cc plv8_type.cc plv8_func.cc $(JSCS)
OBJS = $(SRCS:.cc=.o)
MODULE_big = plv8
EXTENSION = plv8
EXTVER = 1.1
DATA = plv8.control plv8--$(EXTVER).sql
DATA_built = plv8.sql
REGRESS = init-extension plv8 inline json startup_pre startup
SHLIB_LINK := $(SHLIB_LINK) -lv8

CCFLAGS := $(filter-out -Wmissing-prototypes, $(CFLAGS))
CCFLAGS := $(filter-out -Wdeclaration-after-statement, $(CCFLAGS))
ifdef ENABLE_COFFEE
	CCFLAGS := -DENABLE_COFFEE $(CCFLAGS)
	DATA = plcoffee.control plcoffee--0.9.sql
endif

all:

%.o : %.cc
	g++ $(CCFLAGS) $(CPPFLAGS) -I $(V8DIR)/include -fPIC -c -o $@ $<

# Convert .js to .cc
$(filter $(JSCS), $(SRCS)): %.cc: %.js
	echo "extern const unsigned char $(subst -,_,$(basename $@))_binary_data[] = {" >$@
ifdef ENABLE_COFFEE
	(od -txC -v $< | \
	sed -e "s/^[0-9]*//" -e s"/ \([0-9a-f][0-9a-f]\)/0x\1,/g" -e"\$$d" ) >>$@
endif
	echo "0x00};" >>$@

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

ifndef MAJORVERSION
MAJORVERSION := $(basename $(VERSION))
endif


PG_VERSION_NUM := $(shell perl -ne 'print $$1 if /PG_VERSION_NUM\s+(\d+)/' \
		< `$(PG_CONFIG) --includedir`/pg_config.h)

# VERSION specific definitions
ifeq ($(shell test $(PG_VERSION_NUM) -ge 90100 && echo yes), yes)
plv8.sql:
DATA_built =
install: plv8--$(EXTVER).sql
plv8--$(EXTVER).sql: plv8.sql.c
	$(CC) -E -P $(CPPFLAGS) $< > $@
subclean:
	rm -f plv8--$(EXTVER).sql $(JSCS)
else # 9.1
ifeq ($(shell test $(PG_VERSION_NUM) -ge 90000 && echo yes), yes)
REGRESS := init $(filter-out init-extension, $(REGRESS))
else # 9.0
REGRESS := init $(filter-out init-extension inline startup, $(REGRESS))
endif
DATA = uninstall_plv8.sql
plv8.sql.in: plv8.sql.c
	$(CC) -E -P $(CPPFLAGS) $< > $@
subclean:
	rm -f plv8.sql.in $(JSCS)
endif

ifneq ($(basename $(MAJORVERSION)), 9)
REGRESS := $(filter-out inline, $(REGRESS))
endif

# remove dependency to libxml2 and libxslt
LIBS := $(filter-out -lxml2, $(LIBS))
LIBS := $(filter-out -lxslt, $(LIBS))

.PHONY: subclean
clean: subclean
