/*-------------------------------------------------------------------------
 *
 * pgpatch_ffi.h
 *		FFI type descriptors shared between the DWARF reader (dwarf.c, pure C)
 *		and the call machinery (patch.c).  Dependency-free (stdbool only) so it
 *		can be included from the Postgres-free translation units.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PGPATCH_FFI_H
#define PGPATCH_FFI_H

#include <stdbool.h>

/*
 * Machine-type classification of a function's return/parameter types, derived
 * from DWARF and mapped to libffi types by the caller.  Integer/pointer/float
 * classes cover essentially all Postgres internals; struct-by-value is flagged
 * but not marshalled.
 */
typedef enum
{
	PGP_FT_VOID,
	PGP_FT_S8, PGP_FT_U8,
	PGP_FT_S16, PGP_FT_U16,
	PGP_FT_S32, PGP_FT_U32,
	PGP_FT_S64, PGP_FT_U64,
	PGP_FT_FLOAT, PGP_FT_DOUBLE,
	PGP_FT_PTR,
	PGP_FT_STRUCT				/* by-value struct/union: unsupported */
}			PgpFfiType;

#define PGP_MAX_ARGS 12

typedef struct
{
	PgpFfiType	ret;
	int			nargs;
	PgpFfiType	args[PGP_MAX_ARGS];
	bool		has_struct;		/* any struct-by-value arg/return present */
}			PgpSig;

/* Fill *out with the FFI signature of `func` from DWARF; false if not found. */
extern bool pgpatch_dwarf_ffi_sig(const char *func, PgpSig *out);

/* Structured struct/union layout (for programmatic field access). */
typedef struct
{
	unsigned long long offset;	/* byte offset from the start of the struct */
	bool		have_offset;
	char	   *name;			/* field name (malloc'd), or NULL if anonymous */
	char	   *type;			/* field type name (malloc'd) */
}			PgpField;

typedef struct
{
	char	   *name;			/* struct/union name (malloc'd) */
	char	   *kind;			/* "struct" or "union" (malloc'd) */
	unsigned long long size;	/* total size in bytes */
	int			nfields;
	PgpField   *fields;			/* malloc'd array */
}			PgpStruct;

/* Fill *out with the layout of struct/union `name` from DWARF; false if not
 * found.  Free with pgpatch_dwarf_free_struct. */
extern bool pgpatch_dwarf_struct_info(const char *name, PgpStruct *out,
									  char **err);
extern void pgpatch_dwarf_free_struct(PgpStruct *s);

#endif							/* PGPATCH_FFI_H */
