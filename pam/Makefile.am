if HAVE_PAM

pammod_PROGRAMS = pam_fprintd.so
pammoddir=/lib/security

pam_fprintd_so_SOURCES = pam_fprintd.c
pam_fprintd_so_CFLAGS = -fPIC $(WARN_CFLAGS) $(GLIB_CFLAGS)
pam_fprintd_so_LDFLAGS = -shared
pam_fprintd_so_LDADD = $(PAM_LIBS) $(GLIB_LIBS)

endif

EXTRA_DIST = pam_fprint.c
