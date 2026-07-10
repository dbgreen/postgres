/*-------------------------------------------------------------------------
 *
 * pgpatch.h
 *		Internal interfaces for the pgpatch extension.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PGPATCH_H
#define PGPATCH_H

#include "pgpatch_ffi.h"		/* PgpFfiType */

/*
 * Janet VM lifecycle and evaluation (janet_embed.c).  See that file's header
 * comment for the cross-thread VM model.
 */
extern void pgpatch_janet_init(void);

/* Main-thread SQL path: palloc'd result, ereport(ERROR) on Janet error. */
extern char *pgpatch_janet_eval(const char *code);

/* Any-thread core: malloc'd result (caller frees), sets *is_error.  Never
 * calls into Postgres. */
extern char *pgpatch_janet_eval_locked(const char *code, bool *is_error);

/* 1 if `buf` is a complete (or empty) top-level form, 0 if more input needed. */
extern int	pgpatch_input_complete(const char *buf);

/*
 * Call a stored Janet function (opaque JanetFunction *) as a patched-function
 * replacement, under the VM lock.  The captured general (gp[8]) and floating
 * (fp[8]) argument registers are assigned to the replacement's parameters by
 * replaying the AAPCS classification in `argtypes`.  The result is returned in
 * *ires (integer/pointer) or *dres (float/double), per `ret_type`.  On error
 * returns false with *errmsg set to a malloc'd message (caller frees).  The VM
 * lock is released before returning, so the caller (main thread) may ereport().
 */
extern bool pgpatch_janet_call_typed(void *fn, const PgpFfiType *argtypes,
									 int nargs, const uint64 *gp,
									 const double *fp, PgpFfiType ret_type,
									 uint64 *ires, double *dres, char **errmsg);

#endif							/* PGPATCH_H */
