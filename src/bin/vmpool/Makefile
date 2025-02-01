PGFILEDESC = "vmpool - manage vm pool"

subdir = src/bin/vmpool
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global


all:

install: installdirs
	$(INSTALL_PROGRAM) pool.py '$(DESTDIR)$(bindir)/pool'
	$(INSTALL_PROGRAM) pool_start.py '$(DESTDIR)$(bindir)/pool_start'
	$(INSTALL_PROGRAM) pool_stop.py '$(DESTDIR)$(bindir)/pool_stop'
	$(INSTALL_PROGRAM) pool_init_instance.py '$(DESTDIR)$(bindir)/pool_init_instance'
	$(INSTALL_PROGRAM) pool_init.py '$(DESTDIR)$(bindir)/pool_init'
	$(INSTALL_PROGRAM) pool_remove_instance.py '$(DESTDIR)$(bindir)/pool_remove_instance'
	$(INSTALL_PROGRAM) pool_destroy.py '$(DESTDIR)$(bindir)/pool_destroy'
	$(INSTALL_PROGRAM) pool_make_conf.py '$(DESTDIR)$(bindir)/pool_make_conf'
	$(INSTALL_PROGRAM) cs_init.py '$(DESTDIR)$(bindir)/cs_init'
	$(INSTALL_PROGRAM) remote_command.py '$(DESTDIR)$(bindir)/remote_command.py'
	$(INSTALL_PROGRAM) pool_keeper_util.py '$(DESTDIR)$(bindir)/pool_keeper_util.py'
	$(INSTALL_PROGRAM) pg_internal.init '$(DESTDIR)$(datarootdir)/pg_internal.init'
	$(INSTALL_PROGRAM) pg_internal.init.global '$(DESTDIR)$(datarootdir)/pg_internal.init.global'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/pool(X)'
	rm -f '$(DESTDIR)$(bindir)/pool_start(X)'
	rm -f '$(DESTDIR)$(bindir)/pool_stop(X)'
	rm -f '$(DESTDIR)$(bindir)/pool_init_instance(X)'
	rm -f '$(DESTDIR)$(bindir)/pool_init(X)'
	rm -f '$(DESTDIR)$(bindir)/pool_remove_instance(X)'
	rm -f '$(DESTDIR)$(bindir)/pool_destroy(X)'
	rm -f '$(DESTDIR)$(bindir)/pool_make_conf(X)'
	rm -f '$(DESTDIR)$(bindir)/cs_init(X)'
	rm -f '$(DESTDIR)$(bindir)/remote_command.py(X)'
	rm -f '$(DESTDIR)$(bindir)/pool_keeper_util.py(X)'
	rm -f '$(DESTDIR)$(datarootdir)/pg_internal.init(X)'
	rm -f '$(DESTDIR)$(datarootdir)/pg_internal.init.global(X)'

