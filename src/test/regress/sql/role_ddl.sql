-- Set fixed timezone for consistent test results
SET timezone = 'UTC';

-- Create test database
CREATE DATABASE regression_role_ddl_test;

-- Test 1: Basic role
CREATE ROLE regress_role_ddl_test1;
SELECT pg_get_role_ddl('regress_role_ddl_test1');

-- Test 2: Role with LOGIN
CREATE ROLE regress_role_ddl_test2 LOGIN;
SELECT pg_get_role_ddl('regress_role_ddl_test2');

-- Test 3: Role with multiple privileges
CREATE ROLE regress_role_ddl_test3 
  LOGIN 
  SUPERUSER 
  CREATEDB 
  CREATEROLE 
  CONNECTION LIMIT 5 
  VALID UNTIL '2030-12-31 23:59:59+00';
SELECT pg_get_role_ddl('regress_role_ddl_test3');

-- Test 4: Role with configuration parameters
CREATE ROLE regress_role_ddl_test4;
ALTER ROLE regress_role_ddl_test4 SET work_mem TO '256MB';
ALTER ROLE regress_role_ddl_test4 SET search_path TO 'myschema, public';
SELECT pg_get_role_ddl('regress_role_ddl_test4');

-- Test 5: Role with database-specific configuration
CREATE ROLE regress_role_ddl_test5;
ALTER ROLE regress_role_ddl_test5 IN DATABASE regression_role_ddl_test SET work_mem TO '128MB';
SELECT pg_get_role_ddl('regress_role_ddl_test5');

-- Test 6: Test pg_get_role_ddl_statements function
SELECT * FROM pg_get_role_ddl_statements('regress_role_ddl_test4');

-- Test 7: Role with special characters (requires quoting)
CREATE ROLE "regress_role-with-dash";
SELECT pg_get_role_ddl('regress_role-with-dash');

-- Test 8: Non-existent role (should return NULL)
SELECT pg_get_role_ddl(9999999::oid);

-- Cleanup
DROP ROLE regress_role_ddl_test1;
DROP ROLE regress_role_ddl_test2;
DROP ROLE regress_role_ddl_test3;
DROP ROLE regress_role_ddl_test4;
DROP ROLE regress_role_ddl_test5;
DROP ROLE "regress_role-with-dash";

DROP DATABASE regression_role_ddl_test;

-- Reset timezone to default
RESET timezone;