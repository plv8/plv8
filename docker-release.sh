#!/bin/bash
# all-in-one script for performing binary releases to dockerhub
#
# end users: please see README.md for how to use these releases
# developers: feel free to use your own version of this-- see Dockerfile

PLV8_VERSION=2.3.9

# for official releases, login as plv8user
docker login

for PG_VERSION in {9.6,10,11}; do
    echo "building for $PG_VERSION..."
    docker build --build-arg PLV8_VERSION=$PLV8_VERSION --build-arg PG_VERSION=$PG_VERSION -t plv8user/plv8:$PLV8_VERSION-pg$PG_VERSION . > docker-build-$PG_VERSION.out 2>&1 &
done

# build in parallel
wait

# build times are long, so 
for PG_VERSION in {9.6,10,11}; do
    docker push plv8user/plv8:$PLV8_VERSION-pg$PG_VERSION
done
