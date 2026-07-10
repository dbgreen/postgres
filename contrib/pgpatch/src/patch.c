/*-------------------------------------------------------------------------
 *
 * patch.c
 *		Calling, replacing, and peeking/poking backend C functions, and the
 *		Janet bindings for all of it (pgpatch/call, /patch, /peek, ...).
 *
 * Call marshals Janet args per the target's DWARF signature and invokes it
 * with libffi.  Patch overwrites the target's prologue (trampoline.h) to
 * redirect into a fixed pool of hook functions that capture the arm64 argument
 * registers (x0..x7, d0..d7) and forward them, typed from DWARF, to the Janet
 * replacement.
 *
 * Every entry into foreign code runs under pgp_guarded(): a Postgres ereport()
 * is caught by PG_TRY and a hard fault (SIGSEGV/Assert-abort/...) by a signal
 * handler, so poking at a live backend yields a Janet error, not a crash.
 *
 * THREADING: touching backend internals must happen on the main thread (it
 * relies on PG_exception_stack and backend globals), so call/peek/poke go via
 * SELECT pgpatch_eval('...').  The REPL socket runs on a worker thread and is
 * for defining/searching/patching; a patched replacement itself runs on the
 * main thread, since the trampoline fires during main-thread work.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <dlfcn.h>
#ifdef __APPLE__
#include <ffi/ffi.h>			/* SDK layout */
#else
#include <ffi.h>
#endif
#include <setjmp.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include "fmgr.h"
#include "utils/elog.h"
#include "utils/palloc.h"

#include "pgpatch.h"
#include "pgpatch_ffi.h"
#include "pgpatch_janet.h"
#include "trampoline.h"

void		pgpatch_patch_register(JanetTable *env);

/* Resolve a symbol (incl. non-exported) to its runtime address (introspect.c). */
extern void *pgpatch_symbol_addr(const char *name);

#define PGPATCH_MAX_PATCHES 16

typedef struct
{
	void	   *target;			/* address of the patched C function */
	uint8_t		saved[PGPATCH_STUB_LEN];	/* original prologue bytes */
	JanetFunction *fn;			/* Janet replacement (GC-rooted while active) */
	char	   *name;			/* malloc'd target name (for messages) */
	int			nargs;			/* number of args to forward */
	PgpFfiType	argtypes[PGP_MAX_ARGS];	/* per-arg machine class (from DWARF) */
	PgpFfiType	ret_type;		/* replacement's return class */
	bool		active;
}			PatchSlot;

static PatchSlot slots[PGPATCH_MAX_PATCHES];

/* ---- signal guard: turn a crash (SIGSEGV/SIGABRT/...) during a foreign
 * call into a catchable Janet panic instead of killing the backend ---- */

static const int pgp_guarded_signals[] = {
	SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP
};
#define PGP_NSIG (int) (sizeof(pgp_guarded_signals) / sizeof(pgp_guarded_signals[0]))

static sigjmp_buf *pgp_sig_env = NULL;	/* current guard's jump target */

static void
pgp_signal_handler(int signo)
{
	if (pgp_sig_env)
		siglongjmp(*pgp_sig_env, signo);
	/* No guard active: restore default and re-raise (normal crash). */
	signal(signo, SIG_DFL);
	raise(signo);
}

static void
pgp_install_signals(struct sigaction *saved)
{
	struct sigaction sa;
	int			i;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = pgp_signal_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_NODEFER;
	for (i = 0; i < PGP_NSIG; i++)
		sigaction(pgp_guarded_signals[i], &sa, &saved[i]);
}

static void
pgp_restore_signals(struct sigaction *saved)
{
	int			i;

	for (i = 0; i < PGP_NSIG; i++)
		sigaction(pgp_guarded_signals[i], &saved[i], NULL);
}

/*
 * Run op(ctx) with two safety nets, so poking at a live backend can't take it
 * down: a Postgres ereport() is caught by PG_TRY, and a hard fault
 * (SIGSEGV/SIGBUS/Assert-abort/...) is caught by our signal handler.  Returns
 * true on success.  On a caught PG error returns false with *emsg set to a
 * pstrdup'd message; on a caught signal returns false with *crashsig set.
 * Must run on the main thread (needs a valid PG_exception_stack).
 */
