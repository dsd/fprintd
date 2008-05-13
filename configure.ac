AC_INIT([fprintd], [0.1])
AM_INIT_AUTOMAKE
AC_CONFIG_SRCDIR([src/main.c])

AC_PREREQ([2.50])
AC_PROG_CC
AM_PROG_CC_C_O

PKG_CHECK_MODULES(FPRINT, "libfprint")
AC_SUBST(FPRINT_LIBS)
AC_SUBST(FPRINT_CFLAGS)

PKG_CHECK_MODULES(GLIB, "glib-2.0")
AC_SUBST(GLIB_CFLAGS)
AC_SUBST(GLIB_LIBS)

PKG_CHECK_MODULES(DBUS_GLIB, "dbus-glib-1")
AC_SUBST(DBUS_GLIB_LIBS)
AC_SUBST(DBUS_GLIB_CFLAGS)

AS_AC_EXPAND(DATADIR, $datadir)

DBUS_SERVICES_DIR="$DATADIR/dbus-1/services"
AC_SUBST(DBUS_SERVICES_DIR)
AC_DEFINE_UNQUOTED(DBUS_SERVICES_DIR, "$DBUS_SERVICES_DIR", [Where services dir for DBUS is])

# Restore gnu89 inline semantics on gcc 4.3 and newer
saved_cflags="$CFLAGS"
CFLAGS="$CFLAGS -fgnu89-inline"
AC_COMPILE_IFELSE(AC_LANG_PROGRAM([]), inline_cflags="-fgnu89-inline", inline_cflags="")
CFLAGS="$saved_cflags"

AM_CFLAGS="-std=gnu99 $inline_cflags -Wall -Wundef -Wunused -Wstrict-prototypes -Werror-implicit-function-declaration -Wno-pointer-sign -Wshadow"
AC_SUBST(AM_CFLAGS)

AC_CONFIG_FILES([Makefile] [src/Makefile] [data/Makefile] [tests/Makefile])
AC_OUTPUT

