/*-------------------------------------------------------------------------
 *
 * handle_info.c
 *		Utility to enumerate and analyze Windows handles, specifically
 *		Section objects (shared memory).
 *
 * This program is used to test that handles are not inherited by child
 * processes when they should have O_CLOEXEC set.
 *
 * Copyright (c) 2025, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#ifdef WIN32
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
/*
 * System information classes for NtQuerySystemInformation
 */
#define SystemHandleInformation 16
#define SystemExtendedHandleInformation 64

/*
 * Object information classes for NtQueryObject
 */
#define ObjectBasicInformation 0
#define ObjectNameInformation 1
#define ObjectTypeInformation 2

/*
 * UNICODE_STRING structure
 */
typedef struct _LSA_UNICODE_STRING
{
	USHORT Length;
	USHORT MaximumLength;
	PWSTR Buffer;
} LSA_UNICODE_STRING, *PLSA_UNICODE_STRING;

/*
 * OBJECT_NAME_INFORMATION structure
 */
typedef struct _OBJECT_NAME_INFORMATION
{
	LSA_UNICODE_STRING Name;
} OBJECT_NAME_INFORMATION, *POBJECT_NAME_INFORMATION;

/*
 * Extended handle information structure
 */
typedef struct _SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX
{
	PVOID Object;
	ULONG_PTR UniqueProcessId;
	ULONG_PTR HandleValue;
	ULONG GrantedAccess;
	USHORT CreatorBackTraceIndex;
	USHORT ObjectTypeIndex;
	ULONG HandleAttributes;
	ULONG Reserved;
} SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX, *PSYSTEM_HANDLE_TABLE_ENTRY_INFO_EX;

typedef struct _SYSTEM_HANDLE_INFORMATION_EX
{
	ULONG_PTR NumberOfHandles;
	ULONG_PTR Reserved;
	SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX Handles[1];
} SYSTEM_HANDLE_INFORMATION_EX, *PSYSTEM_HANDLE_INFORMATION_EX;

/*
 * Object type information
 */
typedef struct _OBJECT_TYPE_INFORMATION
{
	LSA_UNICODE_STRING TypeName;
	ULONG Reserved[22];
} OBJECT_TYPE_INFORMATION, *POBJECT_TYPE_INFORMATION;

/*
 * NT API function pointers
 */
typedef LONG NTSTATUS;

typedef NTSTATUS (NTAPI *NtQuerySystemInformationFunc)(
	ULONG SystemInformationClass,
	PVOID SystemInformation,
	ULONG SystemInformationLength,
	PULONG ReturnLength);

typedef NTSTATUS (NTAPI *NtQueryObjectFunc)(
	HANDLE Handle,
	ULONG ObjectInformationClass,
	PVOID ObjectInformation,
	ULONG ObjectInformationLength,
	PULONG ReturnLength);

typedef NTSTATUS (NTAPI *NtDuplicateObjectFunc)(
	HANDLE SourceProcessHandle,
	HANDLE SourceHandle,
	HANDLE TargetProcessHandle,
	PHANDLE TargetHandle,
	ULONG DesiredAccess,
	ULONG HandleAttributes,
	ULONG Options);

static NtQuerySystemInformationFunc pNtQuerySystemInformation;
static NtQueryObjectFunc pNtQueryObject;
static NtDuplicateObjectFunc pNtDuplicateObject;

/*
 * Initialize NT API function pointers
 */
static bool
InitializeNtApi(void)
{
	HMODULE ntdll = GetModuleHandle("ntdll.dll");
	if (!ntdll)
		return false;

	pNtQuerySystemInformation = (NtQuerySystemInformationFunc)
		GetProcAddress(ntdll, "NtQuerySystemInformation");
	pNtQueryObject = (NtQueryObjectFunc)
		GetProcAddress(ntdll, "NtQueryObject");
	pNtDuplicateObject = (NtDuplicateObjectFunc)
		GetProcAddress(ntdll, "NtDuplicateObject");

	return (pNtQuerySystemInformation && pNtQueryObject && pNtDuplicateObject);
}

/*
 * Get the type name of a handle
 */
