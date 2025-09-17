--First we create a normal role to follow the defaults
CREATE ROLE regress_following_defaults;
SELECT pg_get_roledef('regress_following_defaults');