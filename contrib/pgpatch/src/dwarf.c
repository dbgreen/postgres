/*-------------------------------------------------------------------------
 *
 * dwarf.c
 *		Minimal DWARF reader for pgpatch: function signatures, argument type
 *		classifications, and struct/union layouts, read from a .dSYM.
 *
 * Scope is deliberately narrow: DWARF 5 / DWARF32 as emitted by clang on
 * macOS.  We walk compile units and their abbrev tables, find the *definition*
 * DIE for a name (skipping the many DW_AT_declaration copies, and following
 * DW_AT_abstract_origin/specification for optimized code), and resolve type
 * references (base, pointer, const/volatile, typedef, struct/union/enum,
 * array).
 *
 * Pure C: no Janet, no Postgres.  Returns malloc'd data.  Unsupported forms or
 * malformed input bail gracefully (NULL / error string), never crashing on
 * well-formed clang output.
 *
 *-------------------------------------------------------------------------
 */
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- DWARF constants (only what we use) ---- */
#define DW_TAG_array_type		0x01
#define DW_TAG_enumeration_type 0x04
#define DW_TAG_formal_parameter 0x05
#define DW_TAG_pointer_type		0x0f
#define DW_TAG_compile_unit		0x11
#define DW_TAG_structure_type	0x13
#define DW_TAG_typedef			0x16
#define DW_TAG_union_type		0x17
#define DW_TAG_base_type		0x24
#define DW_TAG_const_type		0x26
#define DW_TAG_subprogram		0x2e
#define DW_TAG_member			0x0d
#define DW_TAG_volatile_type	0x35
#define DW_TAG_restrict_type	0x37

#define DW_AT_name				0x03
#define DW_AT_byte_size			0x0b
#define DW_AT_encoding			0x3e
#define DW_AT_low_pc			0x11
#define DW_AT_data_member_location 0x38
#define DW_AT_abstract_origin	0x31
#define DW_AT_declaration		0x3c
#define DW_AT_specification		0x47
#define DW_AT_type				0x49
#define DW_AT_str_offsets_base	0x72
#define DW_AT_addr_base			0x73

/* DW_AT_encoding values (for base types). */
#define DW_ATE_address			0x01
#define DW_ATE_boolean			0x02
#define DW_ATE_float			0x04
#define DW_ATE_signed			0x05
#define DW_ATE_signed_char		0x06
#define DW_ATE_unsigned			0x07
#define DW_ATE_unsigned_char	0x08

#define DW_FORM_addr			0x01
#define DW_FORM_block2			0x03
#define DW_FORM_block4			0x04
#define DW_FORM_data2			0x05
#define DW_FORM_data4			0x06
#define DW_FORM_data8			0x07
#define DW_FORM_string			0x08
#define DW_FORM_block			0x09
#define DW_FORM_block1			0x0a
#define DW_FORM_data1			0x0b
#define DW_FORM_flag			0x0c
#define DW_FORM_sdata			0x0d
#define DW_FORM_strp			0x0e
#define DW_FORM_udata			0x0f
#define DW_FORM_ref_addr		0x10
#define DW_FORM_ref1			0x11
#define DW_FORM_ref2			0x12
#define DW_FORM_ref4			0x13
#define DW_FORM_ref8			0x14
#define DW_FORM_ref_udata		0x15
#define DW_FORM_indirect		0x16
#define DW_FORM_sec_offset		0x17
#define DW_FORM_exprloc			0x18
#define DW_FORM_flag_present	0x19
#define DW_FORM_strx			0x1a
#define DW_FORM_addrx			0x1b
#define DW_FORM_data16			0x1e
#define DW_FORM_line_strp		0x1f
#define DW_FORM_ref_sig8		0x20
#define DW_FORM_implicit_const	0x21
#define DW_FORM_loclistx		0x22
#define DW_FORM_rnglistx		0x23
#define DW_FORM_strx1			0x25
#define DW_FORM_strx2			0x26
#define DW_FORM_strx3			0x27
#define DW_FORM_strx4			0x28
#define DW_FORM_addrx1			0x29
#define DW_FORM_addrx2			0x2a
#define DW_FORM_addrx3			0x2b
#define DW_FORM_addrx4			0x2c

#include "pgpatch_ffi.h"

/* Public entry points (also declared where they are used, in introspect.c). */
bool		pgpatch_dwarf_set_path(const char *path);
char	   *pgpatch_dwarf_default_path(void);
char	   *pgpatch_dwarf_signature(const char *func, char **err);
bool		pgpatch_dwarf_ffi_sig(const char *func, PgpSig *out);
char	   *pgpatch_dwarf_struct(const char *name, char **err);

typedef struct
{
	const uint8_t *p;
	size_t		len;
}			Slice;

/* Loaded .dSYM DWARF sections. */
static struct
{
	void	   *map;
	size_t		maplen;
	Slice		info;
	Slice		abbrev;
	Slice		str;
	Slice		str_offs;
	bool		loaded;
}			D;

/* ---- little readers ---- */

static uint64_t
read_uleb(const uint8_t **pp, const uint8_t *end)
{
	uint64_t	r = 0;
	int			shift = 0;
	const uint8_t *p = *pp;

	while (p < end)
	{
		uint8_t		b = *p++;

		r |= (uint64_t) (b & 0x7f) << shift;
		if (!(b & 0x80))
			break;
		shift += 7;
	}
	*pp = p;
	return r;
}

static int64_t
read_sleb(const uint8_t **pp, const uint8_t *end)
{
	int64_t		r = 0;
	int			shift = 0;
	const uint8_t *p = *pp;
	uint8_t		b = 0;

	while (p < end)
	{
		b = *p++;
		r |= (int64_t) (b & 0x7f) << shift;
		shift += 7;
		if (!(b & 0x80))
			break;
	}
	if (shift < 64 && (b & 0x40))
		r |= -((int64_t) 1 << shift);
	*pp = p;
	return r;
}

static uint32_t
read_u32(const uint8_t *p)
{
	uint32_t	v;

	memcpy(&v, p, 4);
	return v;
}

static uint64_t
read_uN(const uint8_t **pp, int n)
{
	uint64_t	v = 0;

	memcpy(&v, *pp, (size_t) n);
	*pp += n;
	return v;
}

/* ---- Mach-O section loading ---- */

