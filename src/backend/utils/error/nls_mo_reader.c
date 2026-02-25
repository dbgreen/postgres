/*-------------------------------------------------------------------------
 *
 * nls_mo_reader.c
 *	  Direct .mo file reader for NLS translation lookup on Windows.
 *
 * On Windows, every dgettext() call in gettext 0.20+ performs locale
 * resolution via EnumSystemLocales() (~259 locales), costing ~12
 * microseconds per call even with all locking removed.  For workloads
 * that raise millions of exceptions (PL/pgSQL EXCEPTION blocks), this
 * adds tens of seconds of overhead.
 *
 * We eliminate this by reading .mo files directly at locale-change time
 * and performing hash lookups ourselves using each .mo file's built-in
 * hash table.  Translations are pre-converted from UTF-8 to the message
 * encoding at load time, so lookups in the hot path are just a hash
 * computation + strcmp with zero per-call overhead.
 *
 * Multiple text domains are supported (postgres, plpgsql, plperl,
 * plpython, pltcl).  All domains are loaded eagerly --- at locale-
 * change time and when new domains are registered via pg_bindtextdomain.
 * This means the hot path (nls_domain_lookup) does zero allocation,
 * which is important because it runs during error reporting.
 *
 * This file is compiled only on Windows (see meson.build).
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/error/nls_mo_reader.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#ifdef ENABLE_NLS

#include <inttypes.h>

#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/elog.h"
#include "utils/memutils.h"

/*
 * Per-domain .mo file representation.
 *
 * Each loaded text domain (e.g. "postgres-19", "plpgsql-19") gets one
 * of these entries.  The postgres domain is always at index 0.
 */
typedef struct NlsDomain
{
	const char	   *name;			/* domain name, e.g. "plpgsql-19" */
	char		   *mo_data;		/* raw .mo file contents */
	uint32			nstrings;		/* number of string pairs */
	const uint32   *orig_tab;		/* original string descriptors */
	const uint32   *trans_tab;		/* translated string descriptors */
	const uint32   *hash_tab;		/* hash table */
	uint32			hash_size;		/* hash table size */
	bool			must_swap;		/* endianness swap needed */
	char		  **conv_tab;		/* pre-converted translations (non-UTF-8) */

	/* Expanded system-dependent strings (format-specifier variants) */
	int				n_sysdep;		/* number of expanded sysdep string pairs */
	char		  **sysdep_msgids;	/* expanded original strings */
	char		  **sysdep_msgstrs;	/* expanded translated strings */
	char		  **sysdep_conv;	/* encoding-converted translations (or NULL) */
} NlsDomain;

#define NLS_MAX_DOMAINS 8

static NlsDomain	nls_domains[NLS_MAX_DOMAINS];
static int			nls_num_domains = 0;

/* Exported to elog.c --- true when .mo translations are loaded and usable */
bool				nls_available = false;

/* Cached locale info for loading additional domains */
static char			nls_lang[128] = "";
static char			nls_locale_path[MAXPGPATH] = "";
static const char  *nls_target_enc = NULL;	/* NULL = no conversion (UTF-8/SQL_ASCII) */

/*
 * All NLS domain data (mo_data, conv_tab, sysdep arrays) is allocated
 * in NlsMemoryContext, a child of TopMemoryContext.  On locale change
 * we MemoryContextReset() it, freeing everything in one shot.
 */
static MemoryContext NlsMemoryContext = NULL;

/*
 * Registry of text domain names for eager loading.  Entries are string
 * constants from PG_TEXTDOMAIN() macros, so they persist for the process
 * lifetime and don't need to be allocated or freed.
 */
static const char  *nls_registered_domains[NLS_MAX_DOMAINS];
static int			nls_num_registered = 0;

/*
 * iconv function pointers, resolved at runtime from libiconv-2.dll
 * (which is already loaded as a dependency of libintl-8.dll).
 */
typedef void *iconv_t;
typedef iconv_t (*iconv_open_func)(const char *, const char *);
typedef size_t (*iconv_func)(iconv_t, const char **, size_t *, char **, size_t *);
typedef int (*iconv_close_func)(iconv_t);

static iconv_open_func		p_iconv_open = NULL;
static iconv_func			p_iconv = NULL;
static iconv_close_func		p_iconv_close = NULL;
static bool					nls_iconv_loaded = false;

/* Byte-swap for .mo files with non-native endianness */
static inline uint32
nls_swap(uint32 i)
{
	return (i << 24) | ((i & 0xff00) << 8) | ((i >> 8) & 0xff00) | (i >> 24);
}

#define NLS_DW(dom, x) ((dom)->must_swap ? nls_swap(x) : (x))

