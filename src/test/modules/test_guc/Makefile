MODULE_big = test_guc
OBJS = \
	$(WIN32RES) \
	test_guc.o

EXTENSION = test_guc
DATA = test_guc--1.0.sql
PGFILEDESC = "test_guc - test module for GUC_EXTRA_IS_CONTEXT"

TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_guc
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
