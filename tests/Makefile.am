BUILT_SOURCES = manager-dbus-glue.h device-dbus-glue.h $(MARSHALFILES)
noinst_HEADERS = $(BUILT_SOURCES)
CLEANFILES = $(BUILT_SOURCES)

bin_PROGRAMS = fprintd-verify fprintd-enroll fprintd-list fprintd-delete

fprintd_verify_SOURCES = verify.c $(MARSHALFILES)
fprintd_verify_CFLAGS = $(WARN_CFLAGS) $(GLIB_CFLAGS)
fprintd_verify_LDADD = $(GLIB_LIBS)

fprintd_enroll_SOURCES = enroll.c $(MARSHALFILES)
fprintd_enroll_CFLAGS = $(WARN_CFLAGS) $(GLIB_CFLAGS)
fprintd_enroll_LDADD = $(GLIB_LIBS)

fprintd_list_SOURCES = list.c
fprintd_list_CFLAGS = $(WARN_CFLAGS) $(GLIB_CFLAGS)
fprintd_list_LDADD = $(GLIB_LIBS)

fprintd_delete_SOURCES = delete.c
fprintd_delete_CFLAGS = $(WARN_CFLAGS) $(GLIB_CFLAGS)
fprintd_delete_LDADD = $(GLIB_LIBS)

manager-dbus-glue.h: ../src/manager.xml
	dbus-binding-tool --prefix=fprint_manager --mode=glib-client $< --output=$@

device-dbus-glue.h: ../src/device.xml
	dbus-binding-tool --prefix=fprint_device --mode=glib-client $< --output=$@

MARSHALFILES = marshal.c marshal.h
GLIB_GENMARSHAL=`pkg-config --variable=glib_genmarshal glib-2.0`

marshal.h: $(top_srcdir)/src/fprintd-marshal.list
	( $(GLIB_GENMARSHAL) --prefix=fprintd_marshal $(top_srcdir)/src/fprintd-marshal.list --header > marshal.h )
marshal.c: marshal.h
	( $(GLIB_GENMARSHAL) --prefix=fprintd_marshal $(top_srcdir)/src/fprintd-marshal.list --body --header > marshal.c )
