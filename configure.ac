AC_INIT([fprintd], [0.1])
AM_INIT_AUTOMAKE
AC_CONFIG_SRCDIR([src/main.c])
AC_CONFIG_HEADERS([config.h])

AC_PREREQ([2.50])
AC_PROG_LIBTOOL
AC_PROG_CC
AM_PROG_CC_C_O

GETTEXT_PACKAGE=fprintd
AC_DEFINE_UNQUOTED(GETTEXT_PACKAGE, ["$GETTEXT_PACKAGE"], [Define to the Gettext package name])
AC_SUBST(GETTEXT_PACKAGE)
AM_GLIB_GNU_GETTEXT
IT_PROG_INTLTOOL([0.35.0])

PKG_CHECK_MODULES(FPRINT, [libfprint > 0.1.0])
AC_SUBST(FPRINT_LIBS)
AC_SUBST(FPRINT_CFLAGS)

PKG_CHECK_MODULES(GLIB, glib-2.0 dbus-glib-1)
AC_SUBST(GLIB_CFLAGS)
AC_SUBST(GLIB_LIBS)

PKG_CHECK_MODULES(DAEMON, glib-2.0 dbus-glib-1 gmodule-2.0 polkit >= 0.8 polkit-dbus)
AC_SUBST(DAEMON_LIBS)
AC_SUBST(DAEMON_CFLAGS)

AC_ARG_ENABLE(pam, AC_HELP_STRING([--enable-pam],[Build the fprintd PAM module]), enable_pam="$enableval", enable_pam=yes)
has_pam=no
if test x$enable_pam = xyes; then
	has_pam=yes
	AC_CHECK_HEADER([security/pam_modules.h], [has_pam=yes] , [has_pam=no])
	if test x$has_pam = xyes; then
		has_pam=no
		AC_CHECK_LIB(pam, pam_start, [PAM_LIBS="-lpam"
					      has_pam=yes],
			has_pam=no)
	fi
	AC_SUBST(PAM_LIBS)
fi
AM_CONDITIONAL(HAVE_PAM, test "x$has_pam" = "xyes")

AC_MSG_CHECKING(for PAM headers and library)
AC_MSG_RESULT([$has_pam])


AC_CHECK_PROG([POLKIT_POLICY_FILE_VALIDATE],
	      [polkit-policy-file-validate], [polkit-policy-file-validate])

AC_PATH_PROG([XSLTPROC], [xsltproc])
GTK_DOC_CHECK([1.3])

AS_AC_EXPAND(DATADIR, $datadir)

DBUS_SERVICES_DIR="$DATADIR/dbus-1/services"
AC_SUBST(DBUS_SERVICES_DIR)
AC_DEFINE_UNQUOTED(DBUS_SERVICES_DIR, "$DBUS_SERVICES_DIR", [Where services dir for DBUS is])

AC_DEFINE_UNQUOTED(SYSCONFDIR, "$sysconfdir", [Where the configuration file will be located])

GNOME_COMPILE_WARNINGS

AC_OUTPUT([
Makefile
src/Makefile
data/Makefile
tests/Makefile
pam/Makefile
doc/Makefile
doc/version.xml
doc/dbus/Makefile
po/Makefile.in
])
