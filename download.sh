#!/bin/bash
idir=/home/`whoami`/Repository/plv8js
mkdir -p $idir
cd $idir
#hg clone https://plv8js.googlecode.com/hg/ .
#hg pull https://plv8js.googlecode.com/hg/
#hg push # for push to googlecode
#hg commit -u username@gmail.com -m "commit message"

make USE_PGXS=1
make USE_PGXS=1 install
make USE_PGXS=1 installcheck
