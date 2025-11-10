-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_guc_contexts" to load this file. \quit

-- Simple GUC functions
CREATE FUNCTION show_simple_guc_items()
RETURNS text
AS 'MODULE_PATHNAME', 'show_simple_guc_items'
LANGUAGE C STRICT;

CREATE FUNCTION count_simple_guc_items()
RETURNS integer
AS 'MODULE_PATHNAME', 'count_simple_guc_items'
LANGUAGE C STRICT;

-- Complex GUC functions
CREATE FUNCTION show_plan_shapes()
RETURNS text
AS 'MODULE_PATHNAME', 'show_plan_shapes'
LANGUAGE C STRICT;

CREATE FUNCTION count_plan_shapes()
RETURNS integer
AS 'MODULE_PATHNAME', 'count_plan_shapes'
LANGUAGE C STRICT;

CREATE FUNCTION lookup_plan_shape(text)
RETURNS text
AS 'MODULE_PATHNAME', 'lookup_plan_shape'
LANGUAGE C STRICT;
