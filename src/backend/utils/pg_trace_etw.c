/*-------------------------------------------------------------------------
 *
 * pg_trace_etw.c
 *	  ETW (Event Tracing for Windows) provider definition and lifecycle.
 *
 * Defines the single "PostgreSQL" TraceLogging provider that the generated
 * TRACE_POSTGRESQL_* macros write to, and the per-process register and
 * unregister entry points from pg_trace_etw.h.  Compiled only with the "etw"
 * option (Windows only).
 *
 * Registration is best-effort.  If it fails, the provider handle stays
 * disabled and every probe macro is a no-op, so a failure never affects
 * server startup.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/pg_trace_etw.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/ipc.h"
#include "utils/pg_trace_etw.h"

/*
 * Define the "PostgreSQL" provider.  The tuple is the stable provider GUID
 * 9ca0b6e3-55e4-406b-84b0-9c61060e0e5a (see pg_trace_etw.h).
 */
TRACELOGGING_DEFINE_PROVIDER(
							 g_hPostgresProvider,
							 "PostgreSQL",
							 (0x9ca0b6e3, 0x55e4, 0x406b, 0x84, 0xb0, 0x9c, 0x61, 0x06, 0x0e, 0x0e, 0x5a));

static bool etw_registered = false;

static void pg_trace_etw_atexit(int code, Datum arg);

/*
 * Register the provider for this process, once.  Safe to call more than once.
 */
void
pg_trace_etw_register(void)
{
	if (etw_registered)
		return;

	if (TraceLoggingRegister(g_hPostgresProvider) == ERROR_SUCCESS)
	{
		etw_registered = true;
		on_proc_exit(pg_trace_etw_atexit, 0);
	}
}

/*
 * Unregister the provider for this process, if registered.
 */
void
pg_trace_etw_unregister(void)
{
	if (!etw_registered)
		return;

	TraceLoggingUnregister(g_hPostgresProvider);
	etw_registered = false;
}

/*
 * on_proc_exit callback wrapper.
 */
static void
pg_trace_etw_atexit(int code, Datum arg)
{
	pg_trace_etw_unregister();
}