static bool
pgp_guarded(void (*op) (void *), void *ctx, char **emsg, int *crashsig)
{
	volatile bool ok = true;
	char	   *volatile em = NULL;
	MemoryContext oldctx = CurrentMemoryContext;
	sigjmp_buf *save_exc = PG_exception_stack;
	ErrorContextCallback *save_ectx = error_context_stack;
	sigjmp_buf *prev_sig_env = pgp_sig_env;
	struct sigaction saved[PGP_NSIG];
	sigjmp_buf	sigenv;
	int			crash;

	*emsg = NULL;
	*crashsig = 0;

	pgp_install_signals(saved);
	pgp_sig_env = &sigenv;

	if ((crash = sigsetjmp(sigenv, 1)) != 0)
	{
		/* A fatal signal fired inside op; unwind cleanly. */
		pgp_sig_env = prev_sig_env;
		pgp_restore_signals(saved);
		PG_exception_stack = save_exc;
		error_context_stack = save_ectx;
		MemoryContextSwitchTo(oldctx);
		*crashsig = crash;
		return false;
	}

	PG_TRY();
	{
		op(ctx);
	}
	PG_CATCH();
	{
		ErrorData  *ed;

		MemoryContextSwitchTo(oldctx);
		ed = CopyErrorData();
		em = pstrdup(ed->message);
		FreeErrorData(ed);
		FlushErrorState();
		ok = false;
	}
	PG_END_TRY();

	pgp_sig_env = prev_sig_env;
	pgp_restore_signals(saved);

	if (!ok)
		*emsg = em;
	return ok;
}

/* ---- libffi type mapping ---- */

static ffi_type *
ffi_type_of(PgpFfiType t)
{
	switch (t)
	{
		case PGP_FT_VOID:
			return &ffi_type_void;
		case PGP_FT_S8:
			return &ffi_type_sint8;
		case PGP_FT_U8:
			return &ffi_type_uint8;
		case PGP_FT_S16:
			return &ffi_type_sint16;
		case PGP_FT_U16:
			return &ffi_type_uint16;
		case PGP_FT_S32:
			return &ffi_type_sint32;
		case PGP_FT_U32:
			return &ffi_type_uint32;
		case PGP_FT_S64:
			return &ffi_type_sint64;
		case PGP_FT_U64:
			return &ffi_type_uint64;
		case PGP_FT_FLOAT:
			return &ffi_type_float;
		case PGP_FT_DOUBLE:
			return &ffi_type_double;
		case PGP_FT_PTR:
		default:
			return &ffi_type_pointer;
	}
}

/* ---- dispatch from the hook pool into the Janet replacement ----
 *
 * The hooks capture both the general (x0..x7) and floating (d0..d7) argument
 * registers.  Dispatch replays the AAPCS assignment using the target's DWARF
 * argument classes to hand the Janet replacement correctly-typed values, and
 * returns the replacement's result in the register class the target expects
 * (x0 for integer/pointer, d0 for float/double -> separate hook pool).
 */
static void
pgpatch_dispatch(int slot, const uint64_t *gp, const double *fp,
				 uint64_t *ires, double *dres)
{
	PatchSlot  *s = &slots[slot];
	char	   *err = NULL;

	*ires = 0;
	*dres = 0;
	if (!s->active || s->fn == NULL)
		return;

	if (!pgpatch_janet_call_typed(s->fn, s->argtypes, s->nargs, gp, fp,
								  s->ret_type, ires, dres, &err))
	{
		char	   *msg = pstrdup(err ? err : "unknown error");

		free(err);
		/* The guard already released the VM lock, so this longjmp is safe. */
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("pgpatch: janet replacement for %s failed: %s",
						s->name ? s->name : "?", msg)));
	}
}