static bool
GetHandleTypeName(HANDLE hProcess, HANDLE handle, char *buffer, size_t bufferSize)
{
	HANDLE hDuplicate = NULL;
	OBJECT_TYPE_INFORMATION *typeInfo = NULL;
	ULONG size = 0;
	NTSTATUS status;
	bool result = false;

	/* Duplicate the handle into our process */
	status = pNtDuplicateObject(
		hProcess,
		handle,
		GetCurrentProcess(),
		&hDuplicate,
		0,
		0,
		0);

	if (status != 0 || !hDuplicate)
		return false;

	/* Query the object type */
	typeInfo = (OBJECT_TYPE_INFORMATION *)malloc(1024);
	if (!typeInfo)
		goto cleanup;

	status = pNtQueryObject(
		hDuplicate,
		ObjectTypeInformation,
		typeInfo,
		1024,
		&size);

	if (status == 0 && typeInfo->TypeName.Length > 0)
	{
		int len = WideCharToMultiByte(
			CP_UTF8,
			0,
			typeInfo->TypeName.Buffer,
			typeInfo->TypeName.Length / sizeof(WCHAR),
			buffer,
			(int)(bufferSize - 1),
			NULL,
			NULL);
		if (len > 0)
		{
			buffer[len] = '\0';
			result = true;
		}
	}

cleanup:
	if (typeInfo)
		free(typeInfo);
	if (hDuplicate)
		CloseHandle(hDuplicate);

	return result;
}

/*
 * Get the name of a handle (for Section objects, this is the shared memory name)
 */
static bool
GetHandleName(HANDLE hProcess, HANDLE handle, char *buffer, size_t bufferSize)
{
	HANDLE hDuplicate = NULL;
	OBJECT_NAME_INFORMATION *nameInfo = NULL;
	ULONG size = 0;
	NTSTATUS status;
	bool result = false;

	/* Duplicate the handle into our process */
	status = pNtDuplicateObject(
		hProcess,
		handle,
		GetCurrentProcess(),
		&hDuplicate,
		0,
		0,
		0);

	if (status != 0 || !hDuplicate)
		return false;

	/* Query the object name */
	size = 4096;
	nameInfo = (OBJECT_NAME_INFORMATION *)malloc(size);
	if (!nameInfo)
		goto cleanup;

	status = pNtQueryObject(
		hDuplicate,
		ObjectNameInformation,
		nameInfo,
		size,
		&size);

	if (status == 0 && nameInfo->Name.Length > 0)
	{
		int len = WideCharToMultiByte(
			CP_UTF8,
			0,
			nameInfo->Name.Buffer,
			nameInfo->Name.Length / sizeof(WCHAR),
			buffer,
			(int)(bufferSize - 1),
			NULL,
			NULL);
		if (len > 0)
		{
			buffer[len] = '\0';
			result = true;
		}
	}
	else
	{
		buffer[0] = '\0';
		result = true;  /* Empty name is valid */
	}

cleanup:
	if (nameInfo)
		free(nameInfo);
	if (hDuplicate)
		CloseHandle(hDuplicate);

	return result;
}

/*
 * Enumerate Section handles for a specific process
 */
static int
EnumerateSectionHandles(DWORD pid, bool verbose)
{
	PSYSTEM_HANDLE_INFORMATION_EX handleInfo = NULL;
	ULONG size = 2 * 1024 * 1024;  /* Start with 2MB */
	ULONG returnLength = 0;
	NTSTATUS status;
	HANDLE hProcess = NULL;
	int sectionCount = 0;
	ULONG_PTR i;
	int retry;

	/* Try to query with increasing buffer sizes */
	for (retry = 0; retry < 3; retry++)
	{
		handleInfo = (PSYSTEM_HANDLE_INFORMATION_EX)malloc(size);
		if (!handleInfo)
		{
			fprintf(stderr, "Failed to allocate %lu bytes\n", (unsigned long)size);
			return -1;
		}

		status = pNtQuerySystemInformation(
			SystemExtendedHandleInformation,
			handleInfo,
			size,
			&returnLength);

		if (status == 0)
			break;  /* Success */

		free(handleInfo);
		handleInfo = NULL;

		if (status == 0xC0000004)  /* STATUS_INFO_LENGTH_MISMATCH */
		{
			/* Need larger buffer */
			size = returnLength + 1024;  /* Add some extra space */
			continue;
		}

		/* Other error */
		fprintf(stderr, "NtQuerySystemInformation failed: 0x%08lx\n", (unsigned long)status);
		return -1;
	}

	if (!handleInfo)
	{
		fprintf(stderr, "Failed to query system information after %d retries\n", retry);
		return -1;
	}

	/* Open the target process */
	hProcess = OpenProcess(PROCESS_DUP_HANDLE, FALSE, pid);
	if (!hProcess)
	{
		fprintf(stderr, "Failed to open process %lu: error %lu\n", 
				(unsigned long)pid, GetLastError());
		free(handleInfo);
		return -1;
	}

	if (verbose)
		printf("Scanning %llu handles for process %lu...\n", 
			   (unsigned long long)handleInfo->NumberOfHandles, 
			   (unsigned long)pid);

	/* Iterate through all handles */
	for (i = 0; i < handleInfo->NumberOfHandles; i++)
	{
		SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX *handle = &handleInfo->Handles[i];

		/* Skip handles not belonging to our target process */
		if (handle->UniqueProcessId != pid)
			continue;

		/* Get the handle type */
		char typeName[256];
		if (GetHandleTypeName(hProcess, (HANDLE)(ULONG_PTR)handle->HandleValue, typeName, sizeof(typeName)))
		{
			/* Check if it's a Section object */
			if (strcmp(typeName, "Section") == 0)
			{
				sectionCount++;

				if (verbose)
				{
					char objectName[512];
					if (GetHandleName(hProcess, (HANDLE)(ULONG_PTR)handle->HandleValue, objectName, sizeof(objectName)))
					{
						printf("  Section handle 0x%llx: %s\n",
							   (unsigned long long)handle->HandleValue,
							   objectName[0] ? objectName : "(unnamed)");
					}
					else
					{
						printf("  Section handle 0x%llx: (could not get name)\n",
							   (unsigned long long)handle->HandleValue);
					}
				}
			}
		}
	}

	CloseHandle(hProcess);
	free(handleInfo);

	return sectionCount;
}
/*
 * Compare Section handles between two processes
 */
