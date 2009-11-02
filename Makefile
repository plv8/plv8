
PGDIR = ../postgresql
V8DIR = ../v8-1.2

PROG = plv8
OBJS = plv8.o
CC = g++
#CFLAGS = -g -Wall
CFLAGS = -O2 -Wall -fPIC
INCS = -I$(PGDIR)/src/include -I$(V8DIR)/include
LIBS = -lv8

all: $(PROG)

$(PROG):$(OBJS)
	$(CC) $(LIBS) -shared -o $@.so $^
# $(V8DIR)/libv8_g.a

.c.o:
	$(CC) $(CFLAGS) $(INCS) -o $@ -c $<

clean:
	rm -rf $(OBJS)
	rm -rf $(PROG).so