/*
 * Two fixed pools of hook functions.  Each is entered via the patched target's
 * prologue with the original arguments still in x0..x7 / d0..d7; declaring both
 * integer and floating params captures both register files (unused ones are
 * harmless).  The GP pool returns its result in x0, the FP pool in d0, so the
 * pool is chosen by the target's return class.
 */
#define HOOK_ARGS \
	uint64_t g0, uint64_t g1, uint64_t g2, uint64_t g3, \
	uint64_t g4, uint64_t g5, uint64_t g6, uint64_t g7, \
	double f0, double f1, double f2, double f3, \
	double f4, double f5, double f6, double f7

#define DEFINE_HOOKS(k) \
	static uint64_t hook_gp_##k(HOOK_ARGS) \
	{ uint64_t gp[8]={g0,g1,g2,g3,g4,g5,g6,g7}; double fp[8]={f0,f1,f2,f3,f4,f5,f6,f7}; \
	  uint64_t ir; double dr; pgpatch_dispatch(k, gp, fp, &ir, &dr); return ir; } \
	static double hook_fp_##k(HOOK_ARGS) \
	{ uint64_t gp[8]={g0,g1,g2,g3,g4,g5,g6,g7}; double fp[8]={f0,f1,f2,f3,f4,f5,f6,f7}; \
	  uint64_t ir; double dr; pgpatch_dispatch(k, gp, fp, &ir, &dr); return dr; }

DEFINE_HOOKS(0) DEFINE_HOOKS(1) DEFINE_HOOKS(2) DEFINE_HOOKS(3)
DEFINE_HOOKS(4) DEFINE_HOOKS(5) DEFINE_HOOKS(6) DEFINE_HOOKS(7)
DEFINE_HOOKS(8) DEFINE_HOOKS(9) DEFINE_HOOKS(10) DEFINE_HOOKS(11)
DEFINE_HOOKS(12) DEFINE_HOOKS(13) DEFINE_HOOKS(14) DEFINE_HOOKS(15)

static void *const hook_gp_ptrs[PGPATCH_MAX_PATCHES] = {
	hook_gp_0, hook_gp_1, hook_gp_2, hook_gp_3, hook_gp_4, hook_gp_5,
	hook_gp_6, hook_gp_7, hook_gp_8, hook_gp_9, hook_gp_10, hook_gp_11,
	hook_gp_12, hook_gp_13, hook_gp_14, hook_gp_15
};
static void *const hook_fp_ptrs[PGPATCH_MAX_PATCHES] = {
	hook_fp_0, hook_fp_1, hook_fp_2, hook_fp_3, hook_fp_4, hook_fp_5,
	hook_fp_6, hook_fp_7, hook_fp_8, hook_fp_9, hook_fp_10, hook_fp_11,
	hook_fp_12, hook_fp_13, hook_fp_14, hook_fp_15
};

/* ---- helpers ---- */

static void *
resolve(const char *name)
{
	void	   *p = pgpatch_symbol_addr(name);

	if (p == NULL)
		p = dlsym(RTLD_DEFAULT, name);
	return p;
}

static int
find_slot_by_target(void *target)
{
	int			i;

	for (i = 0; i < PGPATCH_MAX_PATCHES; i++)
		if (slots[i].active && slots[i].target == target)
			return i;
	return -1;
}

/* Marshal one Janet argument to a machine word. */
static uint64_t
arg_word(const Janet *argv, int32_t i)
{
	Janet		v = argv[i];

	if (janet_checktype(v, JANET_STRING) || janet_checktype(v, JANET_SYMBOL) ||
		janet_checktype(v, JANET_KEYWORD))
		return (uint64_t) (uintptr_t) janet_unwrap_string(v);
	if (janet_checktype(v, JANET_BUFFER))
		return (uint64_t) (uintptr_t) janet_unwrap_buffer(v)->data;
	if (janet_checktype(v, JANET_POINTER))
		return (uint64_t) (uintptr_t) janet_unwrap_pointer(v);
	if (janet_checktype(v, JANET_NIL))
		return 0;
	if (janet_checktype(v, JANET_BOOLEAN))
		return janet_unwrap_boolean(v) ? 1 : 0;
	return (uint64_t) janet_getinteger64(argv, i);
}

