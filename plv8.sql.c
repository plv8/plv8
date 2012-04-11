#include "pg_config.h"
#if PG_VERSION_NUM < 90100
-- adjust this setting to control where the objects get created.
SET search_path = public;

BEGIN;
#endif

CREATE FUNCTION plv8_call_handler() RETURNS language_handler
	AS 'MODULE_PATHNAME' LANGUAGE C;

#if PG_VERSION_NUM >= 90000
CREATE FUNCTION plv8_inline_handler(internal) RETURNS void
	AS 'MODULE_PATHNAME' LANGUAGE C;
#endif

CREATE FUNCTION plv8_call_validator(oid) RETURNS void
	AS 'MODULE_PATHNAME' LANGUAGE C;

DELETE FROM pg_pltemplate WHERE tmplname = 'plv8';
INSERT INTO pg_pltemplate (
	tmplname,
	tmpltrusted,
	tmpldbacreate,
	tmplhandler,
#if PG_VERSION_NUM >= 90000
	tmplinline,
#endif
	tmplvalidator,
	tmpllibrary)
SELECT
	'plv8',
	true,
	false,
	'plv8_call_handler',
#if PG_VERSION_NUM >= 90000
	'plv8_inline_handler',
#endif
	'plv8_call_validator',
	'MODULE_PATHNAME'
;

#if PG_VERSION_NUM < 90100
COMMIT;
#else
CREATE LANGUAGE plv8;
#endif
