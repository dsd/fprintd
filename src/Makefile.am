BUILT_SOURCES = manager-dbus-glue.h device-dbus-glue.h
noinst_HEADERS = $(BUILT_SOURCES)

CLEANFILES = $(BUILT_SOURCES)
EXTRA_DIST = fprintd.xml

libexec_PROGRAMS = fprintd

fprintd_SOURCES = main.c manager.c device.c file_storage.c
fprintd_LDADD = $(DBUS_GLIB_LIBS) $(FPRINT_LIBS) $(POLKIT_LIBS)
fprintd_CFLAGS = $(AM_CFLAGS) $(DBUS_GLIB_CFLAGS) $(FPRINT_CFLAGS) $(POLKIT_CFLAGS) -DLOCALEDIR=\""$(datadir)/locale"\"

manager-dbus-glue.h: manager.xml
	dbus-binding-tool --prefix=fprint_manager --mode=glib-server $< --output=$@

device-dbus-glue.h: device.xml
	dbus-binding-tool --prefix=fprint_device --mode=glib-server $< --output=$@

