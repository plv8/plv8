-- BigInt precedures
-- a BigInt that will work on BigInt
CREATE OR REPLACE FUNCTION bigint_graceful1(val BIGINT)
   RETURNS BIGINT AS $$
    return val - 1n;
   $$ LANGUAGE plv8 STABLE STRICT;
SELECT bigint_graceful1(9223372036854775807);

-- this will fail
SELECT bigint_graceful1(32);

-- a BigInt that will fail on BigInt
CREATE OR REPLACE FUNCTION bigint_graceful2(val BIGINT)
   RETURNS BIGINT AS $$
    return val - 1;
   $$ LANGUAGE plv8 STABLE STRICT;
SELECT bigint_graceful2(32);

-- this will fail
SELECT bigint_graceful2(9223372036854775807);
