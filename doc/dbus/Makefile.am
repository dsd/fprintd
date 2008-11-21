
all : net.reactivated.Fprint.Manager.ref.xml net.reactivated.Fprint.Device.ref.xml

net.reactivated.Fprint.Manager.ref.xml : $(top_srcdir)/src/manager.xml $(top_srcdir)/doc/dbus/spec-to-docbook.xsl
	echo "<?xml version=\"1.0\"?>""<!DOCTYPE refentry PUBLIC \"-//OASIS//DTD DocBook XML V4.1.2//EN\" \"http://www.oasis-open.org/docbook/xml/4.1.2/docbookx.dtd\">" > $@
	$(XSLTPROC) $(top_srcdir)/doc/dbus/spec-to-docbook.xsl $< | tail -n +2 >> $@

net.reactivated.Fprint.Device.ref.xml : $(top_srcdir)/src/device.xml $(top_srcdir)/doc/dbus/spec-to-docbook.xsl
	echo "<?xml version=\"1.0\"?>""<!DOCTYPE refentry PUBLIC \"-//OASIS//DTD DocBook XML V4.1.2//EN\" \"http://www.oasis-open.org/docbook/xml/4.1.2/docbookx.dtd\">" > $@
	$(XSLTPROC) $(top_srcdir)/doc/dbus/spec-to-docbook.xsl $< | tail -n +2 >> $@

EXTRA_DIST = spec-to-docbook.xsl dbus-introspect-docs.dtd

clean-local :
	rm -f *~ *.ref.xml
