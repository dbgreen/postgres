/* src/test/modules/test_guc_context/test_guc_context--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_guc_context" to load this file. \quit

--
-- test_guc_context_get_count()
--
-- Returns the number of items in the current GUC value's list
--
CREATE FUNCTION test_guc_context_get_count()
RETURNS integer
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

--
-- test_guc_context_get_description()
--
-- Returns the description string generated for the current GUC value
--
CREATE FUNCTION test_guc_context_get_description()
RETURNS text
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

--
-- test_guc_context_get_list()
--
-- Returns the comma-separated list reconstructed from the stored List structure
--
CREATE FUNCTION test_guc_context_get_list()
RETURNS text
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;
