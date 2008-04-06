BUILT_SOURCES = manager-dbus-glue.h device-dbus-glue.h
noinst_HEADERS = $(BUILT_SOURCES)
CLEANFILES = $(BUILT_SOURCES)

bin_PROGRAMS = verify enroll

verify_SOURCES = verify.c
verify_CFLAGS = $(AM_CFLAGS) $(DBUS_GLIB_CFLAGS)
verify_LDADD = $(DBUS_GLIB_LIBS)

enroll_SOURCES = enroll.c
enroll_CFLAGS = $(AM_CFLAGS) $(DBUS_GLIB_CFLAGS)
enroll_LDADD = $(DBUS_GLIB_LIBS)

manager-dbus-glue.h: ../src/manager.xml
	dbus-binding-tool --prefix=fprint_manager --mode=glib-client $< --output=$@

device-dbus-glue.h: ../src/device.xml
	dbus-binding-tool --prefix=fprint_device --mode=glib-client $< --output=$@
