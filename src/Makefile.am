BUILT_SOURCES = manager-dbus-glue.h device-dbus-glue.h
noinst_HEADERS = $(BUILT_SOURCES)

CLEANFILES = $(BUILT_SOURCES)
EXTRA_DIST = fprintd.xml

bin_PROGRAMS = fprintd

fprintd_SOURCES = main.c manager.c device.c
fprintd_LDADD = $(GLIB_LIBS) $(DBUS_GLIB_LIBS) $(FPRINT_LIBS)
fprintd_CFLAGS = $(AM_CFLAGS) $(GLIB_CFLAGS) $(DBUS_GLIB_CFLAGS) $(FPRINT_CFLAGS)

manager-dbus-glue.h: manager.xml
	dbus-binding-tool --prefix=fprint_manager --mode=glib-server $< --output=$@

device-dbus-glue.h: device.xml
	dbus-binding-tool --prefix=fprint_device --mode=glib-server $< --output=$@

