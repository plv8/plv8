#!/bin/sh

ORIG=`pwd`
V8DIR=`dirname $0`

if [ -z $1 ]; then
    echo "usage: $0 tagname"
    exit
fi
TAG=$1

cd $V8DIR

GITHEAD=`git symbolic-ref --short HEAD`
echo GITHEAD = $GITHEAD

git checkout $TAG

if [ $? -ne 0 ]; then
    echo "failed to checkout: $TAG"
    exit
fi

V8VER=`v8 -e 'print(JSON.parse(read("META.json")).version)' 2>/dev/null`
if [ -z "$V8VER" ] ; then
    V8VER=`d8 -e 'print(JSON.parse(read("META.json")).version)'`
fi
if [ -z "$V8VER" ] ; then
    echo "Could not get version for plv8"
    exit 1
fi

git archive --output $ORIG/plv8-$V8VER.zip \
            --prefix=plv8-$V8VER/ \
            --format=zip \
            $TAG

git checkout $GITHEAD

