/*-------------------------------------------------------------------------
 *
 * pgpatch.c
 *		Module entry point and SQL-callable surface for pgpatch.
 *
 * _PG_init creates the per-backend Janet VM and starts the REPL listener;
 * pgpatch_eval() runs Janet on the main thread and pgpatch_socket_path()
 * reports the REPL socket.  The interesting machinery lives in janet_embed.c
 * (VM), repl.c (socket), patch.c (call/patch), introspect.c + dwarf.c
 * (discovery), and trampoline_arm64.c (code patching).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "fmgr.h"
#include "utils/builtins.h"

#include "pgpatch.h"
#include "repl.h"

PG_MODULE_MAGIC;

void		_PG_init(void);

PG_FUNCTION_INFO_V1(pgpatch_eval);
PG_FUNCTION_INFO_V1(pgpatch_socket_path);

/*
 * pgpatch_eval(code text) RETURNS text
 *
 * Evaluate a chunk of Janet source in this backend's VM and return the printed
 * representation of the result.  Raises ereport(ERROR) on a Janet-level error.
 */
Datum
pgpatch_eval(PG_FUNCTION_ARGS)
{
	text	   *code_arg = PG_GETARG_TEXT_PP(0);
	char	   *code = text_to_cstring(code_arg);
	char	   *result;

	result = pgpatch_janet_eval(code);

	pfree(code);

	PG_RETURN_TEXT_P(cstring_to_text(result));
}

/*
 * pgpatch_socket_path() RETURNS text
 *
 * Report the Unix-domain socket path of this backend's REPL so a client can
 * connect (e.g. `nc -U <path>`).
 */
Datum
pgpatch_socket_path(PG_FUNCTION_ARGS)
{
	const char *path = pgpatch_repl_socket_path();

	PG_RETURN_TEXT_P(cstring_to_text(path));
}

/*
 * Module entry point.  Runs under LOAD 'pgpatch' and when the shared library
 * is first referenced by CREATE EXTENSION.
 */
void
_PG_init(void)
{
	pgpatch_janet_init();
	pgpatch_repl_start();
}
