AC_INIT([fprintd], [0.1])
AM_INIT_AUTOMAKE
AC_CONFIG_SRCDIR([src/main.c])
AC_CONFIG_HEADERS([config.h])

AC_PREREQ([2.50])
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

AC_CHECK_PROG([POLKIT_POLICY_FILE_VALIDATE],
	      [polkit-policy-file-validate], [polkit-policy-file-validate])

AS_AC_EXPAND(DATADIR, $datadir)

DBUS_SERVICES_DIR="$DATADIR/dbus-1/services"
AC_SUBST(DBUS_SERVICES_DIR)
AC_DEFINE_UNQUOTED(DBUS_SERVICES_DIR, "$DBUS_SERVICES_DIR", [Where services dir for DBUS is])

AC_DEFINE_UNQUOTED(SYSCONFDIR, "$sysconfdir", [Where the configuration file will be located])

# Restore gnu89 inline semantics on gcc 4.3 and newer
saved_cflags="$CFLAGS"
CFLAGS="$CFLAGS -fgnu89-inline"
AC_COMPILE_IFELSE(AC_LANG_PROGRAM([]), inline_cflags="-fgnu89-inline", inline_cflags="")
CFLAGS="$saved_cflags"

AM_CFLAGS="-std=gnu99 $inline_cflags -Wall -Wundef -Wunused -Wstrict-prototypes -Werror-implicit-function-declaration -Wno-pointer-sign -Wshadow"
AC_SUBST(AM_CFLAGS)

AC_CONFIG_FILES([Makefile] [src/Makefile] [data/Makefile] [tests/Makefile] [po/Makefile.in])
AC_OUTPUT