/*
 * nls_hash_string --- PJW hash, exact copy of gettext's __hash_string()
 * so our hash values match the .mo file's pre-built hash table.
 */
static uint32
nls_hash_string(const char *str)
{
	uint32		hval = 0;
	uint32		g;

	while (*str != '\0')
	{
		hval <<= 4;
		hval += (unsigned char) *str++;
		g = hval & 0xf0000000U;
		if (g != 0)
		{
			hval ^= g >> 24;
			hval ^= g;
		}
	}
	return hval;
}

/*
 * nls_domain_lookup --- look up a message in a specific domain's .mo
 * hash table.  Returns the translated string, or msgid itself if not found.
 */
static const char *
nls_domain_lookup(NlsDomain *dom, const char *msgid)
{
	uint32		hash_val;
	uint32		idx;
	uint32		incr;
	uint32		nstr;

	if (dom->hash_tab == NULL || dom->hash_size < 3)
		return msgid;

	hash_val = nls_hash_string(msgid);
	idx = hash_val % dom->hash_size;
	incr = 1 + (hash_val % (dom->hash_size - 2));

	for (;;)
	{
		nstr = NLS_DW(dom, dom->hash_tab[idx]);

		if (nstr == 0)
			break;					/* not in hash table */

		nstr--;						/* entries are 1-based */

		if (nstr < dom->nstrings)
		{
			/* Compare msgid with original string at this index */
			uint32	orig_off = NLS_DW(dom, dom->orig_tab[nstr * 2 + 1]);

			if (strcmp(msgid, dom->mo_data + orig_off) == 0)
			{
				/* Found --- return pre-converted or raw translation */
				if (dom->conv_tab && dom->conv_tab[nstr])
					return dom->conv_tab[nstr];

				return dom->mo_data + NLS_DW(dom, dom->trans_tab[nstr * 2 + 1]);
			}
		}

		/* Open-address probe: double hashing */
		if (idx >= dom->hash_size - incr)
			idx -= dom->hash_size - incr;
		else
			idx += incr;
	}

	/*
	 * Not found in the regular hash table.  Search the expanded sysdep
	 * strings, which contain platform-specific format specifier variants
	 * (e.g. %lld for PRIdPTR on Windows LLP64).
	 */
	for (int i = 0; i < dom->n_sysdep; i++)
	{
		if (strcmp(msgid, dom->sysdep_msgids[i]) == 0)
		{
			if (dom->sysdep_conv && dom->sysdep_conv[i])
				return dom->sysdep_conv[i];
			return dom->sysdep_msgstrs[i];
		}
	}

	return msgid;
}

/*
 * nls_lookup --- look up a message in the primary (postgres) domain.
 * Used by err_gettext() which only translates elog.c's own strings.
 */
const char *
nls_lookup(const char *msgid)
{
	if (nls_num_domains == 0)
		return msgid;

	return nls_domain_lookup(&nls_domains[0], msgid);
}

/* Forward declarations for functions defined below */
static void nls_free_domain(NlsDomain *dom);
static void nls_unload_all(void);
static void nls_load_iconv(void);
static void nls_convert_domain(NlsDomain *dom, const char *target_enc);
static bool nls_load_domain(NlsDomain *dom, const char *path);

/*
 * nls_lookup_domain --- look up a message in a named text domain.
 *
 * All domains are loaded eagerly (at locale-change time or when registered
 * via pg_bindtextdomain), so this function does zero allocation.  If the
 * domain isn't loaded yet, we just return msgid untranslated.
 *
 * NB: this runs during error reporting, so we must not use elog() here.
 */
const char *
nls_lookup_domain(const char *domain, const char *msgid)
{
	int			i;

	for (i = 0; i < nls_num_domains; i++)
	{
		if (strcmp(domain, nls_domains[i].name) == 0)
			return nls_domain_lookup(&nls_domains[i], msgid);
	}

	return msgid;					/* domain not loaded */
}

/*
 * nls_free_domain --- reset a single domain struct.
 *
 * The actual memory is freed by MemoryContextReset(NlsMemoryContext)
 * in nls_unload_all(), so we just zero the struct here.
 */
static void
nls_free_domain(NlsDomain *dom)
{
	memset(dom, 0, sizeof(NlsDomain));
}

/*
 * nls_unload_all --- free all loaded .mo file data and reset state.
 *
 * All domain data lives in NlsMemoryContext, so a single context reset
 * frees everything.  The registered domain list is preserved (it holds
 * pointers to string constants, not allocated memory).
 */
