#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for cdb (GPDB specific routines)
#
#-------------------------------------------------------------------------


subdir = src/backend/cdb
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global
override CPPFLAGS := -I$(libpq_srcdir) $(CPPFLAGS)

SUBDIRS := motion dispatcher endpoint


OBJS = cdbappendonlystorageformat.o \
       cdbappendonlystorageread.o cdbappendonlystoragewrite.o \
	   cdbbufferedappend.o cdbbufferedread.o \
	   cdbcat.o cdbcopy.o \
	   cdbfts.o \
	   cdbgroup.o \
	   cdbgroupingpaths.o \
	   cdbhash.o \
	   cdblegacyhash.o \
	   cdbllize.o cdblocaldistribxact.o \
	   cdbappendonlyxlog.o \
	   cdbmutate.o \
	   cdbpath.o cdbpathlocus.o cdbpathtoplan.o \
	   cdbpgdatabase.o \
	   cdbplan.o cdbpullup.o \
	   cdbrelsize.o \
	   cdbsetop.o cdbsreh.o cdbsrlz.o cdbsubplan.o cdbsubselect.o \
	   cdbtargeteddispatch.o cdbthreadlog.o \
	   cdbtimer.o \
	   cdbutil.o \
	   cdbvars.o cdbvarblock.o \
	   cdbcatalogfunc.o

ifeq ($(PORTNAME),cygwin)
.LIBPATTERNS := $(filter-out %.so,$(.LIBPATTERNS))
endif



ALLOBJS = $(OBJS) $(SUBDIROBJS)

include $(top_srcdir)/src/backend/common.mk
