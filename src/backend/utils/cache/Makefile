#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for utils/cache
#
# IDENTIFICATION
#    src/backend/utils/cache/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/utils/cache
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = attoptcache.o catcache.o evtcache.o inval.o \
	dispatchcat.o pickcat.o lsyscache.o \
	partcache.o plancache.o relcache.o relmapper.o relfilenodemap.o \
	spccache.o syscache.o ts_cache.o typcache.o

include $(top_srcdir)/src/backend/common.mk
