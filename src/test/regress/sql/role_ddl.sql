-- Set fixed timezone for consistent test results
SET timezone = 'UTC'

-- Create test database
CREATE DATABASE role_ddl_test;

-- Test 1: Basic role with defaults
CREATE ROLE test1;
SELECT pg_get_role_ddl('test1');
SELECT pg_get_role_ddl('test1', true);
SELECT pg_get_role_ddl('test1', false);

-- Test 2: Role with LOGIN
CREATE ROLE test2 LOGIN;
SELECT pg_get_role_ddl('test2');

-- Test 3: Role with multiple privileges
CREATE ROLE test3 
  LOGIN 
  SUPERUSER 
  CREATEDB 
  CREATEROLE 
  CONNECTION LIMIT 5 
  VALID UNTIL '2030-12-31 23:59:59+00';
SELECT pg_get_role_ddl('test3');

-- Test 4: Role with configuration parameters
CREATE ROLE test4;
ALTER ROLE test4 SET work_mem TO '256MB';
ALTER ROLE test4 SET search_path TO 'myschema, public';
SELECT pg_get_role_ddl('test4');

-- Test 5: Role with database-specific configuration
CREATE ROLE test5;
ALTER ROLE test5 IN DATABASE role_ddl_test SET work_mem TO '128MB';
SELECT pg_get_role_ddl('test5');

-- Test 6: Test pg_get_role_ddl_statements function
SELECT * FROM pg_get_role_ddl_statements('test4');
SELECT * FROM pg_get_role_ddl_statements('test4', true);
SELECT * FROM pg_get_role_ddl_statements('test4', false);

-- Test 7: Role with special characters (requires quoting)
CREATE ROLE "role-with-dash";
SELECT pg_get_role_ddl('role-with-dash');

-- Test 8: Non-existent role (should return NULL)
SELECT pg_get_role_ddl(9999999::oid);

DROP ROLE test1;
DROP ROLE test2;
DROP ROLE test3;
DROP ROLE test4;
DROP ROLE test5;
DROP ROLE "role-with-dash";
DROP DATABASE role_ddl_test;

-- Reset timezone to default
RESET timezone;