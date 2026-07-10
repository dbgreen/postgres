# contrib/pgpatch/Makefile

MODULE_big = pgpatch
OBJS = \
	src/pgpatch.o \
	src/janet_embed.o \
	src/repl.o \
	src/patch.o \
	src/trampoline_arm64.o \
	src/introspect.o \
	src/dwarf.o \
	vendor/janet/janet.o

EXTENSION = pgpatch
DATA = pgpatch--1.0.sql
PGFILEDESC = "pgpatch - Janet REPL that hot-patches backend functions"

PG_CPPFLAGS = -I$(srcdir)/src -I$(srcdir)/vendor/janet

# Janet uses pthreads; the trampoline uses mach APIs in libSystem; pgpatch/call
# uses libffi (in the macOS SDK) to invoke functions by their DWARF signature.
SHLIB_LINK += -lpthread -lffi

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pgpatch
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
