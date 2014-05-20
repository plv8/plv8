DO $$ plv8.elog(NOTICE, 'this', 'is', 'inline', 'code') $$ LANGUAGE plv8;
DO $$ plv8.return_next(new Object());$$ LANGUAGE plv8;
