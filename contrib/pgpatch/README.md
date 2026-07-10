# pgpatch

Embed a [Janet](https://janet-lang.org) REPL in a live PostgreSQL backend to
explore and debug it from the inside: search its C functions, inspect their
signatures, **call** internal functions that have no SQL surface (great for
fuzzing), and **hot-patch** a function so a Janet function runs instead of the
original.

This is a from-scratch, non-Windows re-creation of an earlier Windows tool that
used `debug.dll` + trampolining. Janet was chosen for its REPL and because it
lets us build a guard around Postgres's `setjmp`/`longjmp` error handling — so a
Janet panic *or* a Postgres `ereport()` raised deep in a call you're probing is
caught and reported instead of taking down the backend.

> **Status:** first working slice. **Platform: macOS / arm64 only.** Requires a
> Postgres built with debug info and, for parameter introspection, a generated
> `.dSYM`.

## How it works

- Loaded into a backend (`LOAD 'pgpatch';` or `CREATE EXTENSION pgpatch;`), the
  module starts a dedicated thread serving a Janet REPL on a per-backend Unix
  socket at `/tmp/pgpatch/<pid>.sock`.
- One Janet VM is shared between the REPL thread and the backend's main thread,
  serialized by a mutex (`janet_vm_load`/`janet_vm_save` around every access).
  The REPL thread never calls Postgres internals.
- `(pgpatch/patch "int4pl" fn)` overwrites the target function's prologue with an
  arm64 absolute-jump stub (via `mach_vm_protect` copy-on-write, never mapping a
  page writable+executable at once — no special entitlements needed). The
  replacement runs on the main thread when the code path fires, behind a guard
  that turns a Janet error into a clean `ereport(ERROR)` and keeps the backend
  alive.

## Try it

```sql
-- In psql (this backend will be the one we patch):
CREATE EXTENSION pgpatch;
SELECT pgpatch_socket_path();   -- e.g. /tmp/pgpatch/12345.sock
SELECT 2 + 2;                   -- 4
```

```
# In another terminal, attach to that backend's REPL:
$ nc -U /tmp/pgpatch/12345.sock
pgpatch> (pgpatch/search "int4" 5)          # find functions in the live process
pgpatch> (pgpatch/params "heap_form_tuple") # inspect a C signature (needs .dSYM)
pgpatch> (defn always42 [&] 42)             # int4pl is fmgr: its C arg is fcinfo
pgpatch> (pgpatch/patch "int4pl" always42)  # so the replacement just returns a Datum
```

```sql
-- Back in the first psql session:
SELECT 2 + 2;                   -- 42
```

```
pgpatch> (pgpatch/unpatch "int4pl")
```

```sql
SELECT 2 + 2;                   -- 4 again
```

## Calling internal functions (fuzzing)

`(pgpatch/call name & args)` invokes any resolvable C function — including
static / non-exported internals — with up to 8 integer/pointer arguments and
returns its result:

```
pgpatch> (pgpatch/call "pg_strncasecmp" "Hello" "HELLO" 5)   # => 0
pgpatch> (pgpatch/call "pg_strtoint32" "42")                 # => 42
```

Argument and return **types come from the function's DWARF signature** (via
libffi), so `double`/`float`, exact integer widths, and pointers are handled
correctly:

```
pgpatch> (pgpatch/call "clamp_row_est" 3.7)   # double -> double  => 4
```

When no DWARF is available, arguments fall back to integer/pointer class
(numbers → machine word; strings/buffers → pointer; `nil` → NULL; booleans →
0/1). Struct-by-value arguments/returns are not supported.

The call is guarded, so a bad input can't take down the backend. A Postgres
`ereport()` **and** a hard crash (SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT — e.g. an
`Assert` firing) are both caught and turned into a Janet error you can loop over:

```
pgpatch> (pgpatch/call "pg_strtoint32" "99999999999999999999")
# error: pgpatch/call: pg_strtoint32 raised: value "..." is out of range ...
pgpatch> (pgpatch/call "pg_strncasecmp" 0 0 5)
# error: pgpatch/call: pg_strncasecmp crashed (signal 11)
```

**Caveat:** after a *caught crash* the backend keeps running, but its internal
state may be undefined (the callee was interrupted mid-operation). Fine for a
fuzzing/exploration loop; reconnect for anything you care about.

**Calling internals must happen on the main thread** (it may touch backend
state and relies on `PG_exception_stack`). Drive it from SQL:
`SELECT pgpatch_eval('(pgpatch/call "pg_strtoint32" "42")');`. The REPL socket
runs on a worker thread and is best for defining, searching, and patching (and
calling genuinely pure functions).

## REPL bindings

- `(pgpatch/search pattern &opt limit)` — function symbols in the live process
  whose name contains `pattern`; returns `@[[name address] ...]`.