static void
nls_unload_all(void)
{
	int			i;

	for (i = 0; i < nls_num_domains; i++)
		nls_free_domain(&nls_domains[i]);

	nls_num_domains = 0;
	nls_lang[0] = '\0';
	nls_locale_path[0] = '\0';
	nls_target_enc = NULL;
	nls_available = false;

	if (NlsMemoryContext)
		MemoryContextReset(NlsMemoryContext);
}

/*
 * nls_load_iconv --- load iconv function pointers from libiconv-2.dll.
 *
 * libiconv-2.dll is already loaded as a dependency of libintl-8.dll
 * (the gettext runtime), so GetModuleHandle will find it without
 * needing LoadLibrary.  This avoids any link-time dependency on iconv.
 */
static void
nls_load_iconv(void)
{
	HMODULE		hmod;

	if (nls_iconv_loaded)
		return;

	nls_iconv_loaded = true;		/* don't retry on failure */

	hmod = GetModuleHandle("libiconv-2.dll");
	if (hmod == NULL)
	{
		elog(DEBUG1, "NLS: libiconv-2.dll not found, encoding conversion disabled");
		return;
	}

	p_iconv_open = (iconv_open_func) GetProcAddress(hmod, "libiconv_open");
	p_iconv = (iconv_func) GetProcAddress(hmod, "libiconv");
	p_iconv_close = (iconv_close_func) GetProcAddress(hmod, "libiconv_close");

	if (p_iconv_open == NULL || p_iconv == NULL || p_iconv_close == NULL)
	{
		elog(DEBUG1, "NLS: could not resolve iconv functions from libiconv-2.dll");
		p_iconv_open = NULL;
		p_iconv = NULL;
		p_iconv_close = NULL;
	}
}

/*
 * nls_convert_domain --- pre-convert all translations in a domain from
 * UTF-8 to the target encoding.
 *
 * Called at load time when the message encoding is not UTF-8.  Uses iconv
 * (from libiconv-2.dll, resolved at runtime) so that all encodings
 * supported by gettext are handled identically.
 */
static void
nls_convert_domain(NlsDomain *dom, const char *target_enc)
{
	char		translit_enc[64];
	iconv_t		cd;
	uint32		i;

	if (p_iconv_open == NULL)
		return;

	/*
	 * Use //TRANSLIT to transliterate characters that don't exist in the
	 * target encoding (e.g. French guillemets in EUC-JP).  This matches
	 * gettext's behavior with bind_textdomain_codeset().  If //TRANSLIT
	 * isn't supported, fall back to the plain encoding name.
	 */
	snprintf(translit_enc, sizeof(translit_enc), "%s//TRANSLIT", target_enc);
	cd = p_iconv_open(translit_enc, "UTF-8");
	if (cd == (iconv_t) -1)
		cd = p_iconv_open(target_enc, "UTF-8");
	if (cd == (iconv_t) -1)
	{
		elog(DEBUG1, "NLS: iconv_open failed for \"%s\" from UTF-8", target_enc);
		return;
	}

	dom->conv_tab = (char **) MemoryContextAllocZero(NlsMemoryContext,
												  dom->nstrings * sizeof(char *));

	for (i = 0; i < dom->nstrings; i++)
	{
		uint32		src_len = NLS_DW(dom, dom->trans_tab[i * 2]);
		uint32		src_off = NLS_DW(dom, dom->trans_tab[i * 2 + 1]);
		const char *src = dom->mo_data + src_off;
		size_t		inleft;
		size_t		outleft;
		size_t		dst_alloc;
		const char *inptr;
		char	   *outptr;
		char	   *dst;
		size_t		rc;

		if (src_len == 0)
			continue;

		/*
		 * Allocate output buffer.  For most encodings the output is the
		 * same size or smaller than UTF-8, but CJK encodings can be
		 * larger.  4x is safe for any encoding.
		 */
		dst_alloc = src_len * 4 + 1;
		dst = (char *) MemoryContextAlloc(NlsMemoryContext, dst_alloc);

		inptr = src;
		inleft = src_len;
		outptr = dst;
		outleft = dst_alloc - 1;

		rc = p_iconv(cd, &inptr, &inleft, &outptr, &outleft);
		if (rc == (size_t) -1 || inleft > 0)
		{
			/*
			 * Conversion failed for this string --- skip it.  The lookup
			 * will return the raw UTF-8 from the .mo file, matching
			 * gettext's fallback behavior.
			 */
			pfree(dst);
			continue;
		}

		*outptr = '\0';
		dom->conv_tab[i] = dst;

		/* Reset iconv state for the next string */
		p_iconv(cd, NULL, NULL, NULL, NULL);
	}

	/* Convert system-dependent translated strings */
	if (dom->n_sysdep > 0)
	{
		dom->sysdep_conv = (char **) MemoryContextAllocZero(NlsMemoryContext,
													 dom->n_sysdep * sizeof(char *));
		for (i = 0; i < (uint32) dom->n_sysdep; i++)
		{
			const char *src = dom->sysdep_msgstrs[i];
			size_t		src_len;
			size_t		dst_alloc;
			char	   *dst;
			size_t		inleft;
			size_t		outleft;
			const char *inptr;
			char	   *outptr;
			size_t		rc;

			if (src == NULL)
				continue;

			src_len = strlen(src);
			if (src_len == 0)
				continue;

			dst_alloc = src_len * 4 + 1;
			dst = (char *) MemoryContextAlloc(NlsMemoryContext, dst_alloc);

			inptr = src;
			inleft = src_len;
			outptr = dst;
			outleft = dst_alloc - 1;

			rc = p_iconv(cd, &inptr, &inleft, &outptr, &outleft);
			if (rc == (size_t) -1 || inleft > 0)
			{
				pfree(dst);
				continue;
			}

			*outptr = '\0';
			dom->sysdep_conv[i] = dst;

			p_iconv(cd, NULL, NULL, NULL, NULL);
		}
	}

	p_iconv_close(cd);
}

