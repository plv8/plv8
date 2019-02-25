#!/bin/bash
# all-in-one script for performing binary releases to dockerhub
#
# end users: please see README.md for how to use these releases
# developers: feel free to use your own version of this-- see Dockerfile

PLV8_VERSION=2.3.9

# for official releases, login as plv8user
docker login

# TODO: support plv8 on alpine, then add ",-alpine" which is already supported by postgres
for PG_PLATFORM in {"",}; do
    for PG_VERSION in {9.4,9.5,9.6,10,11}; do
	PG=$PG_VERSION$PG_PLATFORM
	echo "building for $PG..."
	docker build --build-arg PLV8_VERSION=$PLV8_VERSION --build-arg PG_VERSION=$PG -t plv8user/plv8:$PLV8_VERSION-pg$PG . -f Dockerfile$PG_PLATFORM > docker-build-$PG.out 2>&1 &
    done
done

# build in parallel
wait

# build times are long, so perform the push async
# TODO: support plv8 on alpine, then add ",-alpine" which is already supported by postgres
for PG_PLATFORM in {"",}; do
    for PG_VERSION in {9.4,9.5,9.6,10,11}; do
	PG=$PG_VERSION$PG_PLATFORM
	docker push plv8user/plv8:$PLV8_VERSION-pg$PG"
    done
done
