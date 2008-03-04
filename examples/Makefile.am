BUILT_SOURCES = client-bindings.h
noinst_HEADERS = $(BUILT_SOURCES)
CLEANFILES = $(BUILT_SOURCES)

bin_PROGRAMS = demo

demo_SOURCES = demo.c
demo_CFLAGS = $(AM_CFLAGS) $(DBUS_GLIB_CFLAGS)
demo_LDADD = $(DBUS_GLIB_LIBS)

client-bindings.h: ../src/fprintd.xml
	dbus-binding-tool --prefix=fprint --mode=glib-client $< --output=$@