/*
 * nls_get_sysdep_segment_value --- return the platform-specific string
 * for a named system-dependent format specifier macro (e.g. "PRIdPTR").
 *
 * The .mo file's sysdep section references <inttypes.h> macro names;
 * this function resolves them to the actual format specifier string
 * for the current platform (e.g. PRIdPTR -> "lld" on MSVC x64).
 *
 * Returns NULL if the name is not recognized, causing any sysdep string
 * that references it to be skipped (matching gettext behavior).
 */
static const char *
nls_get_sysdep_segment_value(const char *name)
{
	/* 8-bit */
#ifdef PRId8
	if (strcmp(name, "PRId8") == 0) return PRId8;
#endif
#ifdef PRIi8
	if (strcmp(name, "PRIi8") == 0) return PRIi8;
#endif
#ifdef PRIo8
	if (strcmp(name, "PRIo8") == 0) return PRIo8;
#endif
#ifdef PRIu8
	if (strcmp(name, "PRIu8") == 0) return PRIu8;
#endif
#ifdef PRIx8
	if (strcmp(name, "PRIx8") == 0) return PRIx8;
#endif
#ifdef PRIX8
	if (strcmp(name, "PRIX8") == 0) return PRIX8;
#endif

	/* 16-bit */
#ifdef PRId16
	if (strcmp(name, "PRId16") == 0) return PRId16;
#endif
#ifdef PRIi16
	if (strcmp(name, "PRIi16") == 0) return PRIi16;
#endif
#ifdef PRIo16
	if (strcmp(name, "PRIo16") == 0) return PRIo16;
#endif
#ifdef PRIu16
	if (strcmp(name, "PRIu16") == 0) return PRIu16;
#endif
#ifdef PRIx16
	if (strcmp(name, "PRIx16") == 0) return PRIx16;
#endif
#ifdef PRIX16
	if (strcmp(name, "PRIX16") == 0) return PRIX16;
#endif

	/* 32-bit */
#ifdef PRId32
	if (strcmp(name, "PRId32") == 0) return PRId32;
#endif
#ifdef PRIi32
	if (strcmp(name, "PRIi32") == 0) return PRIi32;
#endif
#ifdef PRIo32
	if (strcmp(name, "PRIo32") == 0) return PRIo32;
#endif
#ifdef PRIu32
	if (strcmp(name, "PRIu32") == 0) return PRIu32;
#endif
#ifdef PRIx32
	if (strcmp(name, "PRIx32") == 0) return PRIx32;
#endif
#ifdef PRIX32
	if (strcmp(name, "PRIX32") == 0) return PRIX32;
#endif

	/* 64-bit */
#ifdef PRId64
	if (strcmp(name, "PRId64") == 0) return PRId64;
#endif
#ifdef PRIi64
	if (strcmp(name, "PRIi64") == 0) return PRIi64;
#endif
#ifdef PRIo64
	if (strcmp(name, "PRIo64") == 0) return PRIo64;
#endif
#ifdef PRIu64
	if (strcmp(name, "PRIu64") == 0) return PRIu64;
#endif
#ifdef PRIx64
	if (strcmp(name, "PRIx64") == 0) return PRIx64;
#endif
#ifdef PRIX64
	if (strcmp(name, "PRIX64") == 0) return PRIX64;
#endif

	/* intmax_t */
#ifdef PRIdMAX
	if (strcmp(name, "PRIdMAX") == 0) return PRIdMAX;
#endif
#ifdef PRIiMAX
	if (strcmp(name, "PRIiMAX") == 0) return PRIiMAX;
#endif
#ifdef PRIoMAX
	if (strcmp(name, "PRIoMAX") == 0) return PRIoMAX;
#endif
#ifdef PRIuMAX
	if (strcmp(name, "PRIuMAX") == 0) return PRIuMAX;
#endif
#ifdef PRIxMAX
	if (strcmp(name, "PRIxMAX") == 0) return PRIxMAX;
#endif
#ifdef PRIXMAX
	if (strcmp(name, "PRIXMAX") == 0) return PRIXMAX;
#endif

	/* intptr_t / uintptr_t */
#ifdef PRIdPTR
	if (strcmp(name, "PRIdPTR") == 0) return PRIdPTR;
#endif
#ifdef PRIiPTR
	if (strcmp(name, "PRIiPTR") == 0) return PRIiPTR;
#endif
#ifdef PRIoPTR
	if (strcmp(name, "PRIoPTR") == 0) return PRIoPTR;
#endif
#ifdef PRIuPTR
	if (strcmp(name, "PRIuPTR") == 0) return PRIuPTR;
#endif
#ifdef PRIxPTR
	if (strcmp(name, "PRIxPTR") == 0) return PRIxPTR;
#endif
#ifdef PRIXPTR
	if (strcmp(name, "PRIXPTR") == 0) return PRIXPTR;
#endif

	return NULL;
}

