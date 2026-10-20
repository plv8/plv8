--
-- plv8_fdw: PL/v8 Foreign Data Wrapper Regression Tests
--

-- 1. Verify FDW Handler and Validator
SELECT proname FROM pg_proc WHERE proname IN ('plv8_fdw_handler', 'plv8_fdw_validator') ORDER BY 1;
SELECT fdwname FROM pg_foreign_data_wrapper WHERE fdwname = 'plv8_fdw';

-- 2. Create ArrayTableFdw Server and Foreign Table
CREATE SERVER test_array_srv FOREIGN DATA WRAPPER plv8_fdw OPTIONS (wrapper 'ArrayTableFdw');

CREATE FOREIGN TABLE ft_users (
    id int,
    username text,
    dept_id int,
    role text,
    salary numeric
) SERVER test_array_srv;

-- 3. Full sequential scan
SELECT id, username, dept_id, role, salary FROM ft_users ORDER BY id;

-- 4. Column projection pruning
SELECT username, salary FROM ft_users ORDER BY username;

-- 5. Filter pushdown: equality, comparison, IN, IS NOT NULL
EXPLAIN (VERBOSE, COSTS OFF)
SELECT username, salary FROM ft_users WHERE role = 'eng' AND salary > 125000;

SELECT username, salary FROM ft_users WHERE role = 'eng' AND salary > 125000 ORDER BY username;

SELECT username, role FROM ft_users WHERE role IN ('sales', 'lead') ORDER BY username;

-- 6. Pathkeys pre-sorted ordering
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, username FROM ft_users ORDER BY id ASC;

-- 7. Create MapTableFdw Server and Foreign Table (Key-Indexed & DML)
CREATE SERVER test_map_srv FOREIGN DATA WRAPPER plv8_fdw OPTIONS (wrapper 'MapTableFdw');

CREATE FOREIGN TABLE ft_sessions (
    key text,
    user_id int,
    status text,
    data jsonb
) SERVER test_map_srv OPTIONS (rowid_column 'key');

-- Full scan
SELECT key, user_id, status FROM ft_sessions ORDER BY key;

-- Point key lookup pushdown
SELECT key, user_id, status FROM ft_sessions WHERE key = 'sess:1';

-- Batch IN key lookup pushdown
SELECT key, user_id, status FROM ft_sessions WHERE key IN ('sess:1', 'sess:3') ORDER BY key;

-- 8. DML Modifications
-- INSERT
INSERT INTO ft_sessions (key, user_id, status, data)
VALUES ('sess:5', 105, 'active', '{"theme":"nord"}');

SELECT key, user_id, status FROM ft_sessions WHERE key = 'sess:5';

-- UPDATE
UPDATE ft_sessions SET status = 'revoked' WHERE key = 'sess:1';
SELECT key, user_id, status FROM ft_sessions WHERE key = 'sess:1';

-- DELETE
DELETE FROM ft_sessions WHERE key = 'sess:2';
SELECT key, user_id, status FROM ft_sessions WHERE key = 'sess:2';

-- 9. Join Pushdown & Rescan Caching
CREATE TEMP TABLE local_depts (
    dept_id int PRIMARY KEY,
    dept_name text
);
INSERT INTO local_depts VALUES (1, 'Engineering'), (2, 'Sales'), (3, 'Executive');

SELECT d.dept_name, u.username, u.salary
FROM local_depts d
JOIN ft_users u ON u.dept_id = d.dept_id
ORDER BY d.dept_id, u.id;

-- 10. Transaction Rollback Coordination
BEGIN;
INSERT INTO ft_sessions (key, user_id, status, data)
VALUES ('sess:rollback_test', 999, 'transient', '{}');
SELECT key FROM ft_sessions WHERE key = 'sess:rollback_test';
ROLLBACK;

SELECT key FROM ft_sessions WHERE key = 'sess:rollback_test';

-- Clean up
DROP FOREIGN TABLE ft_users;
DROP FOREIGN TABLE ft_sessions;
DROP SERVER test_array_srv;
DROP SERVER test_map_srv;
