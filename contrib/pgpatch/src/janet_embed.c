/*-------------------------------------------------------------------------
 *
 * janet_embed.c
 *		Janet VM lifecycle and evaluation for pgpatch.
 *
 * All Janet header exposure is confined to this translation unit so that
 * Janet's macros cannot collide with Postgres macros elsewhere in the
 * extension.
 *
 * Threading model
 * ---------------
 * Janet's per-thread state (janet_vm) is thread-local, but pgpatch runs the
 * REPL on a dedicated thread while patched code paths execute Janet on the
 * backend's main thread.  We therefore keep the canonical VM in a single
 * heap-allocated JanetVM (pgpatch_vm) and bracket *every* access from *any*
 * thread with:
 *
 *		lock(vm) -> janet_vm_load(pgpatch_vm) -> ...work... ->
 *		janet_vm_save(pgpatch_vm) -> unlock(vm)
 *
 * The mutex guarantees only one thread touches Janet at a time, and the
 * save/load keeps the shared VM struct authoritative between threads.  This is
 * exactly the pattern Janet documents for "embedding where only one thread
 * runs Janet at a time" (janet_vm_alloc/save/load).
 *
 * IMPORTANT: functions here must not call palloc/ereport when they can run on
 * the REPL thread; they use libc malloc and return status instead.  The
 * main-thread SQL wrapper converts to palloc/ereport.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <pthread.h>

#include "pgpatch.h"
#include "pgpatch_ffi.h"
#include "pgpatch_janet.h"

/* Register the pgpatch bindings into `env` (defined in patch.c and
 * introspect.c). */
extern void pgpatch_patch_register(JanetTable *env);
extern void pgpatch_introspect_register(JanetTable *env);

static JanetVM *pgpatch_vm = NULL;
static JanetTable *pgpatch_env = NULL;
static pthread_mutex_t pgpatch_vm_lock;
static int	pgpatch_vm_depth = 0;	/* re-entrancy depth (guarded by the lock) */
static bool pgpatch_vm_ready = false;

/*
 * Enter/leave a VM critical section.  The mutex is recursive so a patched
 * function invoked from within Janet (Janet -> C -> patched hook -> Janet) can
 * re-enter on the same thread without deadlocking.  The shared VM struct is
 * loaded/saved only at the outermost entry/exit; nested entries run on the
 * already-loaded VM.
 */
static void
vm_enter(void)
{
	pthread_mutex_lock(&pgpatch_vm_lock);
	if (pgpatch_vm_depth++ == 0)
		janet_vm_load(pgpatch_vm);
}

static void
vm_leave(void)
{
	if (--pgpatch_vm_depth == 0)
		janet_vm_save(pgpatch_vm);
	pthread_mutex_unlock(&pgpatch_vm_lock);
}

/*
 * Create the Janet VM for this backend, once, on the main thread.  Idempotent.
 */
void
pgpatch_janet_init(void)
{
	if (pgpatch_vm_ready)
		return;

	{
		pthread_mutexattr_t attr;

		pthread_mutexattr_init(&attr);
		pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
		pthread_mutex_init(&pgpatch_vm_lock, &attr);
		pthread_mutexattr_destroy(&attr);
	}

	janet_init();
	pgpatch_env = janet_core_env(NULL);
	janet_gcroot(janet_wrap_table(pgpatch_env));

	/* Register the pgpatch bindings while the VM is the live thread-local
	 * one. */
	pgpatch_patch_register(pgpatch_env);
	pgpatch_introspect_register(pgpatch_env);

	/* Snapshot the freshly-initialized VM into the shared, heap-owned copy. */
	pgpatch_vm = janet_vm_alloc();
	janet_vm_save(pgpatch_vm);

	pgpatch_vm_ready = true;
}

/*
 * Core evaluator, safe to call from any thread.  Evaluates `code` under the VM
 * lock and returns a malloc'd, NUL-terminated printed representation of the
 * result (caller frees with free()).  Sets *is_error if the evaluation raised
 * a Janet-level error.  Touches only Janet and libc — never Postgres.
 */
