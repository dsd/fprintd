BUILT_SOURCES = server-bindings.h
noinst_HEADERS = $(BUILT_SOURCES)

CLEANFILES = $(BUILT_SOURCES)
EXTRA_DIST = fprintd.xml

bin_PROGRAMS = fprintd

fprintd_SOURCES = main.c
fprintd_LDADD = $(GLIB_LIBS) $(DBUS_GLIB_LIBS) $(FPRINT_LIBS)
fprintd_CFLAGS = $(AM_CFLAGS) $(GLIB_CFLAGS) $(DBUS_GLIB_CFLAGS) $(FPRINT_CFLAGS)

server-bindings.h: fprintd.xml
	dbus-binding-tool --prefix=fprint --mode=glib-server $< --output=$@