/*
 * Convert a machine value of class `t` to a Janet value.  Integer/pointer
 * classes come in via `u` (signed classes sign-extended into it); float/double
 * via `d`.  Small integers become plain numbers; 64-bit values stay precise.
 */
static Janet
result_to_janet(PgpFfiType t, uint64_t u, double d)
{
	switch (t)
	{
		case PGP_FT_VOID:
			return janet_wrap_nil();
		case PGP_FT_FLOAT:
		case PGP_FT_DOUBLE:
			return janet_wrap_number(d);
		case PGP_FT_S8:
		case PGP_FT_S16:
		case PGP_FT_S32:
			return janet_wrap_number((double) (int64_t) u);
		case PGP_FT_U8:
		case PGP_FT_U16:
		case PGP_FT_U32:
			return janet_wrap_number((double) u);
		case PGP_FT_S64:
			return janet_wrap_s64((int64_t) u);
		case PGP_FT_U64:
		case PGP_FT_PTR:
		default:
			return (u <= 0xffffffffULL) ? janet_wrap_number((double) u)
				: janet_wrap_u64(u);
	}
}

/* Guarded foreign-call operation (run via pgp_guarded). */
typedef struct
{
	ffi_cif    *cif;
	void	   *addr;
	void	  **avalues;
	void	   *ret;
}			CallCtx;

static void
do_ffi_call(void *p)
{
	CallCtx    *c = (CallCtx *) p;

	ffi_call(c->cif, FFI_FN(c->addr), c->ret, c->avalues);
}

/* Map a Janet type keyword (:s32, :ptr, :double, aliases...) to a class. */
static PgpFfiType
type_from_keyword(const Janet *argv, int32_t i)
{
	const char *k = (const char *) janet_getkeyword(argv, i);

	if (!strcmp(k, "s8") || !strcmp(k, "i8"))
		return PGP_FT_S8;
	if (!strcmp(k, "u8") || !strcmp(k, "byte") || !strcmp(k, "bool"))
		return PGP_FT_U8;
	if (!strcmp(k, "s16") || !strcmp(k, "i16") || !strcmp(k, "short"))
		return PGP_FT_S16;
	if (!strcmp(k, "u16") || !strcmp(k, "ushort"))
		return PGP_FT_U16;
	if (!strcmp(k, "s32") || !strcmp(k, "i32") || !strcmp(k, "int"))
		return PGP_FT_S32;
	if (!strcmp(k, "u32") || !strcmp(k, "uint"))
		return PGP_FT_U32;
	if (!strcmp(k, "s64") || !strcmp(k, "i64") || !strcmp(k, "long"))
		return PGP_FT_S64;
	if (!strcmp(k, "u64") || !strcmp(k, "ulong"))
		return PGP_FT_U64;
	if (!strcmp(k, "float"))
		return PGP_FT_FLOAT;
	if (!strcmp(k, "double"))
		return PGP_FT_DOUBLE;
	if (!strcmp(k, "ptr") || !strcmp(k, "pointer"))
		return PGP_FT_PTR;
	janet_panicf("pgpatch: unknown type keyword :%s", k);
}

/* ---- guarded memory peek/poke ---- */

typedef struct
{
	void	   *addr;
	PgpFfiType	type;
	bool		is_string;		/* read a NUL-terminated C string */
	uint64_t	uval;			/* integer/pointer in/out */
	double		dval;			/* float/double in/out */
	int32_t		slen;			/* :string length (out) */
}			MemCtx;

