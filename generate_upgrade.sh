#!/usr/bin/env bash

# version is the first argument, passed in from Makefile
VERSION=$1

older_versions=(1.5.0 1.5.1 1.5.2 1.5.3 1.5.4 1.5.5 1.5.6 1.5.7 2.0.0 2.0.1 2.0.3 2.1.0 2.1.2 2.3.0 2.3.1 2.3.2 2.3.3 2.3.4 2.3.5 2.3.6 2.3.7 2.3.8 2.3.9 2.3.10 2.3.11 2.3.12)

for i in ${older_versions[@]}; do
cat > upgrade/plv8--${i}--$VERSION.sql << EOF
CREATE OR REPLACE FUNCTION plv8_version ( )
RETURNS TEXT AS
\$\$
	return "$VERSION";
\$\$ LANGUAGE plv8;


CREATE OR REPLACE FUNCTION plv8_call_handler() RETURNS language_handler
 AS 'MODULE_PATHNAME' LANGUAGE C;
CREATE OR REPLACE FUNCTION plv8_inline_handler(internal) RETURNS void
 AS 'MODULE_PATHNAME' LANGUAGE C;
CREATE OR REPLACE FUNCTION plv8_call_validator(oid) RETURNS void
 AS 'MODULE_PATHNAME' LANGUAGE C;
EOF
done
