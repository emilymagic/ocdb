#-------------------------------------------------------------------------
#
# Makefile for src/test/perl
#
# Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/test/perl/Makefile
#
#-------------------------------------------------------------------------

subdir = src/test/perl
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

ifeq ($(enable_tap_tests),yes)

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(pgxsdir)/$(subdir)/PostgreSQL/Test'

install: all installdirs
<<<<<<< HEAD
	$(INSTALL_DATA) $(srcdir)/PostgreSQL/Test/Utils.pm '$(DESTDIR)$(pgxsdir)/$(subdir)/PostgreSQL/Test/Utils.pm'
	$(INSTALL_DATA) $(srcdir)/PostgreSQL/Test/SimpleTee.pm '$(DESTDIR)$(pgxsdir)/$(subdir)/PostgreSQL/Test/SimpleTee.pm'
	$(INSTALL_DATA) $(srcdir)/PostgreSQL/Test/RecursiveCopy.pm '$(DESTDIR)$(pgxsdir)/$(subdir)/PostgreSQL/Test/RecursiveCopy.pm'
	$(INSTALL_DATA) $(srcdir)/PostgreSQL/Test/Cluster.pm '$(DESTDIR)$(pgxsdir)/$(subdir)/PostgreSQL/Test/Cluster.pm'
	$(INSTALL_DATA) $(srcdir)/PostgreSQL/Test/BackgroundPsql.pm '$(DESTDIR)$(pgxsdir)/$(subdir)/PostgreSQL/Test/BackgroundPsql.pm'
	$(INSTALL_DATA) $(srcdir)/PostgreSQL/Version.pm '$(DESTDIR)$(pgxsdir)/$(subdir)/PostgreSQL/Version.pm'

uninstall:
	rm -f '$(DESTDIR)$(pgxsdir)/$(subdir)/PostgreSQL/Test/Utils.pm'
	rm -f '$(DESTDIR)$(pgxsdir)/$(subdir)/PostgreSQL/Test/SimpleTee.pm'
	rm -f '$(DESTDIR)$(pgxsdir)/$(subdir)/PostgreSQL/Test/RecursiveCopy.pm'
	rm -f '$(DESTDIR)$(pgxsdir)/$(subdir)/PostgreSQL/Test/Cluster.pm'
	rm -f '$(DESTDIR)$(pgxsdir)/$(subdir)/PostgreSQL/Test/BackgroundPsql.pm'
	rm -f '$(DESTDIR)$(pgxsdir)/$(subdir)/PostgreSQL/Version.pm'
=======
	$(INSTALL_DATA) $(srcdir)/TestLib.pm '$(DESTDIR)$(pgxsdir)/$(subdir)/TestLib.pm'
	$(INSTALL_DATA) $(srcdir)/SimpleTee.pm '$(DESTDIR)$(pgxsdir)/$(subdir)/SimpleTee.pm'
	$(INSTALL_DATA) $(srcdir)/RecursiveCopy.pm '$(DESTDIR)$(pgxsdir)/$(subdir)/RecursiveCopy.pm'
	$(INSTALL_DATA) $(srcdir)/PostgresNode.pm '$(DESTDIR)$(pgxsdir)/$(subdir)/PostgresNode.pm'
	$(INSTALL_DATA) $(srcdir)/PostgreSQL/Test/Cluster.pm '$(DESTDIR)$(pgxsdir)/$(subdir)/PostgreSQL/Test/Cluster.pm'
	$(INSTALL_DATA) $(srcdir)/PostgreSQL/Test/Utils.pm '$(DESTDIR)$(pgxsdir)/$(subdir)/PostgreSQL/Test/Utils.pm'

uninstall:
	rm -f '$(DESTDIR)$(pgxsdir)/$(subdir)/TestLib.pm'
	rm -f '$(DESTDIR)$(pgxsdir)/$(subdir)/SimpleTee.pm'
	rm -f '$(DESTDIR)$(pgxsdir)/$(subdir)/RecursiveCopy.pm'
	rm -f '$(DESTDIR)$(pgxsdir)/$(subdir)/PostgresNode.pm'
	rm -f '$(DESTDIR)$(pgxsdir)/$(subdir)/PostgreSQL/Test/Cluster.pm'
	rm -f '$(DESTDIR)$(pgxsdir)/$(subdir)/PostgreSQL/Test/Utils.pm'
>>>>>>> merge_pg12

endif
