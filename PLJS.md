#PLJS

Compile and Install PL/JS source that created by Sam Mason

  * install darcs
```
sudo aptitude install darcs
```

  * get pljs source for the first time
```
darcs get http://xen.samason.me.uk/~sam/repos/pljs/
```

  * to update pljs source, cd to the pljs directory
```
darcs pull
```

  * download spidermonkey source, untar it (tar xvfz)
```
wget -c http://ftp.mozilla.org/pub/mozilla.org/js/js-1.8.0-rc1.tar.gz
```
  * get nspr from jslibs, required by spidermonkey?
```
svn checkout http://jslibs.googlecode.com/svn/trunk/ .
```
  * compile spidermonkey, cd to the extracted spidermonkey source
```
rm -rf Linux_All_DBG.OBJ
make BUILD_OPT=1 JS_DIST=/path/to/jslibs/libs/nspr -f Makefile.ref
sudo mkdir -p /usr/include/smjs/ -v
sudo cp *.h *.tbl /usr/include/smjs/ -v
sudo cp Linux_All_OPT.OBJ/*.h /usr/include/smjs/ -v
sudo mkdir -p /usr/local/{bin,lib}/ -v
sudo cp Linux_All_OPT.OBJ/js /usr/local/bin/ -v
sudo cp Linux_All_OPT.OBJ/libjs.so /usr/local/lib/ -v
sudo ln -s /usr/local/lib/libjs.so /usr/lib/libjs.so
```

  * modify pljs Makefile, if you install postgresql 8.4 and 8.3 altogether
```
MODULE_big = pljs
OBJS= pljs.o jsimpl.o

PG_CPPFLAGS = -DXP_UNIX -I/usr/include/smjs -L /usr/local/bin -L /usr/lib
SHLIB_LINK += -ljs

ifndef DONT_USE_PGXS
PGXS := /usr/lib/postgresql/8.3/lib/pgxs/src/makefiles/pgxs.mk
include $(PGXS)
else
subdir = contrib/pljs
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

etags:
        etags *.[ch]
```

  * compile and install pljs
```
make -f Makefile
sudo make install
```

  * create language, each user should CREATE LANGUAGE pljs; on their own database
```
sudo su - postgres
psql
INSERT INTO pg_pltemplate (tmplname,tmpltrusted,tmpldbacreate,tmplhandler,tmpllibrary) VALUES
    ('pljs', 't', 't', 'pl_js_call_handler', '$libdir/pljs');
CREATE LANGUAGE pljs;
```

  * for more information see his README file