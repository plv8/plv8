#!/bin/sh

ORIG=`pwd`
V8DIR=`dirname $0`

if [ -z $1 ]; then
    echo "usage: $0 tagname"
    exit
fi
TAG=$1

cd $V8DIR
git checkout $TAG

if [ $? -ne 0 ]; then
    echo "failed to checkout: $TAG"
    exit
fi

V8VER=`v8 -e 'print(JSON.parse(read("META.json")).version)'`
git archive --output $ORIG/plv8-$V8VER.zip \
            --prefix=plv8-$V8VER/ \
            --format=zip \
            $TAG
