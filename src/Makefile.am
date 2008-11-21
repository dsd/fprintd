BUILT_SOURCES = manager-dbus-glue.h device-dbus-glue.h $(MARSHALFILES)
noinst_HEADERS = $(BUILT_SOURCES)

CLEANFILES = $(BUILT_SOURCES)
EXTRA_DIST = manager.xml device.xml fprintd-marshal.list

libexec_PROGRAMS = fprintd

fprintd_SOURCES =				\
	main.c fprintd.h			\
	manager.c				\
	device.c				\
	file_storage.c file_storage.h storage.h	\
	egg-dbus-monitor.c egg-dbus-monitor.h	\
	$(MARSHALFILES)

fprintd_LDADD = $(FPRINT_LIBS) $(DAEMON_LIBS)
fprintd_CFLAGS = $(WARN_CFLAGS) $(FPRINT_CFLAGS) $(DAEMON_CFLAGS) -DLOCALEDIR=\""$(datadir)/locale"\" -DPLUGINDIR=\""$(libdir)/fprintd/modules"\"

manager-dbus-glue.h: manager.xml
	dbus-binding-tool --prefix=fprint_manager --mode=glib-server $< --output=$@

device-dbus-glue.h: device.xml
	dbus-binding-tool --prefix=fprint_device --mode=glib-server $< --output=$@

MARSHALFILES = fprintd-marshal.c fprintd-marshal.h
GLIB_GENMARSHAL=`pkg-config --variable=glib_genmarshal glib-2.0`

fprintd-marshal.h: fprintd-marshal.list
	( $(GLIB_GENMARSHAL) --prefix=fprintd_marshal $(srcdir)/fprintd-marshal.list --header > fprintd-marshal.h )
fprintd-marshal.c: fprintd-marshal.h
	( $(GLIB_GENMARSHAL) --prefix=fprintd_marshal $(srcdir)/fprintd-marshal.list --body --header > fprintd-marshal.c )

