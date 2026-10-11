#!/usr/bin/env bash

# version is the first argument, passed in from Makefile
VERSION=$1
OUTDIR=${2:-upgrade}

older_versions=(1.5.0 1.5.1 1.5.2 1.5.3 1.5.4 1.5.5 1.5.6 1.5.7 2.0.0 2.0.1 2.0.3 2.1.0 2.1.2 2.3.0 2.3.1 2.3.2 2.3.3 2.3.4 2.3.5 2.3.6 2.3.7 2.3.8 2.3.9 2.3.10 2.3.11 2.3.12 2.3.13 2.3.14 2.3.15 3.0.0 3.0.1 3.1.0 3.1.1 3.1.2 3.1.3 3.1.4 3.1.5 3.1.6 3.1.7 3.1.8 3.2.0 3.2.1 3.2.2)

for i in ${older_versions[@]}; do
cat > $OUTDIR/plv8--${i}--$VERSION.sql << EOF
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

CREATE SCHEMA IF NOT EXISTS plv8;

CREATE TABLE IF NOT EXISTS plv8.packages (
	name TEXT NOT NULL,
	version TEXT NOT NULL,
	entry_point TEXT NOT NULL DEFAULT 'index.js',
	dependencies JSONB DEFAULT '{}',
	created_at TIMESTAMP WITH TIME ZONE NOT NULL DEFAULT NOW(),
	updated_at TIMESTAMP WITH TIME ZONE,
	PRIMARY KEY (name, version)
);

CREATE TABLE IF NOT EXISTS plv8.modules (
	path TEXT NOT NULL,
	source TEXT,
	package_name TEXT,
	package_version TEXT,
	created_at TIMESTAMP WITH TIME ZONE NOT NULL DEFAULT NOW(),
	updated_at TIMESTAMP WITH TIME ZONE,
	sloppy BOOLEAN NOT NULL DEFAULT false,
	bytecode BYTEA,
	requires TEXT[],
	is_trusted BOOLEAN NOT NULL DEFAULT true,
	FOREIGN KEY (package_name, package_version)
		REFERENCES plv8.packages (name, version)
);

CREATE UNIQUE INDEX IF NOT EXISTS plv8_modules_standalone_path
	ON plv8.modules (path) WHERE package_name IS NULL;

CREATE UNIQUE INDEX IF NOT EXISTS plv8_modules_package_path
	ON plv8.modules (package_name, package_version, path)
	WHERE package_name IS NOT NULL;

CREATE INDEX IF NOT EXISTS plv8_modules_path ON plv8.modules (path);
CREATE INDEX IF NOT EXISTS plv8_modules_pkg ON plv8.modules (package_name, package_version, path);
CREATE INDEX IF NOT EXISTS plv8_packages_name ON plv8.packages (name);

SELECT pg_catalog.pg_extension_config_dump('plv8.packages', '');
SELECT pg_catalog.pg_extension_config_dump('plv8.modules', '');

GRANT USAGE ON SCHEMA plv8 TO PUBLIC;
GRANT SELECT ON plv8.packages TO PUBLIC;
GRANT SELECT ON plv8.modules TO PUBLIC;

CREATE OR REPLACE FUNCTION plv8_modules_changed()
	RETURNS trigger
	LANGUAGE C AS 'MODULE_PATHNAME', 'plv8_modules_changed';
REVOKE ALL ON FUNCTION plv8_modules_changed() FROM PUBLIC;

CREATE TRIGGER plv8_modules_changed
	AFTER INSERT OR UPDATE OR DELETE OR TRUNCATE ON plv8.modules
	FOR EACH STATEMENT EXECUTE FUNCTION plv8_modules_changed();
ALTER TABLE plv8.modules ENABLE ALWAYS TRIGGER plv8_modules_changed;

CREATE TRIGGER plv8_modules_changed
	AFTER INSERT OR UPDATE OR DELETE OR TRUNCATE ON plv8.packages
	FOR EACH STATEMENT EXECUTE FUNCTION plv8_modules_changed();
ALTER TABLE plv8.packages ENABLE ALWAYS TRIGGER plv8_modules_changed;
EOF
done