char *
pgpatch_janet_eval_locked(const char *code, bool *is_error)
{
	Janet		out = janet_wrap_nil();
	int			status;
	char	   *val;
	char	   *result;
	JanetBuffer *outbuf;
	size_t		plen,
				vlen,
				extra;

	vm_enter();

	/*
	 * Redirect (dyn :out) to a buffer so (print ...) output is captured and
	 * surfaced (to the REPL socket / SQL result) rather than vanishing to the
	 * backend's stdout.  janet_dostring runs the code in a fiber whose env is
	 * pgpatch_env, and (dyn :out) reads that env, so bind :out there.
	 */
	outbuf = janet_buffer(64);
	janet_table_put(pgpatch_env, janet_ckeywordv("out"),
					janet_wrap_buffer(outbuf));

	status = janet_dostring(pgpatch_env, code, "pgpatch", &out);

	/*
	 * Render the value while the VM is loaded.  Successful results use Janet's
	 * pretty-printer so compound values show their contents; errors use the
	 * plain string form of the error value.
	 */
	if (status != 0)
	{
		JanetString js = janet_to_string(out);

		val = strdup((const char *) js);
	}
	else
	{
		JanetBuffer *b = janet_pretty(NULL, 4, JANET_PRETTY_ONELINE, out);

		val = malloc((size_t) b->count + 1);
		if (val != NULL)
		{
			memcpy(val, b->data, (size_t) b->count);
			val[b->count] = '\0';
		}
	}
	if (val == NULL)
		val = strdup("");

	/* Prepend any captured print output, separated by a newline if needed. */
	plen = (size_t) outbuf->count;
	vlen = strlen(val);
	extra = (plen > 0 && outbuf->data[plen - 1] != '\n') ? 1 : 0;
	result = malloc(plen + extra + vlen + 1);
	if (result != NULL)
	{
		memcpy(result, outbuf->data, plen);
		if (extra)
			result[plen] = '\n';
		memcpy(result + plen + extra, val, vlen + 1);
	}
	free(val);

	vm_leave();

	if (is_error)
		*is_error = (status != 0);
	if (result == NULL)
		result = strdup("");	/* out-of-memory fallback */
	return result;
}

/*
 * Return 1 if `buf` parses to a complete top-level form (or is empty), 0 if the
 * parser is still expecting more input (unbalanced delimiters).  Used by the
 * REPL to decide whether to evaluate or keep reading a continuation line.
 * Runs under the VM lock because the parser produces GC-managed values.
 */
int
pgpatch_input_complete(const char *buf)
{
	JanetParser p;
	const unsigned char *c;
	int			complete;

	vm_enter();

	janet_parser_init(&p);
	for (c = (const unsigned char *) buf; *c; c++)
		janet_parser_consume(&p, *c);
	complete = (janet_parser_status(&p) != JANET_PARSE_PENDING);
	janet_parser_deinit(&p);

	vm_leave();

	return complete;
}

/* Coerce a Janet value to a 64-bit machine word for use as a C return value. */
static uint64_t
janet_to_word(Janet v)
{
	switch (janet_type(v))
	{
		case JANET_NUMBER:
			return (uint64_t) (int64_t) janet_unwrap_number(v);
		case JANET_BOOLEAN:
			return janet_unwrap_boolean(v) ? 1 : 0;
		case JANET_NIL:
			return 0;
		case JANET_POINTER:
			return (uint64_t) (uintptr_t) janet_unwrap_pointer(v);
		case JANET_STRING:
		case JANET_SYMBOL:
		case JANET_KEYWORD:
			return (uint64_t) (uintptr_t) janet_unwrap_string(v);
		case JANET_ABSTRACT:
			/* int/u64 and int/s64 (e.g. an address passed straight through). */
			switch (janet_is_int(v))
			{
				case JANET_INT_U64:
					return janet_unwrap_u64(v);
				case JANET_INT_S64:
					return (uint64_t) janet_unwrap_s64(v);
				default:
					return 0;
			}
		default:
			return 0;
	}
}