static void
do_peek(void *p)
{
	MemCtx     *c = (MemCtx *) p;

	if (c->is_string)
	{
		c->slen = (int32_t) strlen((const char *) c->addr);
		return;
	}
	switch (c->type)
	{
		case PGP_FT_S8:
			c->uval = (uint64_t) (int64_t) *(int8_t *) c->addr;
			break;
		case PGP_FT_U8:
			c->uval = *(uint8_t *) c->addr;
			break;
		case PGP_FT_S16:
			c->uval = (uint64_t) (int64_t) *(int16_t *) c->addr;
			break;
		case PGP_FT_U16:
			c->uval = *(uint16_t *) c->addr;
			break;
		case PGP_FT_S32:
			c->uval = (uint64_t) (int64_t) *(int32_t *) c->addr;
			break;
		case PGP_FT_U32:
			c->uval = *(uint32_t *) c->addr;
			break;
		case PGP_FT_S64:
		case PGP_FT_U64:
			c->uval = *(uint64_t *) c->addr;
			break;
		case PGP_FT_PTR:
			c->uval = (uint64_t) (uintptr_t) *(void **) c->addr;
			break;
		case PGP_FT_FLOAT:
			c->dval = *(float *) c->addr;
			break;
		case PGP_FT_DOUBLE:
			c->dval = *(double *) c->addr;
			break;
		default:
			break;
	}
}

static void
do_poke(void *p)
{
	MemCtx     *c = (MemCtx *) p;

	switch (c->type)
	{
		case PGP_FT_S8:
		case PGP_FT_U8:
			*(uint8_t *) c->addr = (uint8_t) c->uval;
			break;
		case PGP_FT_S16:
		case PGP_FT_U16:
			*(uint16_t *) c->addr = (uint16_t) c->uval;
			break;
		case PGP_FT_S32:
		case PGP_FT_U32:
			*(uint32_t *) c->addr = (uint32_t) c->uval;
			break;
		case PGP_FT_S64:
		case PGP_FT_U64:
			*(uint64_t *) c->addr = c->uval;
			break;
		case PGP_FT_PTR:
			*(void **) c->addr = (void *) (uintptr_t) c->uval;
			break;
		case PGP_FT_FLOAT:
			*(float *) c->addr = (float) c->dval;
			break;
		case PGP_FT_DOUBLE:
			*(double *) c->addr = c->dval;
			break;
		default:
			break;
	}
}

/* ---------------- Janet bindings ---------------- */

static Janet
cfun_find(int32_t argc, Janet *argv)
{
	const char *name;
	void	   *p;
	char		buf[20];

	janet_fixarity(argc, 1);
	name = janet_getcstring(argv, 0);
	p = resolve(name);
	if (p == NULL)
		return janet_wrap_nil();
	snprintf(buf, sizeof(buf), "%p", p);
	return janet_cstringv(buf);
}

/* Infer a machine type from a Janet arg (used when DWARF has no signature). */
static PgpFfiType
infer_type(const Janet *argv, int32_t i)
{
	Janet		v = argv[i];

	if (janet_checktype(v, JANET_STRING) || janet_checktype(v, JANET_SYMBOL) ||
		janet_checktype(v, JANET_KEYWORD) || janet_checktype(v, JANET_BUFFER) ||
		janet_checktype(v, JANET_POINTER))
		return PGP_FT_PTR;
	return PGP_FT_S64;
}

/*
 * Shared implementation for pgpatch/call (by name) and pgpatch/call-at (by
 * address).  `dname` is the symbol name to look up in DWARF, or NULL (no DWARF
 * -> integer/pointer inference).  Call args are argv[1..argc-1].
 */
