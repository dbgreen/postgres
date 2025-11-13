# src/test/modules/test_handles/Makefile

MODULE_big = test_handles
OBJS = handle_info.o

PROGRAM = handle_info
OBJS = handle_info.o

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_handles
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif