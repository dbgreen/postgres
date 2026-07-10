/*-------------------------------------------------------------------------
 *
 * introspect.c
 *		Function and type discovery for pgpatch, exposed to the REPL.
 *
 * Symbol search walks the live process's loaded Mach-O images and their
 * LC_SYMTAB tables (no debug info needed, and it finds non-exported symbols
 * too).  Signature and struct-layout introspection is DWARF-based and lives in
 * dwarf.c; the bindings here just wrap it.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <string.h>

#include "pgpatch_ffi.h"
#include "pgpatch_janet.h"

/* DWARF signature lookup (dwarf.c). */
extern char *pgpatch_dwarf_signature(const char *func, char **err);
extern char *pgpatch_dwarf_struct(const char *name, char **err);
extern bool pgpatch_dwarf_set_path(const char *path);

void		pgpatch_introspect_register(JanetTable *env);
void	   *pgpatch_symbol_addr(const char *want);

/*
 * Locate loaded image `idx`'s symbol table.  Fills the nlist/string-table
 * pointers, symbol count, and ASLR slide; returns false if the image has no
 * usable symbol table.  The tables live in __LINKEDIT, whose in-memory address
 * is (vmaddr + slide - fileoff).
 */
static bool
image_symtab(uint32_t idx, const struct nlist_64 **syms, uint32_t *nsyms,
			 const char **strs, intptr_t *slide)
{
	const struct mach_header_64 *mh;
	const struct load_command *lc;
	const struct symtab_command *symtab = NULL;
	uintptr_t	linkedit_base = 0;
	bool		have_le = false;
	uint32_t	i;

	mh = (const struct mach_header_64 *) _dyld_get_image_header(idx);
	if (mh == NULL || mh->magic != MH_MAGIC_64)
		return false;
	*slide = _dyld_get_image_vmaddr_slide(idx);

	lc = (const struct load_command *) ((uintptr_t) mh + sizeof(*mh));
	for (i = 0; i < mh->ncmds; i++)
	{
		if (lc->cmd == LC_SYMTAB)
			symtab = (const struct symtab_command *) lc;
		else if (lc->cmd == LC_SEGMENT_64)
		{
			const struct segment_command_64 *seg =
				(const struct segment_command_64 *) lc;

			if (strcmp(seg->segname, SEG_LINKEDIT) == 0)
			{
				linkedit_base = (uintptr_t) *slide + seg->vmaddr - seg->fileoff;
				have_le = true;
			}
		}
		lc = (const struct load_command *) ((uintptr_t) lc + lc->cmdsize);
	}
	if (symtab == NULL || !have_le)
		return false;

	*syms = (const struct nlist_64 *) (linkedit_base + symtab->symoff);
	*strs = (const char *) (linkedit_base + symtab->stroff);
	*nsyms = symtab->nsyms;
	return true;
}

/*
 * Name of a defined function/data symbol with its leading underscore stripped,
 * or NULL to skip stab entries, undefined/absolute symbols, and empties.
 */
static const char *
sym_name(const struct nlist_64 *nl, const char *strs)
{
	const char *name;

	if (nl->n_type & N_STAB)
		return NULL;
	if ((nl->n_type & N_TYPE) != N_SECT)
		return NULL;
	if (nl->n_value == 0 || nl->n_un.n_strx == 0)
		return NULL;
	name = strs + nl->n_un.n_strx;
	if (name[0] == '\0')
		return NULL;
	return (name[0] == '_') ? name + 1 : name;
}

/*
 * Append [name addr] pairs whose name contains `pat` to `out`, up to `limit`.
 */
static void
scan_image(uint32_t idx, const char *pat, int32_t limit, JanetArray *out)
{
	const struct nlist_64 *syms;
	const char *strs;
	uint32_t	nsyms,
				i;
	intptr_t	slide;

	if (!image_symtab(idx, &syms, &nsyms, &strs, &slide))
		return;

	for (i = 0; i < nsyms && out->count < limit; i++)
	{
		const char *name = sym_name(&syms[i], strs);
		Janet		pair[2];
		char		addrbuf[20];

		if (name == NULL || strstr(name, pat) == NULL)
			continue;
		snprintf(addrbuf, sizeof(addrbuf), "%p",
				 (void *) ((uintptr_t) syms[i].n_value + slide));
		pair[0] = janet_cstringv(name);
		pair[1] = janet_cstringv(addrbuf);
		janet_array_push(out, janet_wrap_tuple(janet_tuple_n(pair, 2)));
	}
}

/*
 * Resolve an exact function name to its runtime address.  Unlike dlsym, this
 * also finds non-exported (local) symbols, which matters for reaching internal
 * Postgres functions.  Returns NULL if not found.
 */
void *
pgpatch_symbol_addr(const char *want)
{
	uint32_t	nimg = _dyld_image_count();
	uint32_t	idx;

	for (idx = 0; idx < nimg; idx++)
	{
		const struct nlist_64 *syms;
		const char *strs;
		uint32_t	nsyms,
					i;
		intptr_t	slide;

		if (!image_symtab(idx, &syms, &nsyms, &strs, &slide))
			continue;
		for (i = 0; i < nsyms; i++)
		{
			const char *name = sym_name(&syms[i], strs);

			if (name && strcmp(name, want) == 0)
				return (void *) ((uintptr_t) syms[i].n_value + slide);
		}
	}
	return NULL;
}

