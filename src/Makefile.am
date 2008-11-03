BUILT_SOURCES = manager-dbus-glue.h device-dbus-glue.h
noinst_HEADERS = $(BUILT_SOURCES)

CLEANFILES = $(BUILT_SOURCES)
EXTRA_DIST = fprintd.xml

libexec_PROGRAMS = fprintd

fprintd_SOURCES = main.c manager.c device.c file_storage.c egg-dbus-monitor.c egg-dbus-monitor.h
fprintd_LDADD = $(FPRINT_LIBS) $(DAEMON_LIBS)
fprintd_CFLAGS = $(WARN_CFLAGS) $(FPRINT_CFLAGS) $(DAEMON_CFLAGS) -DLOCALEDIR=\""$(datadir)/locale"\" -DPLUGINDIR=\""$(libdir)/fprintd/modules"\"

manager-dbus-glue.h: manager.xml
	dbus-binding-tool --prefix=fprint_manager --mode=glib-server $< --output=$@

device-dbus-glue.h: device.xml
	dbus-binding-tool --prefix=fprint_device --mode=glib-server $< --output=$@

