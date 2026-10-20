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

-- 11. Direct Modify Pushdown
CREATE SERVER test_dm_srv FOREIGN DATA WRAPPER plv8_fdw OPTIONS (wrapper 'MapTableFdw', direct_modify 'true');
CREATE FOREIGN TABLE ft_dm_sessions (
    key text,
    user_id int,
    status text,
    data jsonb
) SERVER test_dm_srv OPTIONS (rowid_column 'key');

EXPLAIN (VERBOSE, COSTS OFF)
DELETE FROM ft_dm_sessions WHERE key = 'sess:3';

DELETE FROM ft_dm_sessions WHERE key = 'sess:3';
SELECT key FROM ft_dm_sessions WHERE key = 'sess:3';

-- 12. Session Reset Coordination
SELECT plv8_reset();
SELECT id, username FROM ft_users WHERE id = 101;
SELECT key, user_id, status FROM ft_dm_sessions WHERE key = 'sess:1';

-- 13. TempTableFdw: In-Memory Temporary Table with Primary Key Pushdown
CREATE SERVER test_temp_srv FOREIGN DATA WRAPPER plv8_fdw OPTIONS (wrapper 'TempTableFdw', pk 'id');

CREATE FOREIGN TABLE ft_temp_products (
    id int,
    name text,
    category text,
    price numeric,
    in_stock boolean
) SERVER test_temp_srv OPTIONS (rowid_column 'id');

-- Insert initial records (single and multi-row batch)
INSERT INTO ft_temp_products (id, name, category, price, in_stock) VALUES
    (1, 'Laptop', 'electronics', 1299.99, true),
    (2, 'Headphones', 'electronics', 199.50, true),
    (3, 'Desk Chair', 'furniture', 349.00, false),
    (4, 'Coffee Mug', 'kitchen', 14.95, true),
    (5, 'Monitor', 'electronics', 450.00, true);

-- Full sequential scan
SELECT id, name, category, price, in_stock FROM ft_temp_products ORDER BY id;

-- Point key lookup pushdown (O(1) indexed key lookup)
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, name, price FROM ft_temp_products WHERE id = 2;

SELECT id, name, price FROM ft_temp_products WHERE id = 2;

-- Batch IN key lookup pushdown
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, name, price FROM ft_temp_products WHERE id IN (1, 3, 5) ORDER BY id;

SELECT id, name, price FROM ft_temp_products WHERE id IN (1, 3, 5) ORDER BY id;

-- Multi-predicate filtering (category & price)
SELECT id, name, price FROM ft_temp_products WHERE category = 'electronics' AND price < 500.00 ORDER BY id;

-- DML: Update
UPDATE ft_temp_products SET name = 'Over-Ear Headphones', price = 179.99, in_stock = false WHERE id = 2;
SELECT id, name, price, in_stock FROM ft_temp_products WHERE id = 2;

-- DML: Delete
DELETE FROM ft_temp_products WHERE id = 4;
SELECT id, name FROM ft_temp_products ORDER BY id;

-- Transaction Rollback Coordination on temp table
BEGIN;
INSERT INTO ft_temp_products (id, name, category, price, in_stock)
VALUES (99, 'Mechanical Keyboard', 'electronics', 129.00, true);
SELECT id, name FROM ft_temp_products WHERE id = 99;
ROLLBACK;

SELECT id, name FROM ft_temp_products WHERE id = 99;

-- 14. RandomPeopleFdw: Synthetic Personal Info Table with Limit Pushdown
CREATE SERVER test_rand_srv FOREIGN DATA WRAPPER plv8_fdw OPTIONS (wrapper 'RandomPeopleFdw');

CREATE FOREIGN TABLE ft_random_people (
    id int,
    first_name text,
    last_name text,
    email text,
    age int,
    city text
) SERVER test_rand_srv;

-- Full scan count (yields 1000 generated personal info records)
SELECT count(*) FROM ft_random_people;

-- Limit pushdown verification via EXPLAIN (VERBOSE, COSTS OFF)
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, first_name, last_name, city FROM ft_random_people LIMIT 5;

-- Execution with pushed-down limit (generates and returns only 5 records)
SELECT id, first_name, last_name, email, age, city FROM ft_random_people LIMIT 5;

-- Limit 2 records
SELECT id, first_name, last_name, city FROM ft_random_people LIMIT 2;

-- Point ID lookup pushdown
SELECT id, first_name, last_name, email, age, city FROM ft_random_people WHERE id = 42;

-- Filter combined with limit pushdown
SELECT id, first_name, age, city FROM ft_random_people WHERE age >= 50 LIMIT 3;

-- Clean up
DROP FOREIGN TABLE ft_users;
DROP FOREIGN TABLE ft_sessions;
DROP FOREIGN TABLE ft_dm_sessions;
DROP FOREIGN TABLE ft_temp_products;
DROP FOREIGN TABLE ft_random_people;
DROP SERVER test_array_srv;
DROP SERVER test_map_srv;
DROP SERVER test_dm_srv;
DROP SERVER test_temp_srv;
DROP SERVER test_rand_srv;


