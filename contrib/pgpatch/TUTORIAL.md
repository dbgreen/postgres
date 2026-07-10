# pgpatch tutorial

pgpatch drops a [Janet](https://janet-lang.org) REPL inside a running PostgreSQL
backend so you can explore it from the inside: search its C functions, inspect
their signatures and struct layouts, **call** internal functions that have no
SQL surface, read and write backend memory, and **hot-patch** a function so a
Janet function runs in its place — all without restarting the server.

It is a debugging / exploration / fuzzing tool. Point it at a scratch cluster,
not production.

> **Platform:** macOS / arm64 only. Requires a `postgres` built with debug info
> and, for signature/struct introspection, a generated `.dSYM`.

---

## 1. Setup

Build and install this tree (see the top-level docs for meson/autoconf), then
generate the `.dSYM` so the DWARF reader has something to read:

```sh
dsymutil <prefix>/bin/postgres        # once per build
```

Start a cluster (keep the socket dir short — the Unix-socket path limit is ~103
bytes), then load the extension:

```sql
CREATE EXTENSION pgpatch;             -- or: LOAD 'pgpatch';
SELECT pgpatch_socket_path();         -- e.g. /tmp/pgpatch/12345.sock
```

Attach to *this backend's* REPL from another terminal:

```sh
nc -U /tmp/pgpatch/12345.sock
```

```
; pgpatch Janet REPL. Definitions persist in this backend.
pgpatch> (+ 1 2)
3
```

### The one rule: which thread runs what

The REPL socket runs on a **worker thread**. That thread can safely do anything
that doesn't touch backend internals: define Janet functions, `search`,
`params`, `struct`, `patch`, `unpatch`.

Anything that reaches into the backend — `call`, `call-at`, `peek`, `poke` —
must run on the backend's **main thread**, because it relies on the backend's
error handling and global state. Run those through SQL:

```sql
SELECT pgpatch_eval($$ (pgpatch/call "pg_strtoint32" "42") $$);
```

`pgpatch_eval` evaluates Janet on the main thread and shares the same VM as the
REPL, so definitions you make in the REPL are visible to `pgpatch_eval` and
vice versa. A patched function's replacement always runs on the main thread too
(it fires when the real code path executes during a query).

---

## 2. Finding functions

`(pgpatch/search pattern &opt limit)` scans the live process's symbol tables
(including non-exported functions) for names containing `pattern`:

```
pgpatch> (pgpatch/search "int4pl" 5)
@[("int4pl.cold.1" "0x104be53c4") ("int4pl" "0x1049b1bd0") ...]

pgpatch> (map first (pgpatch/search "numeric_add" 5))
@["numeric_add" "numeric_add_safe"]
```

`(pgpatch/find name)` returns one symbol's address as a string, or nil.

---

## 3. Inspecting signatures and structs

`(pgpatch/params name)` reads the C signature from DWARF:

```
pgpatch> (pgpatch/params "heap_form_tuple")
"HeapTuple heap_form_tuple(TupleDesc tupleDescriptor, const Datum * values, const _Bool * isnull)"
```

`(pgpatch/struct name)` returns a struct/union layout as data you can compute
over — `{:name :kind :size :fields @[{:offset :name :type} ...]}`:

```
pgpatch> (map |[($ :name) ($ :offset) ($ :type)]
              ((pgpatch/struct "HeapTupleData") :fields))
@[("t_len" 0 "uint32") ("t_self" 4 "ItemPointerData")
  ("t_tableOid" 12 "Oid") ("t_data" 16 "HeapTupleHeader")]
```

`(pgpatch/struct-str name)` gives the same thing as a formatted string — print
it to see it laid out:

```
pgpatch> (print (pgpatch/struct-str "HeapTupleData"))
struct HeapTupleData {  /* 24 bytes */
  +0x0000  uint32 t_len;
  +0x0004  ItemPointerData t_self;
  +0x000c  Oid t_tableOid;
  +0x0010  HeapTupleHeader t_data;
}
```

(If your `.dSYM` isn't at the default location, point at it with
`(pgpatch/set-dwarf "<bundle>/Contents/Resources/DWARF/postgres")`.)

---

## 4. Calling internal functions

`(pgpatch/call name & args)` invokes any resolvable C function with up to 12
arguments, typed from its DWARF signature (integers of the right width,
floats/doubles, pointers). Strings and buffers pass as pointers; `nil` as NULL.
Run it on the main thread:

```sql
SELECT pgpatch_eval($$ (pgpatch/call "pg_strncasecmp" "Hello" "HELLO" 5) $$);  -- 0
SELECT pgpatch_eval($$ (pgpatch/call "clamp_row_est" 3.7) $$);                 -- 4
```

Calls are **guarded**: if the callee raises a Postgres error or crashes
(SIGSEGV, an `Assert`, ...), you get a catchable Janet error and the backend
keeps running — so you can loop over inputs while fuzzing:

```sql
SELECT pgpatch_eval($$ (pgpatch/call "pg_strtoint32" "99999999999999999999") $$);
-- ERROR: janet error: pgpatch/call: pg_strtoint32 raised: value "..." out of range
SELECT pgpatch_eval($$ (pgpatch/call "pg_strncasecmp" 0 0 5) $$);
-- ERROR: janet error: pgpatch/call: pg_strncasecmp crashed (signal 11)
```

`(pgpatch/call-at addr & args)` is the same but takes an address (a number or a
`"0x…"` string from `search`/`find`); with no name there's no DWARF, so args are
integer/pointer-inferred.

---

## 5. Reading and writing memory

`(pgpatch/peek addr type)` and `(pgpatch/poke addr type value)` read/write a
typed value at an address (`:s8 :u8 :s16 :u16 :s32 :u32 :s64 :u64 :float
:double :ptr :string`, plus aliases like `:int :uint :byte :long`). Both are
guarded, so a bad address is an error, not a crash.

Combined with `struct`, this reads live struct fields by name:

```janet
# read CurrentMemoryContext->name from this backend:
(let [s     (pgpatch/struct "MemoryContextData")
      noff  ((first (filter |(= ($ :name) "name") (s :fields))) :offset)
      ctx   (pgpatch/peek (pgpatch/find "CurrentMemoryContext") :ptr)
      np    (pgpatch/peek (+ ctx noff) :ptr)]
  (pgpatch/peek np :string))          # => "ExprContext"
```

Run that via `pgpatch_eval` (main thread).

---

## 6. Patching a function

`(pgpatch/patch name fn)` replaces a C function with a Janet function. The
number and types of arguments handed to `fn` come from the target's DWARF
signature (floats and doubles included). Do this from the REPL, then trigger it
from SQL:

```
pgpatch> (defn my+ [fcinfo] 42)
pgpatch> (pgpatch/patch "int4pl" my+)
```
```sql
SELECT 2 + 2;                          -- 42
```
```
pgpatch> (pgpatch/unpatch "int4pl")
```
```sql
SELECT 2 + 2;                          -- 4
```

`int4pl` is an fmgr (V1) function, so its single C argument is the
`FunctionCallInfo` — the replacement returns a `Datum`. For a non-fmgr internal
function the replacement receives the real, typed arguments:

```
pgpatch> (pgpatch/patch "pg_strncasecmp" (fn [a b n] n))   # returns its 3rd arg
```

`(pgpatch/patch-at addr fn)` / `(pgpatch/unpatch-at addr)` work by address (no
DWARF: the replacement's declared arity is forwarded as raw 64-bit words).

Patches are **per-backend** (copy-on-write in this process's memory); they don't
affect other backends, and they're gone when the backend exits.

---

## 7. How it works, and the edges

- **Patching** overwrites the target's first 16 bytes with an absolute jump to a
  hook that captures the arm64 argument registers (x0–x7, d0–d7) and forwards
  them, typed from DWARF, to your Janet function. It's "replace" semantics: the
  original body does **not** run.
- **Calling** marshals arguments with libffi according to the DWARF signature.
- **The guard** wraps every entry into foreign code: a Postgres `ereport()` is
  caught by `PG_TRY`, and a hard fault by a temporary signal handler; either
  becomes a Janet error.

Limitations worth knowing:

- macOS/arm64 only.
- Integer/pointer/float/double arguments and returns; **no struct-by-value**,
  and at most 8 integer + 8 FP register arguments.
- Inlined-away functions have no symbol to patch or call; and patching a symbol
  doesn't affect call sites where the compiler inlined it.
- A caught crash keeps the backend up, but its internal state may be undefined
  afterward — fine for a fuzzing loop; reconnect for anything you care about.
- `(print …)` output is captured and returned; `ffi/read`/`ffi/write` are *not*
  guarded — prefer `peek`/`poke`.

---

## 8. Roadmap: entry/exit hooks (not yet implemented)

Today patching is replace-only. A natural extension is **wrap** semantics —
run Janet logic on the way in (seeing/adjusting the arguments) and/or on the way
out (seeing/adjusting the return value), with the original body running in
between. Both reduce to one new capability: a *trampoline that calls through to
the original function*. See the design notes / discussion for what that entails
(prologue relocation and an executable trampoline), and why a call-through
wrapper — rather than return-address hijacking — is the right approach given
Postgres's `longjmp`-based error handling.
