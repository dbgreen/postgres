# src/test/modules/test_guc_context/Makefile

MODULE_big = test_guc_context
OBJS = \
	$(WIN32RES) \
	test_guc_context.o

EXTENSION = test_guc_context
DATA = test_guc_context--1.0.sql
PGFILEDESC = "test_guc_context - test GUC_EXTRA_IS_CONTEXT functionality"

TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_guc_context
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
