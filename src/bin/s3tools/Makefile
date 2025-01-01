PGFILEDESC = "s3tool - list/clear s3"

subdir = src/bin/s3tools
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS= s3functions.o

LIBS += -laws-cpp-sdk-core -laws-cpp-sdk-s3 -laws-crt-cpp -laws-c-cal -laws-c-auth -laws-c-common -laws-c-event-stream

all: clearbucket

clearbucket: clearbucket.o $(OBJS)
	$(CXX) $(CFLAGS) clearbucket.o $(OBJS) $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

install: all installdirs
	$(INSTALL_PROGRAM) clearbucket$$(X) '$(DESTDIR)$(bindir)/clearbucket$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/clearbucket$(X)'

clean distclean maintainer-clean:
	rm -f clearbucket$(X) clearbucket.o s3functions.o
