-- src/test/modules/test_large_files/test_large_files--1.0.sql

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_large_files" to load this file. \quit

--
-- test_create_sparse_file(filename text, size_gb int) returns boolean
--
-- Creates a sparse file for testing. Windows only.
--
CREATE FUNCTION test_create_sparse_file(filename text, size_gb int)
RETURNS boolean
AS 'MODULE_PATHNAME', 'test_create_sparse_file'
LANGUAGE C STRICT;

--
-- test_sparse_write_read(filename text, offset_gb numeric, test_data text) returns boolean
--
-- Writes data at a large offset and reads it back to verify correctness.
-- Tests pg_pwrite/pg_pread with offsets beyond 2GB and 4GB. Windows only.
--
CREATE FUNCTION test_sparse_write_read(filename text, offset_gb float8, test_data text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'test_sparse_write_read'
LANGUAGE C STRICT;

--
-- test_verify_offset_native(filename text, offset_gb numeric, expected_data text) returns boolean
--
-- Uses native Windows APIs to verify data is at the correct offset.
-- This ensures PostgreSQL's I/O didn't write to a wrapped/incorrect offset.
--
CREATE FUNCTION test_verify_offset_native(filename text, offset_gb float8, expected_data text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'test_verify_offset_native'
LANGUAGE C STRICT;
