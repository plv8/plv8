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

CREATE TRUSTED LANGUAGE plv8
	HANDLER plv8_call_handler
#if PG_VERSION_NUM >= 90000
	INLINE plv8_inline_handler
#endif
	VALIDATOR plv8_call_validator;

#if PG_VERSION_NUM < 90100
COMMIT;
#endif
