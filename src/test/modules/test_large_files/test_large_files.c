/* src/test/modules/test_large_files/test_large_files.c */

#include "postgres.h"

#include "fmgr.h"
#include "storage/fd.h"
#include "utils/builtins.h"

#ifdef WIN32
#include <windows.h>
#include <winioctl.h>
#endif

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(test_sparse_write_read);
PG_FUNCTION_INFO_V1(test_create_sparse_file);
PG_FUNCTION_INFO_V1(test_verify_offset_native);

/*
 * test_verify_offset_native(filename text, offset_gb numeric, expected_data text) returns boolean
 *
 * Uses native Windows APIs to read data at the specified offset and verify it matches.
 * This ensures PostgreSQL's I/O functions wrote to the CORRECT offset, not a wrapped one.
 * Windows only.
 */
Datum
test_verify_offset_native(PG_FUNCTION_ARGS)
{
#ifdef WIN32
	text	   *filename_text = PG_GETARG_TEXT_PP(0);
	float8		offset_gb = PG_GETARG_FLOAT8(1);
	text	   *expected_text = PG_GETARG_TEXT_PP(2);
	char	   *filename;
	char	   *expected_data;
	char	   *read_buffer;
	int			expected_len;
	int64		offset;
	HANDLE		hFile;
	OVERLAPPED	overlapped = {0};
	DWORD		bytesRead;
	bool		success = false;

	filename = text_to_cstring(filename_text);
	expected_data = text_to_cstring(expected_text);
	expected_len = strlen(expected_data) + 1;

	/* Calculate offset in bytes */
	offset = (int64) (offset_gb * 1024.0 * 1024.0 * 1024.0);

	/* Open file with native Windows API */
	hFile = CreateFile(filename,
					   GENERIC_READ,
					   FILE_SHARE_READ | FILE_SHARE_WRITE,
					   NULL,
					   OPEN_EXISTING,
					   FILE_ATTRIBUTE_NORMAL,
					   NULL);

	if (hFile == INVALID_HANDLE_VALUE)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\" for verification: %lu",
						filename, GetLastError())));

	/* Set up OVERLAPPED structure with proper 64-bit offset */
	overlapped.Offset = (DWORD)(offset & 0xFFFFFFFF);
	overlapped.OffsetHigh = (DWORD)(offset >> 32);

	/* Allocate read buffer */
	read_buffer = palloc(expected_len);

	/* Read using native Windows API */
	if (!ReadFile(hFile, read_buffer, expected_len, &bytesRead, &overlapped))
	{
		DWORD error = GetLastError();
		CloseHandle(hFile);
		pfree(read_buffer);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("native ReadFile failed at offset %lld: %lu",
						offset, error)));
	}

	if (bytesRead != expected_len)
	{
		CloseHandle(hFile);
		pfree(read_buffer);
		ereport(ERROR,
				(errmsg("native ReadFile read %lu bytes, expected %d",
						bytesRead, expected_len)));
	}

	/* Verify data matches */
	success = (memcmp(expected_data, read_buffer, expected_len) == 0);

	pfree(read_buffer);
	CloseHandle(hFile);

	if (!success)
		ereport(ERROR,
				(errmsg("data mismatch at offset %lld: PostgreSQL wrote to wrong location",
						offset)));

	PG_RETURN_BOOL(success);
#else
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("this test is only supported on Windows")));
	PG_RETURN_BOOL(false);
#endif
}

/*
 * test_create_sparse_file(filename text, size_gb int) returns boolean
 *
 * Creates a sparse file of the specified size in gigabytes.
 * Windows only.
 */
Datum
test_create_sparse_file(PG_FUNCTION_ARGS)
{
#ifdef WIN32
	text	   *filename_text = PG_GETARG_TEXT_PP(0);
	int32		size_gb = PG_GETARG_INT32(1);
	char	   *filename;
	HANDLE		hFile;
	DWORD		bytesReturned;
	LARGE_INTEGER fileSize;
	bool		success = false;

	filename = text_to_cstring(filename_text);

	/* Open/create the file */
	hFile = CreateFile(filename,
					   GENERIC_WRITE,
					   0,
					   NULL,
					   CREATE_ALWAYS,
					   FILE_ATTRIBUTE_NORMAL,
					   NULL);

	if (hFile == INVALID_HANDLE_VALUE)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %lu",
						filename, GetLastError())));

	/* Mark as sparse */
	if (!DeviceIoControl(hFile, FSCTL_SET_SPARSE, NULL, 0, NULL, 0,
						 &bytesReturned, NULL))
	{
		CloseHandle(hFile);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not set file sparse: %lu", GetLastError())));
	}

	/* Set file size */
	fileSize.QuadPart = (int64) size_gb * 1024 * 1024 * 1024;
	if (!SetFilePointerEx(hFile, fileSize, NULL, FILE_BEGIN))
	{
		CloseHandle(hFile);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not set file pointer: %lu", GetLastError())));
	}

	if (!SetEndOfFile(hFile))
	{
		CloseHandle(hFile);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not set end of file: %lu", GetLastError())));
	}

	success = true;
	CloseHandle(hFile);

	PG_RETURN_BOOL(success);
#else
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("sparse file test only supported on Windows")));
	PG_RETURN_BOOL(false);
#endif
}

/*
 * test_sparse_write_read(filename text, offset_gb numeric, test_data text) returns boolean
 *
 * Writes test data at the specified offset (in GB) and reads it back to verify.
 * Tests that pg_pwrite and pg_pread work correctly with large offsets.
 * Windows only.
 */
Datum
test_sparse_write_read(PG_FUNCTION_ARGS)
{
#ifdef WIN32
	text	   *filename_text = PG_GETARG_TEXT_PP(0);
	float8		offset_gb = PG_GETARG_FLOAT8(1);
	text	   *test_data_text = PG_GETARG_TEXT_PP(2);
	char	   *filename;
	char	   *test_data;
	char	   *read_buffer;
	int			test_data_len;
	pgoff_t		offset;
	int			fd;
	ssize_t		written;
	ssize_t		nread;
	bool		success = false;

	filename = text_to_cstring(filename_text);
	test_data = text_to_cstring(test_data_text);
	test_data_len = strlen(test_data) + 1;	/* include null terminator */

	/* Calculate offset in bytes */
	offset = (pgoff_t) (offset_gb * 1024.0 * 1024.0 * 1024.0);

	/* Open the file using PostgreSQL's VFD layer */
	fd = BasicOpenFile(filename, O_RDWR | PG_BINARY);
	if (fd < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", filename)));

	/* Write test data at the specified offset using pg_pwrite */
	written = pg_pwrite(fd, test_data, test_data_len, offset);
	if (written != test_data_len)
	{
		close(fd);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to file at offset %lld: wrote %zd of %d bytes",
						(long long) offset, written, test_data_len)));
	}

	/* Allocate buffer for reading */
	read_buffer = palloc(test_data_len);

	/* Read back the data using pg_pread */
	nread = pg_pread(fd, read_buffer, test_data_len, offset);
	if (nread != test_data_len)
	{
		close(fd);
		pfree(read_buffer);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read from file at offset %lld: read %zd of %d bytes",
						(long long) offset, nread, test_data_len)));
	}

	/* Verify data matches */
	success = (memcmp(test_data, read_buffer, test_data_len) == 0);

	pfree(read_buffer);
	close(fd);

	if (!success)
		ereport(ERROR,
				(errmsg("data mismatch: read data does not match written data")));

	PG_RETURN_BOOL(success);
#else
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("this test is only supported on Windows")));
	PG_RETURN_BOOL(false);
#endif
}