- `(pgpatch/params name)` — C signature (return type + parameter types/names)
  read from DWARF in the `.dSYM`.
- `(pgpatch/struct name)` — layout of a struct/union (or typedef of one) as a
  table `{:name :kind :size :fields @[{:offset :name :type} ...]}`. The
  offset/type pairs drive field reads (e.g. with Janet's `ffi/read`):

  ```
  pgpatch> (map |[($ :name) ($ :offset) ($ :type)]
                ((pgpatch/struct "HeapTupleData") :fields))
  @[("t_len" 0 "uint32") ("t_self" 4 "ItemPointerData")
    ("t_tableOid" 12 "Oid") ("t_data" 16 "HeapTupleHeader")]
  ```
- `(pgpatch/struct-str name)` — the same layout as a formatted string; view it
  with `(print (pgpatch/struct-str "HeapTupleData"))`:

  ```
  struct HeapTupleData {  /* 24 bytes */
    +0x0000  uint32 t_len;
    +0x0004  ItemPointerData t_self;
    +0x000c  Oid t_tableOid;
    +0x0010  HeapTupleHeader t_data;
  }
  ```
- `(pgpatch/find name)` — address of a symbol, or nil.
- `(pgpatch/call name & args)` — call a C function by name (DWARF-typed; main
  thread). `(pgpatch/call-at addr & args)` — same by address (integer or
  `"0x.."` string from `pgpatch/search`), integer/pointer inference.
- `(pgpatch/patch name fn)` — replace C function `name` with Janet `fn`. Args
  are typed and counted from the target's DWARF signature (including
  floats/doubles); without DWARF, `fn`'s arity is forwarded as raw words.
  `(pgpatch/patch-at addr fn)` — same by address (raw words).
- `(pgpatch/unpatch name)` / `(pgpatch/unpatch-at addr)` — restore the original.
- `(pgpatch/peek addr type)` / `(pgpatch/poke addr type value)` — read/write
  memory of `type` (`:s8 :u8 :s16 :u16 :s32 :u32 :s64 :u64 :float :double :ptr
  :string`, plus aliases `:int :uint :byte :long ...`) at `addr`.  Guarded, so
  a bad address raises a Janet error instead of crashing the backend.  Combine
  with `pgpatch/struct` to read live struct fields by name:

  ```
  # read CurrentMemoryContext->name from a live backend:
  (let [s     (pgpatch/struct "MemoryContextData")
        noff  ((first (filter |(= ($ :name) "name") (s :fields))) :offset)
        ctx   (pgpatch/peek (pgpatch/find "CurrentMemoryContext") :ptr)
        np    (pgpatch/peek (+ ctx noff) :ptr)]
    (pgpatch/peek np :string))            # => "ExprContext"
  ```
- `(pgpatch/set-dwarf path)` — point the DWARF reader at a specific
  `.dSYM/Contents/Resources/DWARF/<name>` file (otherwise derived from the
  running executable).

## SQL functions

- `pgpatch_eval(text) -> text` — evaluate Janet in this backend.
- `pgpatch_socket_path() -> text` — this backend's REPL socket path.

## Building / debug info

Build Postgres with debug info (meson `debugoptimized` already passes `-g`).
For `(pgpatch/params ...)`, generate a `.dSYM` next to the installed binary:

```
dsymutil <prefix>/bin/postgres
```

## Current limitations / next steps

- macOS/arm64 only; Linux (embedded ELF DWARF, no `.dSYM`) is future work.
- `pgpatch/call` uses libffi with types from DWARF, so integers (exact width),
  floats/doubles, and pointers all work. **Struct-by-value args/returns are not
  supported** (flagged and refused). Without a `.dSYM`, calls fall back to
  integer/pointer class only. (`pgpatch/patch` replacements still receive raw
  64-bit words — see below.)
- "Replace" semantics only (Janet called instead of the original). Calling
  through to the original ("wrap") needs prologue relocation of PC-relative
  instructions.
- For fmgr (`V1`) functions the sole C argument is the `FunctionCallInfo`, so a
  replacement receives that pointer, not the unpacked SQL arguments.
- Up to 16 concurrent patches (fixed hook pool). Patch hooks capture x0–x7 and
  d0–d7 and replay the AAPCS assignment from DWARF; >8 integer or >8 FP args (or
  struct-by-value) beyond the registers are not handled.
- `call-at`/`patch-at` have no name, so no DWARF: args are integer/pointer
  inferred and patch forwards the replacement's arity as raw words.
- The DWARF reader is a minimal DWARF-5/DWARF32 subset; it handles
  base/pointer/const/volatile/typedef/struct/union/enum/array types and
  `abstract_origin`/`specification`, but not the full standard.