static Janet
call_impl(void *addr, const char *dname, int32_t argc, Janet *argv)
{
	const char *name = dname ? dname : "<addr>";
	PgpSig		sig;
	bool		have_dwarf;
	PgpFfiType	rettype;
	PgpFfiType	argtype[PGP_MAX_ARGS];
	ffi_type   *atypes[PGP_MAX_ARGS];
	void	   *avalues[PGP_MAX_ARGS];
	union
	{
		int64_t		s;
		uint64_t	u;
		double		d;
		float		f;
		void	   *p;
	}			slot[PGP_MAX_ARGS];
	union
	{
		ffi_arg		u;			/* libffi widens integer returns to this */
		double		d;
		float		f;
	}			ret;
	ffi_cif		cif;
	int			n,
				i;

	n = argc - 1;

	/* Derive argument/return machine types from DWARF, else infer/word. */
	have_dwarf = dname && pgpatch_dwarf_ffi_sig(dname, &sig);
	rettype = have_dwarf ? sig.ret : PGP_FT_S64;
	if (rettype == PGP_FT_STRUCT)
		janet_panicf("pgpatch/call: %s returns a struct by value (unsupported)", name);
	for (i = 0; i < n; i++)
	{
		PgpFfiType	t = (have_dwarf && i < sig.nargs)
			? sig.args[i] : infer_type(argv, i + 1);

		if (t == PGP_FT_STRUCT)
			janet_panicf("pgpatch/call: %s takes a struct by value (unsupported)", name);
		argtype[i] = t;
		atypes[i] = ffi_type_of(t);
	}

	if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, (unsigned) n,
					 ffi_type_of(rettype), atypes) != FFI_OK)
		janet_panic("pgpatch/call: ffi_prep_cif failed");

	/* Marshal Janet args into per-arg storage. */
	for (i = 0; i < n; i++)
	{
		if (argtype[i] == PGP_FT_FLOAT)
			slot[i].f = (float) janet_getnumber(argv, i + 1);
		else if (argtype[i] == PGP_FT_DOUBLE)
			slot[i].d = janet_getnumber(argv, i + 1);
		else
			slot[i].u = arg_word(argv, i + 1);	/* ints, ptr, string, nil, bool */
		avalues[i] = &slot[i];
	}

	/*
	 * Guarded call: a callee ereport() or hard crash becomes a Janet panic and
	 * the backend survives.  Must run on the main thread.
	 */
	{
		CallCtx		cc = {&cif, addr, avalues, &ret};
		char	   *emsg = NULL;
		int			crashsig = 0;

		if (!pgp_guarded(do_ffi_call, &cc, &emsg, &crashsig))
		{
			if (crashsig)
			{
				char		msg[128];

				snprintf(msg, sizeof(msg),
						 "pgpatch/call: %s crashed (signal %d)", name, crashsig);
				janet_panic(msg);
			}
			janet_panicf("pgpatch/call: %s raised: %s", name,
						 emsg ? emsg : "error");
		}
	}

	return result_to_janet(rettype, (uint64_t) ret.u,
						   rettype == PGP_FT_FLOAT ? (double) ret.f : ret.d);
}

/* Extract a code address from a Janet value: a number, or a "0x..."/decimal
 * string (as returned by pgpatch/find and pgpatch/search). */
static void *
addr_from_janet(const Janet *argv, int32_t i)
{
	Janet		v = argv[i];

	if (janet_checktype(v, JANET_STRING) || janet_checktype(v, JANET_SYMBOL) ||
		janet_checktype(v, JANET_KEYWORD))
	{
		const char *s = (const char *) janet_unwrap_string(v);

		return (void *) (uintptr_t) strtoull(s, NULL, 0);
	}
	return (void *) (uintptr_t) janet_getinteger64(argv, i);
}

static Janet
cfun_call(int32_t argc, Janet *argv)
{
	const char *name;
	void	   *addr;

	janet_arity(argc, 1, 1 + PGP_MAX_ARGS);
	name = janet_getcstring(argv, 0);
	addr = resolve(name);
	if (addr == NULL)
		janet_panicf("pgpatch/call: symbol not found: %s", name);
	return call_impl(addr, name, argc, argv);
}

static Janet
cfun_call_at(int32_t argc, Janet *argv)
{
	void	   *addr;

	janet_arity(argc, 1, 1 + PGP_MAX_ARGS);
	addr = addr_from_janet(argv, 0);
	if (addr == NULL)
		janet_panic("pgpatch/call-at: null address");
	return call_impl(addr, NULL, argc, argv);
}

