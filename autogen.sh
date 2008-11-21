#!/bin/sh
aclocal || exit 1
autoheader || exit 1
glib-gettextize -f -c || exit 1
gtkdocize --copy || exit 1
intltoolize -c -f || exit 1
libtoolize -c || exit 1
autoconf || exit 1
automake -a -c || exit 1
./configure $*
