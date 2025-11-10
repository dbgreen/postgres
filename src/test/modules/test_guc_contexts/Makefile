# Makefile for test_guc_contexts

MODULE_big = test_guc_contexts
OBJS = test_guc_contexts.o
PGFILEDESC = "test_guc_contexts - test GUC_EXTRA_IS_CONTEXT"

EXTENSION = test_guc_contexts
DATA = test_guc_contexts--1.0.sql

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_guc_contexts
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