static Janet
cfun_peek(int32_t argc, Janet *argv)
{
	MemCtx		m;
	const char *k;
	char	   *emsg = NULL;
	int			crashsig = 0;

	janet_fixarity(argc, 2);
	memset(&m, 0, sizeof(m));
	m.addr = addr_from_janet(argv, 0);
	k = (const char *) janet_getkeyword(argv, 1);
	if (!strcmp(k, "string"))
		m.is_string = true;
	else
		m.type = type_from_keyword(argv, 1);

	if (!pgp_guarded(do_peek, &m, &emsg, &crashsig))
	{
		if (crashsig)
		{
			char		msg[128];

			snprintf(msg, sizeof(msg),
					 "pgpatch/peek: bad address %p (signal %d)", m.addr, crashsig);
			janet_panic(msg);
		}
		janet_panicf("pgpatch/peek: %s", emsg ? emsg : "error");
	}

	if (m.is_string)
		return janet_stringv((const uint8_t *) m.addr, m.slen);
	return result_to_janet(m.type, m.uval, m.dval);
}

static Janet
cfun_poke(int32_t argc, Janet *argv)
{
	MemCtx		m;
	char	   *emsg = NULL;
	int			crashsig = 0;

	janet_fixarity(argc, 3);
	memset(&m, 0, sizeof(m));
	m.addr = addr_from_janet(argv, 0);
	m.type = type_from_keyword(argv, 1);
	if (m.type == PGP_FT_FLOAT || m.type == PGP_FT_DOUBLE)
		m.dval = janet_getnumber(argv, 2);
	else
		m.uval = arg_word(argv, 2);

	if (!pgp_guarded(do_poke, &m, &emsg, &crashsig))
	{
		if (crashsig)
		{
			char		msg[128];

			snprintf(msg, sizeof(msg),
					 "pgpatch/poke: bad address %p (signal %d)", m.addr, crashsig);
			janet_panic(msg);
		}
		janet_panicf("pgpatch/poke: %s", emsg ? emsg : "error");
	}
	return janet_wrap_nil();
}

/*
 * Install (or re-point) a patch of `target` to Janet `fn`.  `dname` is the
 * symbol name for DWARF typing, or NULL (by address -> raw integer/pointer
 * words, arity from the replacement).
 */
static Janet
patch_impl(void *target, const char *dname, JanetFunction *fn)
{
	PgpSig		sig;
	bool		have_dwarf;
	int			nargs;
	PgpFfiType	argtypes[PGP_MAX_ARGS];
	PgpFfiType	ret_type;
	void	   *const *pool;
	int			slot;
	int			i;

	have_dwarf = dname && pgpatch_dwarf_ffi_sig(dname, &sig) && !sig.has_struct;
	if (have_dwarf)
	{
		nargs = sig.nargs;
		ret_type = sig.ret;
		for (i = 0; i < nargs && i < PGP_MAX_ARGS; i++)
			argtypes[i] = sig.args[i];
	}
	else
	{
		/* No DWARF: forward the replacement's declared arity as raw words. */
		nargs = fn->def->arity;
		if (nargs < 0)
			nargs = 0;
		if (nargs > PGP_MAX_ARGS)
			nargs = PGP_MAX_ARGS;
		ret_type = PGP_FT_U64;
		for (i = 0; i < nargs; i++)
			argtypes[i] = PGP_FT_U64;
	}

	/* Float/double returns must come back in d0 -> use the FP hook pool. */
	pool = (ret_type == PGP_FT_FLOAT || ret_type == PGP_FT_DOUBLE)
		? hook_fp_ptrs : hook_gp_ptrs;

	slot = find_slot_by_target(target);
	if (slot >= 0)
	{
		/* Re-point: revert to original first so saved[] stays authoritative. */
		janet_gcunroot(janet_wrap_function(slots[slot].fn));
		pgpatch_arch_unpatch(target, slots[slot].saved);
	}
	else
	{
		for (slot = 0; slot < PGPATCH_MAX_PATCHES; slot++)
			if (!slots[slot].active)
				break;
		if (slot == PGPATCH_MAX_PATCHES)
			janet_panic("pgpatch: no free patch slots");
		slots[slot].target = target;
		slots[slot].name = strdup(dname ? dname : "<addr>");
	}

	if (!pgpatch_arch_patch(target, pool[slot], slots[slot].saved))
		janet_panic("pgpatch: failed to install patch");
	slots[slot].active = true;
	slots[slot].fn = fn;
	slots[slot].nargs = nargs;
	slots[slot].ret_type = ret_type;
	for (i = 0; i < nargs; i++)
		slots[slot].argtypes[i] = argtypes[i];
	janet_gcroot(janet_wrap_function(fn));
	return janet_wrap_nil();
}