static void
CompareSectionHandles(DWORD pid1, DWORD pid2)
{
	int count1, count2;

	printf("Comparing Section handles between processes...\n");
	printf("Process 1 (PID %lu):\n", (unsigned long)pid1);
	count1 = EnumerateSectionHandles(pid1, true);

	printf("\nProcess 2 (PID %lu):\n", (unsigned long)pid2);
	count2 = EnumerateSectionHandles(pid2, true);

	printf("\nSummary:\n");
	printf("  Process %lu has %d Section handle(s)\n", (unsigned long)pid1, count1);
	printf("  Process %lu has %d Section handle(s)\n", (unsigned long)pid2, count2);

	if (count2 > 0)
	{
		printf("\nWARNING: Child process has Section handles - possible inheritance!\n");
	}
	else
	{
		printf("\nOK: Child process has no Section handles - no inheritance detected.\n");
	}
}

static void
usage(const char *progname)
{
	printf("Usage: %s [OPTIONS] <pid1> [pid2]\n", progname);
	printf("\nEnumerates Section object handles for Windows processes.\n");
	printf("\nOptions:\n");
	printf("  -v, --verbose    Show detailed handle information\n");
	printf("  -c, --compare    Compare handles between two processes\n");
	printf("  -h, --help       Show this help message\n");
	printf("\nExamples:\n");
	printf("  %s 1234              # Count Section handles for PID 1234\n", progname);
	printf("  %s -v 1234           # List all Section handles for PID 1234\n", progname);
	printf("  %s -c 1234 5678      # Compare handles between PIDs 1234 and 5678\n", progname);
}

int
main(int argc, char *argv[])
{
	DWORD pid1 = 0, pid2 = 0;
	bool verbose = false;
	bool compare = false;
	int i;

	/* Parse command line arguments */
	for (i = 1; i < argc; i++)
	{
		if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0)
		{
			verbose = true;
		}
		else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--compare") == 0)
		{
			compare = true;
		}
		else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
		{
			usage(argv[0]);
			return 0;
		}
		else if (pid1 == 0)
		{
			pid1 = (DWORD)atoi(argv[i]);
		}
		else if (pid2 == 0)
		{
			pid2 = (DWORD)atoi(argv[i]);
		}
	}

	if (pid1 == 0)
	{
		fprintf(stderr, "Error: At least one PID must be specified\n\n");
		usage(argv[0]);
		return 1;
	}

	/* Initialize NT API */
	if (!InitializeNtApi())
	{
		fprintf(stderr, "Failed to initialize NT API functions\n");
		return 1;
	}

	/* Execute the requested operation */
	if (compare && pid2 != 0)
	{
		CompareSectionHandles(pid1, pid2);
	}
	else
	{
		int count = EnumerateSectionHandles(pid1, verbose);
		if (count < 0)
			return 1;

		if (!verbose)
			printf("Process %lu has %d Section handle(s)\n", (unsigned long)pid1, count);
	}

	return 0;
}

#else  /* !WIN32 */

int
main(int argc, char *argv[])
{
	fprintf(stderr, "This program is Windows-specific\n");
	return 1;
}

#endif /* WIN32 */