/*
 * nls_expand_sysdep_string --- expand a system-dependent string template
 * from the .mo file into a fully-resolved string.
 *
 * The sysdep string descriptor (struct sysdep_string_desc in glibc) is:
 *   uint32 offset          -- file offset to packed static segment data
 *   { uint32 segsize,      -- byte length of this static segment
 *     uint32 sysdepref     -- segment table index, or 0xFFFFFFFF = end
 *   } segments[]
 *
 * Static segments are packed sequentially starting at 'offset' in the
 * .mo data.  For each {segsize, sysdepref} pair we copy segsize bytes
 * of static text, then (unless sysdepref == SEGMENTS_END) append the
 * platform-specific expansion of the referenced macro.  The final pair
 * always has sysdepref == 0xFFFFFFFF; its segsize gives the length of
 * the trailing static text (typically just the NUL terminator).
 *
 * Returns a palloc'd string (in NlsMemoryContext), or NULL if expansion
 * failed because of an unknown segment reference.
 */
static char *
nls_expand_sysdep_string(NlsDomain *dom, const uint32 *desc,
						 uint32 n_sysdep_segments,
						 const char **seg_values)
{
	const uint32   *p;
	const char	   *sp;			/* walks through packed static segments */
	size_t			total_len;
	char		   *result;
	char		   *out;

	/* First pass: compute total expanded length */
	p = desc;
	sp = dom->mo_data + NLS_DW(dom, *p++);
	total_len = 0;

	for (;;)
	{
		uint32		segsize = NLS_DW(dom, *p++);
		uint32		sysdepref = NLS_DW(dom, *p++);

		total_len += segsize;

		if (sysdepref == 0xFFFFFFFF)	/* SEGMENTS_END */
			break;

		if (sysdepref >= n_sysdep_segments || seg_values[sysdepref] == NULL)
			return NULL;			/* unknown segment, skip this string */

		total_len += strlen(seg_values[sysdepref]);
	}

	/* Second pass: build the expanded string */
	result = (char *) MemoryContextAlloc(NlsMemoryContext, total_len + 1);

	out = result;
	p = desc;
	sp = dom->mo_data + NLS_DW(dom, *p++);

	for (;;)
	{
		uint32		segsize = NLS_DW(dom, *p++);
		uint32		sysdepref = NLS_DW(dom, *p++);

		memcpy(out, sp, segsize);
		out += segsize;
		sp += segsize;

		if (sysdepref == 0xFFFFFFFF)	/* SEGMENTS_END */
			break;

		{
			const char *val = seg_values[sysdepref];
			size_t		vlen = strlen(val);

			memcpy(out, val, vlen);
			out += vlen;
		}
	}

	*out = '\0';
	return result;
}

/*
 * nls_load_sysdep --- parse and expand system-dependent strings from
 * a .mo file's revision 0.1 extended header.
 *
 * System-dependent strings contain references to <inttypes.h> format
 * specifier macros (PRId64, PRIdPTR, etc.) that vary across platforms.
 * On Windows LLP64, PRIdPTR expands to "lld" while on LP64 Unix it's
 * "ld".  The .mo file stores templates with these macro references;
 * we expand them here using the current platform's actual values.
 *
 * The expanded msgid/msgstr pairs are stored in dom->sysdep_msgids[]
 * and dom->sysdep_msgstrs[], searched by linear scan in
 * nls_domain_lookup() after a hash table miss.
 */
