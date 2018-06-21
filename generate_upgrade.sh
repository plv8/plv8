#!/usr/bin/env bash

# version is the first argument, passed in from Makefile
VERSION=$1

older_versions=(1.5.0 1.5.1 1.5.2 1.5.3 1.5.4 1.5.5 1.5.6 1.5.7 2.0.0 2.0.1 2.0.3 2.1.0 2.3.0 2.3.1 2.3.2 2.3.3 2.3.4 2.3.5)

for i in ${older_versions[@]}; do
cat > upgrade/plv8--${i}--$VERSION.sql << EOF
CREATE OR REPLACE FUNCTION plv8_version ( )
RETURNS TEXT AS
\$\$
	return "$VERSION";
\$\$ LANGUAGE plv8;
EOF
done
