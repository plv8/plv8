copy prebuild\plv8_config.h .
copy prebuild\coffee-script.cc .
copy prebuild\livescript.cc .
copy prebuild\plv8.control .
copy prebuild\plv8*.sql .
mkdir vendor
cd vendor
nuget install v8-v140-x64 -Version 5.8.283.38
ren v8-v140-x64.5.8.283.38 v8
ren v8.redist-v140-x64.5.8.283.38 v8-dll
cd ..