static void
dwarf_close(void)
{
	if (D.map)
		munmap(D.map, D.maplen);
	memset(&D, 0, sizeof(D));
}

static bool
find_sections(void)
{
	const struct mach_header_64 *mh = (const struct mach_header_64 *) D.map;
	const struct load_command *lc;
	uint32_t	i;

	if (D.maplen < sizeof(*mh) || mh->magic != MH_MAGIC_64)
		return false;

	lc = (const struct load_command *) ((uintptr_t) mh + sizeof(*mh));
	for (i = 0; i < mh->ncmds; i++)
	{
		if (lc->cmd == LC_SEGMENT_64)
		{
			const struct segment_command_64 *seg =
				(const struct segment_command_64 *) lc;

			if (strcmp(seg->segname, "__DWARF") == 0)
			{
				const struct section_64 *sec =
					(const struct section_64 *) ((uintptr_t) seg + sizeof(*seg));
				uint32_t	s;

				for (s = 0; s < seg->nsects; s++, sec++)
				{
					Slice		sl = {(const uint8_t *) D.map + sec->offset,
						(size_t) sec->size};
					char		nm[17];

					/* sectname is a fixed char[16], not NUL-terminated when
					 * full (e.g. "__debug_str_offs" is exactly 16 chars). */
					memcpy(nm, sec->sectname, 16);
					nm[16] = '\0';

					if (strcmp(nm, "__debug_info") == 0)
						D.info = sl;
					else if (strcmp(nm, "__debug_abbrev") == 0)
						D.abbrev = sl;
					else if (strcmp(nm, "__debug_str") == 0)
						D.str = sl;
					else if (strcmp(nm, "__debug_str_offs") == 0)
						D.str_offs = sl;
				}
			}
		}
		lc = (const struct load_command *) ((uintptr_t) lc + lc->cmdsize);
	}
	return D.info.p && D.abbrev.p && D.str.p && D.str_offs.p;
}