static void
nls_load_sysdep(NlsDomain *dom, long fsize)
{
	uint32		   *hdr = (uint32 *) dom->mo_data;
	uint32			revision;
	uint32			minor;
	uint32			n_sysdep_segments;
	uint32			sysdep_segments_off;
	uint32			n_sysdep_strings;
	uint32			sysdep_orig_off;
	uint32			sysdep_trans_off;
	const uint32   *seg_tab;
	const uint32   *orig_tab;
	const uint32   *trans_tab;
	const char	  **seg_values = NULL;
	uint32			i;

	/* Check for revision 0.1+ extended header */
	revision = NLS_DW(dom, hdr[1]);
	minor = revision & 0xFFFF;
	if (minor < 1 || fsize < 48)
		return;						/* no sysdep section */

	n_sysdep_segments = NLS_DW(dom, hdr[7]);	/* offset 28 */
	sysdep_segments_off = NLS_DW(dom, hdr[8]);	/* offset 32 */
	n_sysdep_strings = NLS_DW(dom, hdr[9]);		/* offset 36 */
	sysdep_orig_off = NLS_DW(dom, hdr[10]);		/* offset 40 */
	sysdep_trans_off = NLS_DW(dom, hdr[11]);	/* offset 44 */

	if (n_sysdep_strings == 0 || n_sysdep_segments == 0)
		return;

	/* Build segment-index-to-platform-value mapping (temporary) */
	seg_values = (const char **) palloc0(n_sysdep_segments * sizeof(char *));

	seg_tab = (const uint32 *) (dom->mo_data + sysdep_segments_off);
	for (i = 0; i < n_sysdep_segments; i++)
	{
		uint32		seg_name_off = NLS_DW(dom, seg_tab[i * 2 + 1]);
		const char *seg_name = dom->mo_data + seg_name_off;

		seg_values[i] = nls_get_sysdep_segment_value(seg_name);
		/* NULL means unrecognized; strings referencing it will be skipped */
	}

	/* Allocate arrays for expanded string pairs */
	dom->sysdep_msgids = (char **) MemoryContextAllocZero(NlsMemoryContext,
														  n_sysdep_strings * sizeof(char *));
	dom->sysdep_msgstrs = (char **) MemoryContextAllocZero(NlsMemoryContext,
														   n_sysdep_strings * sizeof(char *));

	/* Expand each sysdep string pair (original + translated) */
	orig_tab = (const uint32 *) (dom->mo_data + sysdep_orig_off);
	trans_tab = (const uint32 *) (dom->mo_data + sysdep_trans_off);

	for (i = 0; i < n_sysdep_strings; i++)
	{
		uint32		orig_desc_off = NLS_DW(dom, orig_tab[i]);
		uint32		trans_desc_off = NLS_DW(dom, trans_tab[i]);
		const uint32 *orig_desc;
		const uint32 *trans_desc;
		char	   *expanded_orig;
		char	   *expanded_trans;

		orig_desc = (const uint32 *) (dom->mo_data + orig_desc_off);
		trans_desc = (const uint32 *) (dom->mo_data + trans_desc_off);

		expanded_orig = nls_expand_sysdep_string(dom, orig_desc,
												 n_sysdep_segments,
												 seg_values);
		if (expanded_orig == NULL)
			continue;				/* skip if expansion failed */

		expanded_trans = nls_expand_sysdep_string(dom, trans_desc,
												  n_sysdep_segments,
												  seg_values);
		if (expanded_trans == NULL)
		{
			pfree(expanded_orig);
			continue;
		}

		dom->sysdep_msgids[dom->n_sysdep] = expanded_orig;
		dom->sysdep_msgstrs[dom->n_sysdep] = expanded_trans;
		dom->n_sysdep++;
	}

	pfree(seg_values);
}

/*
 * nls_load_domain --- load a .mo file into a domain struct and set up
 * pointers for direct hash lookup.
 *
 * Returns true if a valid .mo file was loaded.  All allocations go into
 * NlsMemoryContext.
 */
