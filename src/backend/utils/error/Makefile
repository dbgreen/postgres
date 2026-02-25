#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/error
#
# IDENTIFICATION
#    src/backend/utils/error/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/error
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	assert.o \
	csvlog.o \
	elog.o \
	jsonlog.o

ifeq ($(PORTNAME), win32)
OBJS += nls_mo_reader.o
endif

include $(top_srcdir)/src/backend/common.mk
