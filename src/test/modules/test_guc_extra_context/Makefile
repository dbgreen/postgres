# src/test/modules/test_guc_extra_context/Makefile

MODULE_big = test_guc_extra_context
OBJS = \
	$(WIN32RES) \
	test_guc_extra_context.o

PGFILEDESC = "test_guc_extra_context - test GUC_EXTRA_IS_CONTEXT feature"

TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_guc_extra_context
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