static Janet
cfun_search(int32_t argc, Janet *argv)
{
	const char *pat;
	int32_t		limit;
	JanetArray *out;
	uint32_t	nimg,
				i;

	janet_arity(argc, 1, 2);
	pat = janet_getcstring(argv, 0);
	limit = (argc >= 2) ? janet_getinteger(argv, 1) : 100;
	if (limit <= 0)
		limit = 100;

	out = janet_array(0);
	nimg = _dyld_image_count();
	for (i = 0; i < nimg && out->count < limit; i++)
		scan_image(i, pat, limit, out);

	return janet_wrap_array(out);
}

/* Panic with a malloc'd DWARF error message (freed here), or a fallback. */
pg_noreturn static void
dwarf_panic(char *err, const char *what)
{
	if (err)
	{
		Janet		j = janet_cstringv(err);

		free(err);
		janet_panicv(j);
	}
	janet_panicf("%s: not found", what);
}

/* Turn a malloc'd DWARF-string result (freed here) into a Janet string. */
static Janet
dwarf_string_result(char *s, char *err)
{
	Janet		result;

	if (s == NULL)
		dwarf_panic(err, "pgpatch");
	result = janet_cstringv(s);
	free(s);
	return result;
}

static Janet
cfun_params(int32_t argc, Janet *argv)
{
	char	   *err = NULL;

	janet_fixarity(argc, 1);
	return dwarf_string_result(pgpatch_dwarf_signature(janet_getcstring(argv, 0),
														&err), err);
}

static Janet
cfun_struct_str(int32_t argc, Janet *argv)
{
	char	   *err = NULL;

	janet_fixarity(argc, 1);
	return dwarf_string_result(pgpatch_dwarf_struct(janet_getcstring(argv, 0),
													&err), err);
}

static Janet
cfun_struct(int32_t argc, Janet *argv)
{
	const char *name;
	char	   *err = NULL;
	PgpStruct	s;
	JanetTable *t;
	JanetArray *fs;
	int			i;

	janet_fixarity(argc, 1);
	name = janet_getcstring(argv, 0);

	if (!pgpatch_dwarf_struct_info(name, &s, &err))
		dwarf_panic(err, "pgpatch/struct");

	t = janet_table(4);
	janet_table_put(t, janet_ckeywordv("name"), janet_cstringv(s.name));
	janet_table_put(t, janet_ckeywordv("kind"), janet_ckeywordv(s.kind));
	janet_table_put(t, janet_ckeywordv("size"), janet_wrap_number((double) s.size));

	fs = janet_array(s.nfields);
	for (i = 0; i < s.nfields; i++)
	{
		JanetTable *f = janet_table(3);

		janet_table_put(f, janet_ckeywordv("offset"),
						s.fields[i].have_offset
						? janet_wrap_number((double) s.fields[i].offset)
						: janet_wrap_nil());
		janet_table_put(f, janet_ckeywordv("name"),
						s.fields[i].name ? janet_cstringv(s.fields[i].name)
						: janet_wrap_nil());
		janet_table_put(f, janet_ckeywordv("type"),
						janet_cstringv(s.fields[i].type));
		janet_array_push(fs, janet_wrap_table(f));
	}
	janet_table_put(t, janet_ckeywordv("fields"), janet_wrap_array(fs));

	pgpatch_dwarf_free_struct(&s);
	return janet_wrap_table(t);
}

static Janet
cfun_set_dwarf(int32_t argc, Janet *argv)
{
	const char *path;

	janet_fixarity(argc, 1);
	path = janet_getcstring(argv, 0);
	if (!pgpatch_dwarf_set_path(path))
		janet_panicf("pgpatch/set-dwarf: could not open DWARF at %s", path);
	return janet_wrap_boolean(1);
}

static const JanetReg cfuns[] = {
	{"search", cfun_search,
		"(pgpatch/search pattern &opt limit)\n\n"
		"Search loaded images for function symbols whose name contains "
		"`pattern`.  Returns an array of [name address] pairs (default limit 100)."},
	{"params", cfun_params,
		"(pgpatch/params name)\n\n"
		"Return the C signature (return type + parameter types/names) of "
		"function `name`, read from DWARF in the .dSYM."},
	{"struct", cfun_struct,
		"(pgpatch/struct name)\n\n"
		"Return the layout of struct/union `name` (or a typedef of one) as a "
		"table {:name :kind :size :fields @[{:offset :name :type} ...]}, from "
		"DWARF.  Use the offsets/types to read fields (e.g. with ffi/read)."},
	{"struct-str", cfun_struct_str,
		"(pgpatch/struct-str name)\n\n"
		"Like pgpatch/struct but returns a formatted, human-readable string; "
		"use (print (pgpatch/struct-str name))."},
	{"set-dwarf", cfun_set_dwarf,
		"(pgpatch/set-dwarf path)\n\n"
		"Point the DWARF reader at a specific .dSYM DWARF file "
		"(<bundle>/Contents/Resources/DWARF/<name>)."},
	{NULL, NULL, NULL}
};

void
pgpatch_introspect_register(JanetTable *env)
{
	janet_cfuns_prefix(env, "pgpatch", cfuns);
}
