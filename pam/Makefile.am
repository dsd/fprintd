if HAVE_PAM

pammod_PROGRAMS = pam_fprintd.so
pammoddir=$(libdir)/security

pam_fprintd_so_SOURCES = pam_fprintd.c $(MARSHALFILES)
pam_fprintd_so_CFLAGS = -fPIC $(WARN_CFLAGS) $(GLIB_CFLAGS)
pam_fprintd_so_LDFLAGS = -shared
pam_fprintd_so_LDADD = $(PAM_LIBS) $(GLIB_LIBS)

MARSHALFILES = marshal.c marshal.h
GLIB_GENMARSHAL=`pkg-config --variable=glib_genmarshal glib-2.0`
BUILT_SOURCES = $(MARSHALFILES)

marshal.h: $(top_srcdir)/src/fprintd-marshal.list
	( $(GLIB_GENMARSHAL) --prefix=fprintd_marshal $(top_srcdir)/src/fprintd-marshal.list --header > marshal.h )
marshal.c: marshal.h
	( $(GLIB_GENMARSHAL) --prefix=fprintd_marshal $(top_srcdir)/src/fprintd-marshal.list --body --header > marshal.c )

endif

EXTRA_DIST = pam_fprintd.c fingerprint-strings.h