static bool
nls_load_domain(NlsDomain *dom, const char *path)
{
	FILE	   *fp;
	long		fsize;
	uint32		magic;
	uint32	   *hdr;

	fp = fopen(path, "rb");
	if (fp == NULL)
		return false;

	fseek(fp, 0, SEEK_END);
	fsize = ftell(fp);
	if (fsize < 28)					/* minimum .mo header size */
	{
		fclose(fp);
		return false;
	}
	fseek(fp, 0, SEEK_SET);

	dom->mo_data = (char *) MemoryContextAlloc(NlsMemoryContext, fsize);

	if (fread(dom->mo_data, 1, fsize, fp) != (size_t) fsize)
	{
		pfree(dom->mo_data);
		dom->mo_data = NULL;
		fclose(fp);
		return false;
	}
	fclose(fp);

	/* Check magic number and determine endianness */
	memcpy(&magic, dom->mo_data, sizeof(uint32));
	if (magic == 0x950412de)
		dom->must_swap = false;
	else if (magic == 0xde120495)
		dom->must_swap = true;
	else
	{
		pfree(dom->mo_data);
		dom->mo_data = NULL;
		return false;
	}

	/* Parse header: offsets are at fixed positions in the .mo header */
	hdr = (uint32 *) dom->mo_data;
	dom->nstrings  = NLS_DW(dom, hdr[2]);		/* offset 8: number of strings */
	dom->orig_tab  = (const uint32 *) (dom->mo_data + NLS_DW(dom, hdr[3]));
	dom->trans_tab = (const uint32 *) (dom->mo_data + NLS_DW(dom, hdr[4]));
	dom->hash_size = NLS_DW(dom, hdr[5]);		/* offset 20: hash table size */
	dom->hash_tab  = (const uint32 *) (dom->mo_data + NLS_DW(dom, hdr[6]));

	/* Validate that the .mo file has a usable hash table */
	if (dom->hash_size <= 2 || dom->nstrings == 0)
	{
		pfree(dom->mo_data);
		dom->mo_data = NULL;
		return false;
	}

	/* Parse system-dependent strings (revision 0.1+ format) */
	nls_load_sysdep(dom, fsize);

	return true;
}

/*
 * nls_probe_locale --- probe whether the current locale has a message
 * catalog, and if so, load .mo files directly for fast lookups.
 *
 * We use dgettext() once to probe whether gettext can find a catalog.
 * If it can, we parse the "Language:" field from the .mo metadata to
 * determine the locale directory name, then read .mo files ourselves
 * for the postgres domain and all other registered text domains.
 *
 * If the message encoding is not UTF-8, we also pre-convert all
 * translations from UTF-8 (the .mo file encoding) to the message
 * encoding at this time.
 */
void
nls_probe_locale(void)
{
	const char *metadata;
	const char *langp;
	char		mo_path[MAXPGPATH];
	int			i;
	int			msg_enc;
	NlsDomain  *dom;

	/* Reset any previously loaded catalogs */
	nls_unload_all();

	/* Create memory context on first use */
	if (NlsMemoryContext == NULL)
		NlsMemoryContext = AllocSetContextCreate(TopMemoryContext,
												 "NlsMemoryContext",
												 ALLOCSET_DEFAULT_SIZES);

	/*
	 * Probe gettext to see if a catalog is available.  dgettext() with the
	 * empty msgid returns the .mo file's metadata header if a catalog is
	 * loaded, or the empty string "" if no catalog exists.
	 */
	metadata = dgettext(PG_TEXTDOMAIN("postgres"), "");
	if (metadata == NULL || metadata[0] == '\0')
	{
		elog(DEBUG3, "NLS: no message catalog found for current locale");
		return;					/* no catalog for this locale */
	}

	/*
	 * Parse "Language: xx" from the metadata to get the locale directory
	 * name (e.g., "de", "fr", "pt_BR").
	 */
	langp = strstr(metadata, "Language:");
	if (langp == NULL)
		langp = strstr(metadata, "language:");
	if (langp == NULL)
	{
		elog(DEBUG3, "NLS: no Language field in .mo metadata");
		return;					/* can't determine language */
	}

	langp += 9;					/* skip "Language:" */
	while (*langp == ' ')
		langp++;

	for (i = 0; i < (int) sizeof(nls_lang) - 1 && langp[i] != '\0' &&
		 langp[i] != '\n' && langp[i] != '\r'; i++)
		nls_lang[i] = langp[i];
	nls_lang[i] = '\0';

	if (nls_lang[0] == '\0')
		return;

	/* Cache the locale path for loading other domains */
	get_locale_path(my_exec_path, nls_locale_path);

	/*
	 * Ensure the postgres domain is registered.  It's always needed and
	 * is always loaded first (at index 0).
	 */
	{
		bool		found = false;

		for (i = 0; i < nls_num_registered; i++)
		{
			if (strcmp(nls_registered_domains[i], PG_TEXTDOMAIN("postgres")) == 0)
			{
				found = true;
				break;
			}
		}
		if (!found && nls_num_registered < NLS_MAX_DOMAINS)
			nls_registered_domains[nls_num_registered++] = PG_TEXTDOMAIN("postgres");
	}

	/*
	 * Load all registered domains eagerly.  This ensures that the hot
	 * path (nls_domain_lookup via nls_lookup_domain) never allocates.
	 */
	for (i = 0; i < nls_num_registered; i++)
	{
		if (nls_num_domains >= NLS_MAX_DOMAINS)
			break;

		dom = &nls_domains[nls_num_domains];
		memset(dom, 0, sizeof(NlsDomain));

		snprintf(mo_path, MAXPGPATH, "%s/%s/LC_MESSAGES/%s.mo",
				 nls_locale_path, nls_lang, nls_registered_domains[i]);

		if (!nls_load_domain(dom, mo_path))
		{
			elog(DEBUG3, "NLS: could not load .mo file \"%s\"", mo_path);
			continue;
		}

		dom->name = nls_registered_domains[i];
		nls_num_domains++;
	}

	if (nls_num_domains == 0)
		return;

	/*
	 * If the message encoding is not UTF-8, pre-convert all translations
	 * from UTF-8 (the standard .mo file encoding) to the message encoding
	 * using iconv (loaded at runtime from libiconv-2.dll).  This uses the
	 * same iconv library and encoding names that gettext itself uses, so
	 * all encodings are handled identically.
	 */
	msg_enc = GetMessageEncoding();
	if (msg_enc != PG_UTF8 && msg_enc != PG_SQL_ASCII &&
		PG_VALID_ENCODING(msg_enc) && pg_enc2gettext_tbl[msg_enc] != NULL)
	{
		nls_load_iconv();
		nls_target_enc = pg_enc2gettext_tbl[msg_enc];
		for (i = 0; i < nls_num_domains; i++)
			nls_convert_domain(&nls_domains[i], nls_target_enc);
		elog(DEBUG3, "NLS: converting translations from UTF-8 to %s",
			 nls_target_enc);
	}

	nls_available = true;
	elog(DEBUG3, "NLS: loaded %d domain(s) for language \"%s\"",
		 nls_num_domains, nls_lang);
}

