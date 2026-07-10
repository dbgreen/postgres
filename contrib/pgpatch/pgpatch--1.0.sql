/* contrib/pgpatch/pgpatch--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pgpatch" to load this file. \quit

CREATE FUNCTION pgpatch_eval(code text)
RETURNS text
AS 'MODULE_PATHNAME', 'pgpatch_eval'
LANGUAGE C STRICT;

CREATE FUNCTION pgpatch_socket_path()
RETURNS text
AS 'MODULE_PATHNAME', 'pgpatch_socket_path'
LANGUAGE C;
