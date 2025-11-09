/* src/test/modules/test_guc_extra_context/test_guc_extra_context--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_guc_extra_context" to load this file. \quit

--
-- guc_test_get_context_address()
--
-- Returns the memory context address as a hex string for the current GUC value.
-- This is used to verify that:
--   1. Each SET creates a new context (different addresses)
--   2. ROLLBACK restores the old context (same address)
--
CREATE FUNCTION guc_test_get_context_address()
RETURNS text
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

--
-- guc_test_context_exists()
--
-- Returns true if a context currently exists for the GUC value.
--
CREATE FUNCTION guc_test_context_exists()
RETURNS boolean
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;
