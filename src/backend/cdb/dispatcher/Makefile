#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for dispatcher(CDB2)
#
#-------------------------------------------------------------------------

subdir = src/backend/cdb/dispatcher
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS += -I$(libpq_srcdir) -I$(top_srcdir)/src/port -I$(top_srcdir)/src/backend/utils/misc

OBJS = cdbconn.o cdbdisp.o cdbdisp_async.o cdbdispatchresult.o cdbdisp_query.o cdbgang.o cdbgang_async.o cdbpq.o
include $(top_srcdir)/src/backend/common.mk