bool
pgpatch_dwarf_set_path(const char *path)
{
	int			fd;
	struct stat st;

	dwarf_close();

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return false;
	if (fstat(fd, &st) != 0 || st.st_size <= 0)
	{
		close(fd);
		return false;
	}
	D.maplen = (size_t) st.st_size;
	D.map = mmap(NULL, D.maplen, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (D.map == MAP_FAILED)
	{
		D.map = NULL;
		return false;
	}
	if (!find_sections())
	{
		dwarf_close();
		return false;
	}
	D.loaded = true;
	return true;
}

/*
 * Default dSYM DWARF path derived from the main executable:
 *   <exe>.dSYM/Contents/Resources/DWARF/<basename>
 * Caller frees.
 */
char *
pgpatch_dwarf_default_path(void)
{
	char		exe[4096];
	uint32_t	sz = sizeof(exe);
	const char *base;
	char	   *out;

	if (_NSGetExecutablePath(exe, &sz) != 0)
		return NULL;
	base = strrchr(exe, '/');
	base = base ? base + 1 : exe;

	out = malloc(strlen(exe) + strlen(base) + 64);
	if (!out)
		return NULL;
	sprintf(out, "%s.dSYM/Contents/Resources/DWARF/%s", exe, base);
	return out;
}

/* ---- abbrev table (parsed for a given offset, cached) ---- */

typedef struct
{
	uint64_t	attr;
	uint64_t	form;
	int64_t		implicit;		/* for DW_FORM_implicit_const */
}			AttrSpec;

typedef struct
{
	uint64_t	tag;
	bool		has_children;
	AttrSpec   *attrs;
	int			nattrs;
}			Abbrev;

typedef struct
{
	Abbrev	   *by_code;			/* indexed by code; index 0 unused */
	int			ncodes;
	uint64_t	off;
	bool		valid;
}			AbbrevTable;

static AbbrevTable AT;			/* single cached table (all CUs share off 0) */

static void
free_abbrev_table(void)
{
	if (AT.by_code)
	{
		int			i;

		for (i = 0; i < AT.ncodes; i++)
			free(AT.by_code[i].attrs);
		free(AT.by_code);
	}
	memset(&AT, 0, sizeof(AT));
}

static bool
parse_abbrev(uint64_t off)
{
	const uint8_t *p = D.abbrev.p + off;
	const uint8_t *end = D.abbrev.p + D.abbrev.len;
	int			cap = 64;

	if (AT.valid && AT.off == off)
		return true;			/* cached */
	free_abbrev_table();

	AT.by_code = calloc((size_t) cap, sizeof(Abbrev));
	AT.ncodes = 1;
	AT.off = off;

	while (p < end)
	{
		uint64_t	code = read_uleb(&p, end);
		Abbrev		ab;
		int			acap = 8;

		if (code == 0)
			break;				/* end of this table */

		memset(&ab, 0, sizeof(ab));
		ab.tag = read_uleb(&p, end);
		ab.has_children = (*p++ != 0);
		ab.attrs = malloc((size_t) acap * sizeof(AttrSpec));

		for (;;)
		{
			AttrSpec	sp;

			sp.attr = read_uleb(&p, end);
			sp.form = read_uleb(&p, end);
			sp.implicit = 0;
			if (sp.form == DW_FORM_implicit_const)
				sp.implicit = read_sleb(&p, end);
			if (sp.attr == 0 && sp.form == 0)
				break;
			if (ab.nattrs >= acap)
			{
				acap *= 2;
				ab.attrs = realloc(ab.attrs, (size_t) acap * sizeof(AttrSpec));
			}
			ab.attrs[ab.nattrs++] = sp;
		}

		if ((int) code >= AT.ncodes)
		{
			int			need = (int) code + 1;

			if (need > cap)
			{
				while (cap < need)
					cap *= 2;
				AT.by_code = realloc(AT.by_code, (size_t) cap * sizeof(Abbrev));
				memset(AT.by_code + AT.ncodes, 0,
					   (size_t) (cap - AT.ncodes) * sizeof(Abbrev));
			}
			AT.ncodes = need;
		}
		AT.by_code[code] = ab;
	}
	AT.valid = true;
	return true;
}

/* ---- per-CU context ---- */

typedef struct
{
	const uint8_t *cu_start;	/* start of CU (its first byte in info) */
	const uint8_t *die_start;	/* first DIE (after CU header) */
	const uint8_t *cu_end;		/* end of this CU */
	int			addr_size;
	uint64_t	str_offsets_base;
}			CU;

/* An extracted attribute value. */
typedef struct
{
	uint64_t	uval;			/* numeric / index / offset */
	const char *sval;			/* resolved string (or NULL) */
}			AttrVal;

static const char *
resolve_strx(CU *cu, uint64_t index)
{
	const uint8_t *e = D.str_offs.p + cu->str_offsets_base + index * 4;

	if (e + 4 > D.str_offs.p + D.str_offs.len)
		return "?";
	{
		uint32_t	so = read_u32(e);

		if (so >= D.str.len)
			return "?";
		return (const char *) (D.str.p + so);
	}
}

/*
 * Read one attribute value of the given form, advancing *pp.  Fills av.
 * Returns false on an unsupported form (caller should bail).
 */
static bool
read_form(CU *cu, uint64_t form, int64_t implicit,
		  const uint8_t **pp, const uint8_t *end, AttrVal *av)
{
	const uint8_t *p = *pp;

	av->uval = 0;
	av->sval = NULL;

	switch (form)
	{
		case DW_FORM_addr:
			av->uval = read_uN(&p, cu->addr_size);
			break;
		case DW_FORM_data1:
		case DW_FORM_ref1:
		case DW_FORM_flag:
		case DW_FORM_strx1:
		case DW_FORM_addrx1:
			av->uval = read_uN(&p, 1);
			break;
		case DW_FORM_data2:
		case DW_FORM_ref2:
		case DW_FORM_strx2:
		case DW_FORM_addrx2:
			av->uval = read_uN(&p, 2);
			break;
		case DW_FORM_strx3:
		case DW_FORM_addrx3:
			av->uval = read_uN(&p, 3);
			break;
		case DW_FORM_data4:
		case DW_FORM_ref4:
		case DW_FORM_ref_addr:
		case DW_FORM_sec_offset:
		case DW_FORM_strp:
		case DW_FORM_line_strp:
		case DW_FORM_strx4:
		case DW_FORM_addrx4:
			av->uval = read_uN(&p, 4);
			break;
		case DW_FORM_data8:
		case DW_FORM_ref8:
		case DW_FORM_ref_sig8:
			av->uval = read_uN(&p, 8);
			break;
		case DW_FORM_data16:
			p += 16;
			break;
		case DW_FORM_sdata:
			av->uval = (uint64_t) read_sleb(&p, end);
			break;
		case DW_FORM_udata:
		case DW_FORM_ref_udata:
		case DW_FORM_strx:
		case DW_FORM_addrx:
		case DW_FORM_loclistx:
		case DW_FORM_rnglistx:
			av->uval = read_uleb(&p, end);
			break;
		case DW_FORM_string:
			av->sval = (const char *) p;
			p += strlen((const char *) p) + 1;
			break;
		case DW_FORM_flag_present:
			av->uval = 1;
			break;
		case DW_FORM_implicit_const:
			av->uval = (uint64_t) implicit;
			break;
		case DW_FORM_exprloc:
		case DW_FORM_block:
			{
				uint64_t	n = read_uleb(&p, end);

				p += n;
			}
			break;
		case DW_FORM_block1:
			{
				uint64_t	n = read_uN(&p, 1);

				p += n;
			}
			break;
		case DW_FORM_block2:
			{
				uint64_t	n = read_uN(&p, 2);

				p += n;
			}
			break;
		case DW_FORM_block4:
			{
				uint64_t	n = read_uN(&p, 4);

				p += n;
			}
			break;
		default:
			return false;		/* unsupported */
	}

	/* Resolve string-ish forms to a char*. */
	if (form == DW_FORM_strp || form == DW_FORM_line_strp)
	{
		/* line_strp points at __debug_line_str which we don't load; only
		 * __debug_str (strp) is resolved. */
		if (form == DW_FORM_strp && av->uval < D.str.len)
			av->sval = (const char *) (D.str.p + av->uval);
	}
	else if (form == DW_FORM_strx || form == DW_FORM_strx1 ||
			 form == DW_FORM_strx2 || form == DW_FORM_strx3 ||
			 form == DW_FORM_strx4)
	{
		av->sval = resolve_strx(cu, av->uval);
	}

	*pp = p;
	return true;
}

/*
 * Read a DIE header (abbrev code) at *pp.  Returns the Abbrev (or NULL for a
 * null DIE / end-of-children) and advances *pp past the code.
 */
static const Abbrev *
read_die_code(const uint8_t **pp, const uint8_t *end)
{
	uint64_t	code = read_uleb(pp, end);

	if (code == 0)
		return NULL;
	if ((int) code >= AT.ncodes || AT.by_code[code].attrs == NULL)
		return NULL;
	return &AT.by_code[code];
}

/* Forward decl for recursive type naming. */
static void namestr_append(char **buf, size_t *len, size_t *cap, const char *s);

/*
 * Read the DIE at CU-relative offset `off` and fill in *name and the type
 * reference, following DW_AT_abstract_origin / DW_AT_specification when those
 * attributes are absent.  Optimized (out-of-line) definitions carry only
 * low_pc and an abstract_origin; the name/params live on the referenced
 * abstract DIE.  depth guards against cycles/self-references.
 */
static void
die_get(CU *cu, uint64_t off, const char **name,
		uint64_t *type_ref, bool *have_type, int depth)
{
	const uint8_t *p = cu->cu_start + off;
	const uint8_t *end = cu->cu_end;
	const Abbrev *ab;
	uint64_t	origin = 0;
	bool		have_origin = false;
	int			i;

	if (depth > 6 || p >= end)
		return;
	ab = read_die_code(&p, end);
	if (ab == NULL)
		return;

	for (i = 0; i < ab->nattrs; i++)
	{
		AttrVal		av;

		if (!read_form(cu, ab->attrs[i].form, ab->attrs[i].implicit, &p, end, &av))
			return;
		if (ab->attrs[i].attr == DW_AT_name && av.sval && *name == NULL)
			*name = av.sval;
		else if (ab->attrs[i].attr == DW_AT_type && !*have_type)
		{
			*type_ref = av.uval;
			*have_type = true;
		}
		else if ((ab->attrs[i].attr == DW_AT_abstract_origin ||
				  ab->attrs[i].attr == DW_AT_specification) && !have_origin)
		{
			origin = av.uval;
			have_origin = true;
		}
	}

	if ((*name == NULL || !*have_type) && have_origin)
		die_get(cu, origin, name, type_ref, have_type, depth + 1);
}

/*
 * Produce a type name for the DIE at CU-relative offset `ref` (bytes from
 * cu_start).  Appends to *buf.  depth guards against cycles.
 */
static void
type_name(CU *cu, uint64_t ref, char **buf, size_t *len, size_t *cap, int depth)
{
	const uint8_t *p;
	const uint8_t *end = cu->cu_end;
	const Abbrev *ab;
	AttrVal		av;
	const char *name = NULL;
	uint64_t	type_ref = 0;
	bool		have_type = false;
	int			i;

	if (depth > 12 || ref == 0)
	{
		namestr_append(buf, len, cap, "void");
		return;
	}

	p = cu->cu_start + ref;
	if (p >= end)
	{
		namestr_append(buf, len, cap, "?");
		return;
	}
	ab = read_die_code(&p, end);
	if (ab == NULL)
	{
		namestr_append(buf, len, cap, "?");
		return;
	}

	for (i = 0; i < ab->nattrs; i++)
	{
		if (!read_form(cu, ab->attrs[i].form, ab->attrs[i].implicit, &p, end, &av))
			break;
		if (ab->attrs[i].attr == DW_AT_name && av.sval)
			name = av.sval;
		else if (ab->attrs[i].attr == DW_AT_type)
		{
			type_ref = av.uval;
			have_type = true;
		}
	}

	switch (ab->tag)
	{
		case DW_TAG_base_type:
		case DW_TAG_typedef:
			namestr_append(buf, len, cap, name ? name : "?");
			break;
		case DW_TAG_structure_type:
			namestr_append(buf, len, cap, "struct ");
			namestr_append(buf, len, cap, name ? name : "{...}");
			break;
		case DW_TAG_union_type:
			namestr_append(buf, len, cap, "union ");
			namestr_append(buf, len, cap, name ? name : "{...}");
			break;
		case DW_TAG_enumeration_type:
			namestr_append(buf, len, cap, "enum ");
			namestr_append(buf, len, cap, name ? name : "{...}");
			break;
		case DW_TAG_pointer_type:
			if (have_type)
				type_name(cu, type_ref, buf, len, cap, depth + 1);
			else
				namestr_append(buf, len, cap, "void");
			namestr_append(buf, len, cap, " *");
			break;
		case DW_TAG_const_type:
			namestr_append(buf, len, cap, "const ");
			if (have_type)
				type_name(cu, type_ref, buf, len, cap, depth + 1);
			else
				namestr_append(buf, len, cap, "void");
			break;
		case DW_TAG_volatile_type:
			namestr_append(buf, len, cap, "volatile ");
			if (have_type)
				type_name(cu, type_ref, buf, len, cap, depth + 1);
			else
				namestr_append(buf, len, cap, "void");
			break;
		case DW_TAG_restrict_type:
			if (have_type)
				type_name(cu, type_ref, buf, len, cap, depth + 1);
			else
				namestr_append(buf, len, cap, "void");
			break;
		case DW_TAG_array_type:
			if (have_type)
				type_name(cu, type_ref, buf, len, cap, depth + 1);
			namestr_append(buf, len, cap, "[]");
			break;
		default:
			namestr_append(buf, len, cap, name ? name : "void");
			break;
	}
}

static void
namestr_append(char **buf, size_t *len, size_t *cap, const char *s)
{
	size_t		sl = strlen(s);

	if (*len + sl + 1 > *cap)
	{
		while (*len + sl + 1 > *cap)
			*cap = *cap ? *cap * 2 : 128;
		*buf = realloc(*buf, *cap);
	}
	memcpy(*buf + *len, s, sl);
	*len += sl;
	(*buf)[*len] = '\0';
}

/*
 * Skip a DIE's attributes (used to advance past DIEs we don't care about).
 * Returns false on unsupported form.
 */
static bool
skip_attrs(CU *cu, const Abbrev *ab, const uint8_t **pp, const uint8_t *end)
{
	AttrVal		av;
	int			i;

	for (i = 0; i < ab->nattrs; i++)
		if (!read_form(cu, ab->attrs[i].form, ab->attrs[i].implicit, pp, end, &av))
			return false;
	return true;
}

/*
 * Raw signature of a function definition: the DIE references (CU-relative) of
 * its return type and each direct parameter type, plus the parameter names.
 * These refs are resolved later (by type_name for strings, type_classify for
 * FFI) using the same CU.
 */
typedef struct
{
	uint64_t	ret_ref;
	bool		have_ret;
	int			nparams;
	uint64_t	ptype[PGP_MAX_ARGS];
	bool		have_ptype[PGP_MAX_ARGS];
	const char *pname[PGP_MAX_ARGS];
}			RawSig;

/*
 * Walk one CU for the *definition* of `func` (the subprogram DIE that has
 * DW_AT_low_pc and children, not the many DW_AT_declaration copies), following
 * DW_AT_abstract_origin/DW_AT_specification for optimized code.  On success
 * fills *rs and returns true.
 */
static bool
collect_rawsig(CU *cu, const char *func, RawSig *rs)
{
	const uint8_t *p = cu->die_start;
	const uint8_t *end = cu->cu_end;
	int			depth = 0;

	while (p < end)
	{
		const Abbrev *ab = read_die_code(&p, end);
		int			i;

		if (ab == NULL)			/* null DIE: end of a child list */
		{
			if (depth == 0)
				break;
			depth--;
			continue;
		}

		if (ab->tag == DW_TAG_subprogram)
		{
			const char *name = NULL;
			uint64_t	ret_ref = 0;
			bool		have_ret = false;
			bool		have_low_pc = false;
			uint64_t	origin = 0;
			bool		have_origin = false;
			const uint8_t *after;

			for (i = 0; i < ab->nattrs; i++)
			{
				AttrVal		av;

				if (!read_form(cu, ab->attrs[i].form, ab->attrs[i].implicit,
							   &p, end, &av))
					return false;
				if (ab->attrs[i].attr == DW_AT_name && av.sval)
					name = av.sval;
				else if (ab->attrs[i].attr == DW_AT_type)
				{
					ret_ref = av.uval;
					have_ret = true;
				}
				else if (ab->attrs[i].attr == DW_AT_low_pc)
					have_low_pc = true;
				else if (ab->attrs[i].attr == DW_AT_abstract_origin ||
						 ab->attrs[i].attr == DW_AT_specification)
				{
					origin = av.uval;
					have_origin = true;
				}
			}
			after = p;

			/* Optimized definitions put name/return-type on an abstract DIE. */
			if ((name == NULL || !have_ret) && have_origin)
				die_get(cu, origin, &name, &ret_ref, &have_ret, 0);

			/* A definition has low_pc and children (its params/locals). */
			if (name && have_low_pc && ab->has_children &&
				strcmp(name, func) == 0)
			{
				const uint8_t *cp = after;
				int			cdepth = 0;

				rs->ret_ref = ret_ref;
				rs->have_ret = have_ret;
				rs->nparams = 0;

				while (cp < end)
				{
					const Abbrev *cab = read_die_code(&cp, end);

					if (cab == NULL)
					{
						if (cdepth == 0)
							break;
						cdepth--;
						continue;
					}
					if (cdepth == 0 && cab->tag == DW_TAG_formal_parameter)
					{
						const char *pname = NULL;
						uint64_t	ptype = 0;
						bool		have_pt = false;
						uint64_t	porigin = 0;
						bool		have_po = false;
						int			k;

						for (k = 0; k < cab->nattrs; k++)
						{
							AttrVal		av;

							if (!read_form(cu, cab->attrs[k].form,
										   cab->attrs[k].implicit, &cp, end, &av))
								break;
							if (cab->attrs[k].attr == DW_AT_name && av.sval)
								pname = av.sval;
							else if (cab->attrs[k].attr == DW_AT_type)
							{
								ptype = av.uval;
								have_pt = true;
							}
							else if (cab->attrs[k].attr == DW_AT_abstract_origin ||
									 cab->attrs[k].attr == DW_AT_specification)
							{
								porigin = av.uval;
								have_po = true;
							}
						}
						if ((pname == NULL || !have_pt) && have_po)
							die_get(cu, porigin, &pname, &ptype, &have_pt, 0);

						if (rs->nparams < PGP_MAX_ARGS)
						{
							int			ix = rs->nparams;

							rs->ptype[ix] = ptype;
							rs->have_ptype[ix] = have_pt;
							rs->pname[ix] = pname;
							rs->nparams++;
						}
						if (cab->has_children)
							cdepth++;	/* unlikely for a param */
					}
					else
					{
						/* Not a direct param: skip it, descend if needed. */
						if (!skip_attrs(cu, cab, &cp, end))
							break;
						if (cab->has_children)
							cdepth++;
						/* Params come first; the first non-param at depth 0
						 * ends the parameter list. */
						if (cdepth == 0)
							break;
					}
				}
				return true;
			}

			/* Not our definition: continue, descending if it has children. */
			if (ab->has_children)
				depth++;
			continue;
		}

		/* Non-subprogram DIE: skip its attrs, descend if it has children. */
		if (!skip_attrs(cu, ab, &p, end))
			return false;
		if (ab->has_children)
			depth++;
	}

	return false;
}

/*
 * Search one CU for the definition of `func`.  On success returns a malloc'd
 * signature string and sets *found = true.
 */
static char *
search_cu(CU *cu, const char *func, bool *found)
{
	RawSig		rs;
	char	   *buf = NULL;
	size_t		len = 0,
				cap = 0;
	int			i;

	if (!collect_rawsig(cu, func, &rs))
	{
		*found = false;
		return NULL;
	}

	if (rs.have_ret)
		type_name(cu, rs.ret_ref, &buf, &len, &cap, 0);
	else
		namestr_append(&buf, &len, &cap, "void");
	namestr_append(&buf, &len, &cap, " ");
	namestr_append(&buf, &len, &cap, func);
	namestr_append(&buf, &len, &cap, "(");

	for (i = 0; i < rs.nparams; i++)
	{
		if (i > 0)
			namestr_append(&buf, &len, &cap, ", ");
		if (rs.have_ptype[i])
			type_name(cu, rs.ptype[i], &buf, &len, &cap, 0);
		else
			namestr_append(&buf, &len, &cap, "void");
		if (rs.pname[i])
		{
			namestr_append(&buf, &len, &cap, " ");
			namestr_append(&buf, &len, &cap, rs.pname[i]);
		}
	}
	namestr_append(&buf, &len, &cap, ")");
	*found = true;
	return buf;
}

/* Auto-load the default dSYM if nothing is loaded yet. */
static bool
dwarf_ensure_loaded(char **err)
{
	char	   *def;
	bool		ok;

	if (D.loaded)
		return true;
	def = pgpatch_dwarf_default_path();
	ok = def && pgpatch_dwarf_set_path(def);
	free(def);
	if (!ok && err)
		*err = strdup("no DWARF loaded; generate a .dSYM (dsymutil) "
					  "or call (pgpatch/set-dwarf \"path\")");
	return ok;
}

/*
 * Parse the next CU header at *pp and fill *cu.  Always advances *pp to the
 * next CU.  Returns 1 if usable (DWARF5), 0 to skip, -1 to stop.
 */
static int
next_cu(const uint8_t **pp, const uint8_t *info_end, CU *cu)
{
	const uint8_t *p = *pp;
	const uint8_t *cu_start = p;
	uint32_t	unit_len = read_u32(p);
	const uint8_t *cu_end;
	uint16_t	version;
	uint64_t	abbr_off;
	const Abbrev *root;
	uint8_t		addr_size;
	int			i;

	if (unit_len == 0 || unit_len == 0xffffffff)
		return -1;				/* DWARF64 unsupported */
	cu_end = p + 4 + unit_len;
	if (cu_end > info_end)
		return -1;
	*pp = cu_end;				/* advance regardless of outcome */

	p += 4;
	memcpy(&version, p, 2);
	p += 2;
	if (version != 5)
		return 0;				/* only DWARF5 supported */
	p += 1;						/* unit_type */
	addr_size = *p++;
	abbr_off = read_u32(p);
	p += 4;

	cu->cu_start = cu_start;
	cu->cu_end = cu_end;
	cu->addr_size = addr_size;
	cu->str_offsets_base = 8;	/* DWARF5 default (after 8-byte header) */

	if (!parse_abbrev(abbr_off))
		return 0;

	/* Root DIE: read compile_unit to capture str_offsets_base. */
	{
		const uint8_t *rp = p;

		root = read_die_code(&rp, cu_end);
		if (root && root->tag == DW_TAG_compile_unit)
		{
			for (i = 0; i < root->nattrs; i++)
			{
				AttrVal		av;

				if (!read_form(cu, root->attrs[i].form,
							   root->attrs[i].implicit, &rp, cu_end, &av))
					break;
				if (root->attrs[i].attr == DW_AT_str_offsets_base)
					cu->str_offsets_base = av.uval;
			}
		}
	}
	cu->die_start = p;
	return 1;
}

/*
 * Classify the type DIE at CU-relative offset `ref` into a machine class,
 * resolving through typedef/const/volatile/restrict.
 */
static PgpFfiType
type_classify(CU *cu, uint64_t ref, int depth)
{
	const uint8_t *p;
	const uint8_t *end = cu->cu_end;
	const Abbrev *ab;
	uint64_t	type_ref = 0;
	uint64_t	enc = 0,
				bsz = 0;
	bool		have_type = false,
				have_enc = false;
	int			i;

	if (depth > 12 || ref == 0)
		return PGP_FT_S64;
	p = cu->cu_start + ref;
	if (p >= end)
		return PGP_FT_S64;
	ab = read_die_code(&p, end);
	if (ab == NULL)
		return PGP_FT_S64;

	for (i = 0; i < ab->nattrs; i++)
	{
		AttrVal		av;

		if (!read_form(cu, ab->attrs[i].form, ab->attrs[i].implicit, &p, end, &av))
			break;
		if (ab->attrs[i].attr == DW_AT_type)
		{
			type_ref = av.uval;
			have_type = true;
		}
		else if (ab->attrs[i].attr == DW_AT_encoding)
		{
			enc = av.uval;
			have_enc = true;
		}
		else if (ab->attrs[i].attr == DW_AT_byte_size)
			bsz = av.uval;
	}

	switch (ab->tag)
	{
		case DW_TAG_pointer_type:
			return PGP_FT_PTR;
		case DW_TAG_base_type:
			if (have_enc && enc == DW_ATE_float)
				return (bsz == 4) ? PGP_FT_FLOAT : PGP_FT_DOUBLE;
			if (have_enc && enc == DW_ATE_boolean)
				return PGP_FT_U8;
			{
				bool		sgn = have_enc && (enc == DW_ATE_signed ||
											   enc == DW_ATE_signed_char);

				switch (bsz)
				{
					case 1:
						return sgn ? PGP_FT_S8 : PGP_FT_U8;
					case 2:
						return sgn ? PGP_FT_S16 : PGP_FT_U16;
					case 4:
						return sgn ? PGP_FT_S32 : PGP_FT_U32;
					default:
						return sgn ? PGP_FT_S64 : PGP_FT_U64;
				}
			}
		case DW_TAG_enumeration_type:
			switch (bsz)
			{
				case 1:
					return PGP_FT_S8;
				case 2:
					return PGP_FT_S16;
				case 8:
					return PGP_FT_S64;
				default:
					return PGP_FT_S32;
			}
		case DW_TAG_typedef:
		case DW_TAG_const_type:
		case DW_TAG_volatile_type:
		case DW_TAG_restrict_type:
			if (have_type)
				return type_classify(cu, type_ref, depth + 1);
			return PGP_FT_S64;
		case DW_TAG_structure_type:
		case DW_TAG_union_type:
			return PGP_FT_STRUCT;
		default:
			return PGP_FT_S64;
	}
}

/*
 * Public: return a malloc'd signature string for `func`, or NULL if not found.
 */
char *
pgpatch_dwarf_signature(const char *func, char **err)
{
	const uint8_t *p;
	const uint8_t *info_end;

	if (err)
		*err = NULL;
	if (!dwarf_ensure_loaded(err))
		return NULL;

	p = D.info.p;
	info_end = D.info.p + D.info.len;

	while (p + 12 <= info_end)
	{
		CU			cu;
		int			r = next_cu(&p, info_end, &cu);
		bool		found = false;
		char	   *sig;

		if (r < 0)
			break;
		if (r == 0)
			continue;
		sig = search_cu(&cu, func, &found);
		if (found)
			return sig;
		free(sig);
	}

	if (err)
		*err = strdup("function definition with parameters not found in DWARF");
	return NULL;
}

/*
 * Public: fill *out with the FFI signature (return + parameter classes) of
 * `func` from DWARF.  Returns false if not found.
 */
bool
pgpatch_dwarf_ffi_sig(const char *func, PgpSig *out)
{
	const uint8_t *p;
	const uint8_t *info_end;

	if (!dwarf_ensure_loaded(NULL))
		return false;

	p = D.info.p;
	info_end = D.info.p + D.info.len;

	while (p + 12 <= info_end)
	{
		CU			cu;
		int			r = next_cu(&p, info_end, &cu);
		RawSig		rs;

		if (r < 0)
			break;
		if (r == 0)
			continue;
		if (collect_rawsig(&cu, func, &rs))
		{
			int			i;

			out->ret = rs.have_ret ? type_classify(&cu, rs.ret_ref, 0) : PGP_FT_VOID;
			out->nargs = rs.nparams;
			out->has_struct = (out->ret == PGP_FT_STRUCT);
			for (i = 0; i < rs.nparams; i++)
			{
				PgpFfiType	t = rs.have_ptype[i]
					? type_classify(&cu, rs.ptype[i], 0) : PGP_FT_S64;

				out->args[i] = t;
				if (t == PGP_FT_STRUCT)
					out->has_struct = true;
			}
			return true;
		}
	}
	return false;
}

/* ---- struct/union layout introspection ---- */

/* Is `form` a constant-encoded form (usable directly as a member offset)? */
static bool
is_const_form(uint64_t form)
{
	switch (form)
	{
		case DW_FORM_data1:
		case DW_FORM_data2:
		case DW_FORM_data4:
		case DW_FORM_data8:
		case DW_FORM_udata:
		case DW_FORM_sdata:
		case DW_FORM_implicit_const:
			return true;
		default:
			return false;
	}
}

/*
 * Follow typedef/const/volatile/restrict from the DIE at `ref` to a
 * structure/union type DIE; return its CU-relative offset, or 0.
 */
static uint64_t
resolve_to_aggregate(CU *cu, uint64_t ref, int depth)
{
	const uint8_t *p;
	const uint8_t *end = cu->cu_end;
	const Abbrev *ab;
	uint64_t	type_ref = 0;
	bool		have_type = false;
	int			i;

	if (depth > 12 || ref == 0)
		return 0;
	p = cu->cu_start + ref;
	if (p >= end)
		return 0;
	ab = read_die_code(&p, end);
	if (ab == NULL)
		return 0;

	for (i = 0; i < ab->nattrs; i++)
	{
		AttrVal		av;

		if (!read_form(cu, ab->attrs[i].form, ab->attrs[i].implicit, &p, end, &av))
			break;
		if (ab->attrs[i].attr == DW_AT_type)
		{
			type_ref = av.uval;
			have_type = true;
		}
	}

	if (ab->tag == DW_TAG_structure_type || ab->tag == DW_TAG_union_type)
		return ref;
	if ((ab->tag == DW_TAG_typedef || ab->tag == DW_TAG_const_type ||
		 ab->tag == DW_TAG_volatile_type || ab->tag == DW_TAG_restrict_type) &&
		have_type)
		return resolve_to_aggregate(cu, type_ref, depth + 1);
	return 0;
}

/*
 * Format the aggregate (struct/union) at CU-relative offset `agg_ref` into a
 * malloc'd string.  Returns NULL if it is a forward declaration (no byte_size).
 */
static char *
format_aggregate(CU *cu, uint64_t agg_ref, const char *dispname)
{
	const uint8_t *p = cu->cu_start + agg_ref;
	const uint8_t *end = cu->cu_end;
	const Abbrev *ab;
	uint64_t	bsz = 0;
	bool		have_bsz = false;
	const char *kind;
	const uint8_t *cp;
	int			cdepth = 0;
	char	   *buf = NULL;
	size_t		len = 0,
				cap = 0;
	char		line[64];
	int			i;

	ab = read_die_code(&p, end);
	if (ab == NULL)
		return NULL;
	kind = (ab->tag == DW_TAG_union_type) ? "union" : "struct";

	for (i = 0; i < ab->nattrs; i++)
	{
		AttrVal		av;

		if (!read_form(cu, ab->attrs[i].form, ab->attrs[i].implicit, &p, end, &av))
			return NULL;
		if (ab->attrs[i].attr == DW_AT_byte_size)
		{
			bsz = av.uval;
			have_bsz = true;
		}
	}
	if (!have_bsz)
		return NULL;			/* forward declaration, not a definition */
	cp = p;						/* children (members) start here */

	snprintf(line, sizeof(line), "%s %s {  /* %llu bytes */\n",
			 kind, dispname, (unsigned long long) bsz);
	namestr_append(&buf, &len, &cap, line);

	while (cp < end)
	{
		const Abbrev *mab = read_die_code(&cp, end);

		if (mab == NULL)
		{
			if (cdepth == 0)
				break;
			cdepth--;
			continue;
		}
		if (cdepth == 0 && mab->tag == DW_TAG_member)
		{
			const char *mname = NULL;
			uint64_t	mtype = 0;
			bool		have_mt = false;
			uint64_t	moff = 0;
			bool		have_off = false;
			int			k;

			for (k = 0; k < mab->nattrs; k++)
			{
				AttrVal		av;
				uint64_t	form = mab->attrs[k].form;

				if (!read_form(cu, form, mab->attrs[k].implicit, &cp, end, &av))
					break;
				if (mab->attrs[k].attr == DW_AT_name && av.sval)
					mname = av.sval;
				else if (mab->attrs[k].attr == DW_AT_type)
				{
					mtype = av.uval;
					have_mt = true;
				}
				else if (mab->attrs[k].attr == DW_AT_data_member_location &&
						 is_const_form(form))
				{
					moff = av.uval;
					have_off = true;
				}
			}

			if (have_off)
				snprintf(line, sizeof(line), "  +0x%04llx  ",
						 (unsigned long long) moff);
			else
				snprintf(line, sizeof(line), "  +?????  ");
			namestr_append(&buf, &len, &cap, line);
			if (have_mt)
				type_name(cu, mtype, &buf, &len, &cap, 0);
			else
				namestr_append(&buf, &len, &cap, "void");
			namestr_append(&buf, &len, &cap, " ");
			namestr_append(&buf, &len, &cap, mname ? mname : "/*anon*/");
			namestr_append(&buf, &len, &cap, ";\n");

			if (mab->has_children)
				cdepth++;
		}
		else
		{
			/* Nested type/anon aggregate: skip, descend so its members are
			 * not mistaken for ours. */
			if (!skip_attrs(cu, mab, &cp, end))
				break;
			if (mab->has_children)
				cdepth++;
		}
	}

	namestr_append(&buf, &len, &cap, "}");
	return buf;
}

/* Search one CU for a struct/union/typedef named `name`; format if found. */
/*
 * Locate the aggregate (struct/union, or a typedef of one) named `name` in this
 * CU.  On success sets *ref_out to its CU-relative DIE offset and returns true.
 */
static bool
find_aggregate_in_cu(CU *cu, const char *name, uint64_t *ref_out)
{
	const uint8_t *p = cu->die_start;
	const uint8_t *end = cu->cu_end;
	int			depth = 0;
	uint64_t	td_target = 0;
	bool		have_td = false;

	while (p < end)
	{
		const uint8_t *die_at = p;
		const Abbrev *ab = read_die_code(&p, end);
		uint64_t	ref;
		int			i;

		if (ab == NULL)
		{
			if (depth == 0)
				break;
			depth--;
			continue;
		}
		ref = (uint64_t) (die_at - cu->cu_start);

		if (ab->tag == DW_TAG_structure_type || ab->tag == DW_TAG_union_type ||
			ab->tag == DW_TAG_typedef)
		{
			const char *nm = NULL;
			uint64_t	type_ref = 0;
			bool		have_type = false;

			for (i = 0; i < ab->nattrs; i++)
			{
				AttrVal		av;

				if (!read_form(cu, ab->attrs[i].form, ab->attrs[i].implicit,
							   &p, end, &av))
					return false;
				if (ab->attrs[i].attr == DW_AT_name && av.sval)
					nm = av.sval;
				else if (ab->attrs[i].attr == DW_AT_type)
				{
					type_ref = av.uval;
					have_type = true;
				}
			}

			if (nm && strcmp(nm, name) == 0)
			{
				if (ab->tag == DW_TAG_typedef)
				{
					if (have_type)
					{
						td_target = type_ref;
						have_td = true;
					}
				}
				else if (ab->has_children)
				{
					*ref_out = ref;
					return true;
				}
			}
			if (ab->has_children)
				depth++;
			continue;
		}

		if (!skip_attrs(cu, ab, &p, end))
			return false;
		if (ab->has_children)
			depth++;
	}

	/* No direct tag match; try a typedef we saw. */
	if (have_td)
	{
		uint64_t	agg = resolve_to_aggregate(cu, td_target, 0);

		if (agg)
		{
			*ref_out = agg;
			return true;
		}
	}
	return false;
}

/*
 * Collect the members of the aggregate at `agg_ref` into *out (structured).
 * Returns false for a forward declaration (no byte_size).
 */
static bool
collect_members(CU *cu, uint64_t agg_ref, const char *dispname, PgpStruct *out)
{
	const uint8_t *p = cu->cu_start + agg_ref;
	const uint8_t *end = cu->cu_end;
	const Abbrev *ab;
	uint64_t	bsz = 0;
	bool		have_bsz = false;
	const char *kind;
	const uint8_t *cp;
	int			cdepth = 0;
	int			cap = 8;
	int			i;

	ab = read_die_code(&p, end);
	if (ab == NULL)
		return false;
	kind = (ab->tag == DW_TAG_union_type) ? "union" : "struct";

	for (i = 0; i < ab->nattrs; i++)
	{
		AttrVal		av;

		if (!read_form(cu, ab->attrs[i].form, ab->attrs[i].implicit, &p, end, &av))
			return false;
		if (ab->attrs[i].attr == DW_AT_byte_size)
		{
			bsz = av.uval;
			have_bsz = true;
		}
	}
	if (!have_bsz)
		return false;

	out->name = strdup(dispname);
	out->kind = strdup(kind);
	out->size = (unsigned long long) bsz;
	out->nfields = 0;
	out->fields = malloc((size_t) cap * sizeof(PgpField));
	cp = p;

	while (cp < end)
	{
		const Abbrev *mab = read_die_code(&cp, end);

		if (mab == NULL)
		{
			if (cdepth == 0)
				break;
			cdepth--;
			continue;
		}
		if (cdepth == 0 && mab->tag == DW_TAG_member)
		{
			const char *mname = NULL;
			uint64_t	mtype = 0;
			bool		have_mt = false;
			uint64_t	moff = 0;
			bool		have_off = false;
			char	   *tbuf = NULL;
			size_t		tl = 0,
						tc = 0;
			int			k;

			for (k = 0; k < mab->nattrs; k++)
			{
				AttrVal		av;
				uint64_t	form = mab->attrs[k].form;

				if (!read_form(cu, form, mab->attrs[k].implicit, &cp, end, &av))
					break;
				if (mab->attrs[k].attr == DW_AT_name && av.sval)
					mname = av.sval;
				else if (mab->attrs[k].attr == DW_AT_type)
				{
					mtype = av.uval;
					have_mt = true;
				}
				else if (mab->attrs[k].attr == DW_AT_data_member_location &&
						 is_const_form(form))
				{
					moff = av.uval;
					have_off = true;
				}
			}

			if (have_mt)
				type_name(cu, mtype, &tbuf, &tl, &tc, 0);
			else
				tbuf = strdup("void");

			if (out->nfields >= cap)
			{
				cap *= 2;
				out->fields = realloc(out->fields, (size_t) cap * sizeof(PgpField));
			}
			out->fields[out->nfields].offset = (unsigned long long) moff;
			out->fields[out->nfields].have_offset = have_off;
			out->fields[out->nfields].name = mname ? strdup(mname) : NULL;
			out->fields[out->nfields].type = tbuf ? tbuf : strdup("?");
			out->nfields++;

			if (mab->has_children)
				cdepth++;
		}
		else
		{
			if (!skip_attrs(cu, mab, &cp, end))
				break;
			if (mab->has_children)
				cdepth++;
		}
	}
	return true;
}

void
pgpatch_dwarf_free_struct(PgpStruct *s)
{
	int			i;

	if (s->fields)
	{
		for (i = 0; i < s->nfields; i++)
		{
			free(s->fields[i].name);
			free(s->fields[i].type);
		}
		free(s->fields);
	}
	free(s->name);
	free(s->kind);
	memset(s, 0, sizeof(*s));
}

/*
 * Public: return a malloc'd, human-readable layout of struct/union `name`, or
 * NULL if not found.
 */
char *
pgpatch_dwarf_struct(const char *name, char **err)
{
	const uint8_t *p;
	const uint8_t *info_end;

	if (err)
		*err = NULL;
	if (!dwarf_ensure_loaded(err))
		return NULL;

	p = D.info.p;
	info_end = D.info.p + D.info.len;

	while (p + 12 <= info_end)
	{
		CU			cu;
		int			r = next_cu(&p, info_end, &cu);
		uint64_t	ref;

		if (r < 0)
			break;
		if (r == 0)
			continue;
		if (find_aggregate_in_cu(&cu, name, &ref))
		{
			char	   *s = format_aggregate(&cu, ref, name);

			if (s)
				return s;
		}
	}

	if (err)
		*err = strdup("struct/union/typedef definition not found in DWARF");
	return NULL;
}

/*
 * Public: fill *out with the structured layout of struct/union `name`.
 */
bool
pgpatch_dwarf_struct_info(const char *name, PgpStruct *out, char **err)
{
	const uint8_t *p;
	const uint8_t *info_end;

	if (err)
		*err = NULL;
	memset(out, 0, sizeof(*out));
	if (!dwarf_ensure_loaded(err))
		return false;

	p = D.info.p;
	info_end = D.info.p + D.info.len;

	while (p + 12 <= info_end)
	{
		CU			cu;
		int			r = next_cu(&p, info_end, &cu);
		uint64_t	ref;

		if (r < 0)
			break;
		if (r == 0)
			continue;
		if (find_aggregate_in_cu(&cu, name, &ref) &&
			collect_members(&cu, ref, name, out))
			return true;
	}

	if (err)
		*err = strdup("struct/union/typedef definition not found in DWARF");
	return false;
}
