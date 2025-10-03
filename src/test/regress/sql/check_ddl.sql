--First we create a normal role to follow the defaults
CREATE ROLE regress_following_defaults;
SELECT pg_get_roledef('regress_following_defaults');

--Then we query using a non-existing role
SELECT pg_get_roledef('regress_non_existing_role');

-- Test role with CONNECTION LIMIT and VALID UNTIL in UTC
CREATE ROLE regress_role_connlimit_valid_until CONNECTION LIMIT 64 VALID UNTIL '2038-01-18 00:00:00-00';
SET TIME ZONE 'utc';
SELECT pg_get_roledef('regress_role_connlimit_valid_until');
RESET TIME ZONE;
