-- Regression test for https://github.com/plv8/plv8/issues/606
-- Crash when a cached plv8 function is called from a different security context.

CREATE FUNCTION sd_plv8_func() RETURNS int AS $$
  var i = 1;
  plv8.elog(NOTICE, "hi there");
  return i;
$$ LANGUAGE plv8;

CREATE FUNCTION sd_plv8_wrapper() RETURNS void AS $$
DECLARE
  tmp int := 0;
BEGIN
  tmp := sd_plv8_func();
END;
$$ LANGUAGE plpgsql;

CREATE PROCEDURE sd_secdef_proc() AS $$
BEGIN
  RAISE WARNING 'about to call plv8 via wrapper';
  PERFORM sd_plv8_wrapper();
  RAISE WARNING 'plv8 call succeeded';
END;
$$ LANGUAGE plpgsql SECURITY DEFINER;

CREATE ROLE sd_test_role LOGIN;
GRANT USAGE ON SCHEMA public TO sd_test_role;
GRANT EXECUTE ON FUNCTION sd_plv8_func() TO sd_test_role;
GRANT EXECUTE ON FUNCTION sd_plv8_wrapper() TO sd_test_role;
GRANT EXECUTE ON PROCEDURE sd_secdef_proc() TO sd_test_role;

-- Cache the plv8 function under sd_test_role's isolate
SET ROLE sd_test_role;
SELECT sd_plv8_wrapper();

-- The procedure is owned by the superuser, so GetPlv8Context() switches
-- to a different isolate while fn_extra still holds sd_test_role's.
-- On unfixed code this crashes the server.
CALL sd_secdef_proc();

-- Preservation: plv8 still works after the cross-context call
SELECT sd_plv8_wrapper();
RESET ROLE;

-- Reverse direction: superuser cache, then SET ROLE
SELECT sd_plv8_func();
SET ROLE sd_test_role;
SELECT sd_plv8_func();
RESET ROLE;

-- Cleanup
DROP PROCEDURE sd_secdef_proc();
DROP FUNCTION sd_plv8_wrapper();
DROP FUNCTION sd_plv8_func();
REVOKE ALL ON SCHEMA public FROM sd_test_role;
DROP ROLE sd_test_role;
