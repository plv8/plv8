-- Regression test for https://github.com/plv8/plv8/issues/606
--
-- A plv8 function compiled and cached (via fn_extra) under one user's
-- context must not be run inside another user's isolate.  The cached
-- state survives across statements only within a transaction, which is
-- why every scenario below runs inside BEGIN/COMMIT.

CREATE FUNCTION sd_plv8_func() RETURNS int AS $$
  var i = 1;
  plv8.elog(NOTICE, "hi there");
  return i;
$$ LANGUAGE plv8;

-- Uses plv8.prepare(), which builds objects from templates that belong
-- to the context of the current user.
CREATE FUNCTION sd_plv8_prepare() RETURNS int AS $$
  var plan = plv8.prepare("SELECT 41 + 1 AS x");
  var rows = plan.execute();
  plan.free();
  return rows[0].x;
$$ LANGUAGE plv8;

CREATE FUNCTION sd_plv8_wrapper() RETURNS void AS $$
DECLARE
  tmp int := 0;
BEGIN
  tmp := sd_plv8_func();
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION sd_plv8_prepare_wrapper() RETURNS int AS $$
DECLARE
  tmp int := 0;
BEGIN
  tmp := sd_plv8_prepare();
  RETURN tmp;
END;
$$ LANGUAGE plpgsql;

CREATE PROCEDURE sd_secdef_proc() AS $$
BEGIN
  RAISE WARNING 'about to call plv8 via wrapper';
  PERFORM sd_plv8_wrapper();
  RAISE WARNING 'plv8 call succeeded';
END;
$$ LANGUAGE plpgsql SECURITY DEFINER;

CREATE PROCEDURE sd_secdef_prepare_proc() AS $$
BEGIN
  RAISE WARNING 'prepare via security definer: %', sd_plv8_prepare_wrapper();
END;
$$ LANGUAGE plpgsql SECURITY DEFINER;

-- Calls a SECURITY DEFINER routine from JavaScript, then keeps using
-- plv8 in the outer (caller's) context afterwards.
CREATE FUNCTION sd_plv8_nested() RETURNS int AS $$
  plv8.execute("CALL sd_secdef_prepare_proc()");
  var plan = plv8.prepare("SELECT 7 AS x");
  var rows = plan.execute();
  plan.free();
  return rows[0].x;
$$ LANGUAGE plv8;

CREATE ROLE sd_test_role LOGIN;
GRANT USAGE ON SCHEMA public TO sd_test_role;
GRANT EXECUTE ON FUNCTION sd_plv8_func() TO sd_test_role;
GRANT EXECUTE ON FUNCTION sd_plv8_prepare() TO sd_test_role;
GRANT EXECUTE ON FUNCTION sd_plv8_wrapper() TO sd_test_role;
GRANT EXECUTE ON FUNCTION sd_plv8_prepare_wrapper() TO sd_test_role;
GRANT EXECUTE ON FUNCTION sd_plv8_nested() TO sd_test_role;
GRANT EXECUTE ON PROCEDURE sd_secdef_proc() TO sd_test_role;
GRANT EXECUTE ON PROCEDURE sd_secdef_prepare_proc() TO sd_test_role;

-- Cache the plv8 function under sd_test_role's context, then call it
-- through a procedure owned by the superuser.
SET ROLE sd_test_role;
BEGIN;
SELECT sd_plv8_wrapper();
CALL sd_secdef_proc();
-- plv8 still works after the cross-context call
SELECT sd_plv8_wrapper();
COMMIT;
RESET ROLE;

-- Same, with a function that uses plv8.prepare()
SET ROLE sd_test_role;
BEGIN;
SELECT sd_plv8_prepare_wrapper();
CALL sd_secdef_prepare_proc();
SELECT sd_plv8_prepare_wrapper();
COMMIT;
RESET ROLE;

-- Nested: JavaScript -> SECURITY DEFINER procedure -> plv8, then more
-- JavaScript in the outer context
SET ROLE sd_test_role;
BEGIN;
SELECT sd_plv8_prepare_wrapper();
SELECT sd_plv8_nested();
SELECT sd_plv8_nested();
COMMIT;
RESET ROLE;

-- Reverse direction: cache under the superuser, then SET ROLE
BEGIN;
SELECT sd_plv8_wrapper();
SET ROLE sd_test_role;
SELECT sd_plv8_wrapper();
RESET ROLE;
SELECT sd_plv8_wrapper();
COMMIT;

-- plv8_reset() disposes of the context the cached function belongs to
BEGIN;
SELECT sd_plv8_prepare_wrapper();
SELECT plv8_reset();
SELECT sd_plv8_prepare_wrapper();
COMMIT;

-- Cleanup
DROP FUNCTION sd_plv8_nested();
DROP PROCEDURE sd_secdef_prepare_proc();
DROP PROCEDURE sd_secdef_proc();
DROP FUNCTION sd_plv8_prepare_wrapper();
DROP FUNCTION sd_plv8_wrapper();
DROP FUNCTION sd_plv8_prepare();
DROP FUNCTION sd_plv8_func();
REVOKE ALL ON SCHEMA public FROM sd_test_role;
DROP ROLE sd_test_role;
