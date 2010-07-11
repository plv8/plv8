all:
	g++ -O2 -Wall -fPIC -I ../postgres/pgsql/src/include -I ../v8/include -o plv8.o -c plv8.cc
	g++ -lv8 -shared -o plv8.so plv8.o
clean:
	rm plv8.so plv8.o
distclean: clean
realclean: clean

.PHONY: all clean distclean realclean