static Janet
cfun_patch(int32_t argc, Janet *argv)
{
	const char *name;
	void	   *target;

	janet_fixarity(argc, 2);
	name = janet_getcstring(argv, 0);
	target = resolve(name);
	if (target == NULL)
		janet_panicf("pgpatch: symbol not found: %s", name);
	return patch_impl(target, name, janet_getfunction(argv, 1));
}

static Janet
cfun_patch_at(int32_t argc, Janet *argv)
{
	void	   *target;

	janet_fixarity(argc, 2);
	target = addr_from_janet(argv, 0);
	if (target == NULL)
		janet_panic("pgpatch/patch-at: null address");
	return patch_impl(target, NULL, janet_getfunction(argv, 1));
}

static Janet
unpatch_target(void *target)
{
	int			slot = find_slot_by_target(target);

	if (slot < 0)
		return janet_wrap_nil();
	pgpatch_arch_unpatch(slots[slot].target, slots[slot].saved);
	janet_gcunroot(janet_wrap_function(slots[slot].fn));
	free(slots[slot].name);
	memset(&slots[slot], 0, sizeof(slots[slot]));
	return janet_wrap_nil();
}

static Janet
cfun_unpatch(int32_t argc, Janet *argv)
{
	void	   *target;

	janet_fixarity(argc, 1);
	target = resolve(janet_getcstring(argv, 0));
	if (target == NULL)
		return janet_wrap_nil();
	return unpatch_target(target);
}

static Janet
cfun_unpatch_at(int32_t argc, Janet *argv)
{
	janet_fixarity(argc, 1);
	return unpatch_target(addr_from_janet(argv, 0));
}

static const JanetReg cfuns[] = {
	{"find", cfun_find,
		"(pgpatch/find name)\n\nReturn the address of C symbol `name` as a string, or nil."},
	{"call", cfun_call,
		"(pgpatch/call name & args)\n\n"
		"Call C function `name` with up to 8 args, typed from its DWARF "
		"signature (integers, floats/doubles, pointers; strings/buffers as "
		"pointers, nil as NULL).  Guarded against ereport and crashes.  Must run "
		"on the main thread: use SELECT pgpatch_eval('...')."},
	{"call-at", cfun_call_at,
		"(pgpatch/call-at addr & args)\n\n"
		"Like pgpatch/call but by address (number or \"0x..\" string from "
		"pgpatch/search); args use integer/pointer inference (no DWARF)."},
	{"patch", cfun_patch,
		"(pgpatch/patch name fn)\n\n"
		"Replace C function `name` with Janet `fn`.  Arguments are typed and "
		"counted from the target's DWARF signature (floats/doubles included)."},
	{"patch-at", cfun_patch_at,
		"(pgpatch/patch-at addr fn)\n\n"
		"Like pgpatch/patch but by address; forwards fn's arity as raw words."},
	{"unpatch", cfun_unpatch,
		"(pgpatch/unpatch name)\n\nRestore the original C function `name`."},
	{"unpatch-at", cfun_unpatch_at,
		"(pgpatch/unpatch-at addr)\n\nRestore the original function at `addr`."},
	{"peek", cfun_peek,
		"(pgpatch/peek addr type)\n\n"
		"Read a value of `type` (:s8 :u8 :s16 :u16 :s32 :u32 :s64 :u64 :float "
		":double :ptr :string, plus aliases :int :uint :byte ...) from memory at "
		"`addr`.  Guarded: a bad address yields an error, not a crash."},
	{"poke", cfun_poke,
		"(pgpatch/poke addr type value)\n\n"
		"Write `value` of `type` to memory at `addr`.  Guarded.  Dangerous."},
	{NULL, NULL, NULL}
};

void
pgpatch_patch_register(JanetTable *env)
{
	/* _prefix binds the functions under "pgpatch/<name>". */
	janet_cfuns_prefix(env, "pgpatch", cfuns);
}