/*
 * nls_register_domain --- register a text domain for eager loading.
 *
 * Called from pg_bindtextdomain() when an extension registers its text
 * domain.  If the locale has already been probed, the domain's .mo file
 * is loaded immediately; otherwise it will be loaded on the next call to
 * nls_probe_locale().
 *
 * The domain parameter must be a string constant (from PG_TEXTDOMAIN)
 * that persists for the process lifetime.
 */
void
nls_register_domain(const char *domain)
{
	int			i;
	char		mo_path[MAXPGPATH];
	NlsDomain  *dom;

	/* Check if already registered */
	for (i = 0; i < nls_num_registered; i++)
	{
		if (strcmp(nls_registered_domains[i], domain) == 0)
			return;					/* already registered */
	}

	if (nls_num_registered >= NLS_MAX_DOMAINS)
	{
		elog(DEBUG1, "NLS: too many registered domains (max %d), ignoring \"%s\"",
			 NLS_MAX_DOMAINS, domain);
		return;
	}

	/* Store pointer to string constant --- no allocation needed */
	nls_registered_domains[nls_num_registered++] = domain;

	/* If locale is already active, load the domain immediately */
	if (nls_available && nls_lang[0] != '\0')
	{
		/* Already loaded? */
		for (i = 0; i < nls_num_domains; i++)
		{
			if (strcmp(nls_domains[i].name, domain) == 0)
				return;
		}

		if (nls_num_domains >= NLS_MAX_DOMAINS)
			return;

		dom = &nls_domains[nls_num_domains];
		memset(dom, 0, sizeof(NlsDomain));

		snprintf(mo_path, MAXPGPATH, "%s/%s/LC_MESSAGES/%s.mo",
				 nls_locale_path, nls_lang, domain);

		if (nls_load_domain(dom, mo_path))
		{
			dom->name = domain;
			if (nls_target_enc != NULL)
				nls_convert_domain(dom, nls_target_enc);

			nls_num_domains++;
			elog(DEBUG3, "NLS: eagerly loaded domain \"%s\"", domain);
		}
	}
}

#else							/* !ENABLE_NLS */

/*
 * Stubs when ENABLE_NLS is not defined.  This file is only compiled on
 * Windows, but NLS might be disabled in the build configuration.
 */
bool		nls_available = false;

const char *
nls_lookup(const char *msgid)
{
	return msgid;
}

const char *
nls_lookup_domain(const char *domain, const char *msgid)
{
	return msgid;
}

void
nls_probe_locale(void)
{
}

void
nls_register_domain(const char *domain)
{
}

#endif							/* ENABLE_NLS */