/* Wrap one captured argument word as a typed Janet value. */
static Janet
word_to_janet(uint64 w, PgpFfiType t)
{
	switch (t)
	{
		case PGP_FT_S8:
			return janet_wrap_number((double) (int8_t) w);
		case PGP_FT_U8:
			return janet_wrap_number((double) (uint8_t) w);
		case PGP_FT_S16:
			return janet_wrap_number((double) (int16_t) w);
		case PGP_FT_U16:
			return janet_wrap_number((double) (uint16_t) w);
		case PGP_FT_S32:
			return janet_wrap_number((double) (int32_t) w);
		case PGP_FT_U32:
			return janet_wrap_number((double) (uint32_t) w);
		case PGP_FT_S64:
			return janet_wrap_s64((int64_t) w);
		case PGP_FT_PTR:
		case PGP_FT_U64:
		default:
			return janet_wrap_u64(w);
	}
}

/*
 * Call a Janet replacement for a patched function, under the VM lock.  The
 * captured general (gp) and floating (fp) argument registers are assigned to
 * the replacement's parameters by replaying the AAPCS classification described
 * by `argtypes` (integer/pointer -> next gp; float/double -> next fp).  The
 * result is returned in *ires (integer/pointer classes) or *dres (float/double,
 * per `ret_type`).
 *
 * This is the "guard": the whole Janet interaction happens while the lock is
 * held and the VM is loaded, and the lock is released before returning, so the
 * caller (main thread) may ereport() safely.  A Janet panic is caught by
 * janet_pcall rather than longjmp-ing into Postgres.
 */
bool
pgpatch_janet_call_typed(void *fn_v, const PgpFfiType *argtypes, int nargs,
						 const uint64 *gp, const double *fp,
						 PgpFfiType ret_type, uint64 *ires, double *dres,
						 char **errmsg)
{
	JanetFunction *fn = (JanetFunction *) fn_v;
	Janet		args[PGP_MAX_ARGS];
	Janet		out = janet_wrap_nil();
	JanetFiber *fiber = NULL;
	JanetSignal sig;
	bool		ok;
	int			i,
				gpi = 0,
				fpi = 0;

	if (errmsg)
		*errmsg = NULL;
	if (nargs < 0)
		nargs = 0;
	if (nargs > PGP_MAX_ARGS)
		nargs = PGP_MAX_ARGS;

	vm_enter();

	for (i = 0; i < nargs; i++)
	{
		if (argtypes[i] == PGP_FT_FLOAT || argtypes[i] == PGP_FT_DOUBLE)
			args[i] = janet_wrap_number(fpi < 8 ? fp[fpi++] : 0.0);
		else
			args[i] = word_to_janet(gpi < 8 ? gp[gpi++] : 0, argtypes[i]);
	}
	sig = janet_pcall(fn, nargs, args, &out, &fiber);

	if (sig == JANET_SIGNAL_OK)
	{
		if (ret_type == PGP_FT_FLOAT || ret_type == PGP_FT_DOUBLE)
			*dres = janet_checktype(out, JANET_NUMBER) ? janet_unwrap_number(out) : 0.0;
		else
			*ires = janet_to_word(out);
		ok = true;
	}
	else
	{
		JanetString js = janet_to_string(out);

		if (errmsg)
			*errmsg = strdup((const char *) js);
		ok = false;
	}

	vm_leave();

	return ok;
}

/*
 * Main-thread SQL wrapper: evaluate and return a palloc'd result, raising
 * ereport(ERROR) on a Janet-level error.
 */
char *
pgpatch_janet_eval(const char *code)
{
	bool		is_error = false;
	char	   *raw = pgpatch_janet_eval_locked(code, &is_error);
	char	   *result = pstrdup(raw);

	free(raw);

	if (is_error)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("janet error: %s", result)));

	return result;
}
