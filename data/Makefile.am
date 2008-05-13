
dbus_servicesdir = $(datadir)/dbus-1/system-services
dbus_confdir = $(sysconfdir)/dbus-1/system.d

dbus_services_in_files = net.reactivated.Fprint.service.in

dbus_services_DATA = $(dbus_services_in_files:.service.in=.service)

$(dbus_services_DATA): $(dbus_services_in_files)
	sed -e "s|\@LIBEXECDIR\@|$(libexecdir)|" $< > $@

dbus_conf_DATA = net.reactivated.Fprint.conf



