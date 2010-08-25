sub version {
	return $_[1] if ($_[0] <= $ARGV[0]);
}

print << "EOF";
-- adjust this setting to control where the objects get created.
SET search_path = public;

BEGIN;

CREATE FUNCTION plv8_call_handler() RETURNS language_handler
	AS 'MODULE_PATHNAME' LANGUAGE C;

@{[ &version(9.0, << "VERSION"
CREATE FUNCTION plv8_inline_handler(internal) RETURNS void
	AS 'MODULE_PATHNAME' LANGUAGE C;
VERSION
)]}
CREATE FUNCTION plv8_call_validator(oid) RETURNS void
	AS 'MODULE_PATHNAME' LANGUAGE C;

INSERT INTO pg_pltemplate (
	tmplname,
	tmpltrusted,
	tmpldbacreate,
	tmplhandler,
	@{[ &version(9.0, "tmplinline,") ]}
	tmplvalidator,
	tmpllibrary)
SELECT
	'plv8',
	true,
	false,
	'plv8_call_handler',
	@{[ &version(9.0, "'plv8_inline_handler',") ]}
	'plv8_call_validator',
	'MODULE_PATHNAME'
WHERE
	NOT EXISTS (SELECT 1 FROM pg_pltemplate WHERE tmplname = 'plv8')
;

COMMIT;
EOF
