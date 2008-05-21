
dbus_servicesdir = $(datadir)/dbus-1/system-services
dbus_services_in_files = net.reactivated.Fprint.service.in
dbus_services_DATA = $(dbus_services_in_files:.service.in=.service)

$(dbus_services_DATA): $(dbus_services_in_files)
	sed -e "s|\@LIBEXECDIR\@|$(libexecdir)|" $< > $@

dbus_confdir = $(sysconfdir)/dbus-1/system.d
dbus_conf_DATA = net.reactivated.Fprint.conf

polkitdir = $(datadir)/PolicyKit/policy
polkit_in_files = net.reactivated.fprint.device.policy.in

@INTLTOOL_POLICY_RULE@
polkit_DATA = $(polkit_in_files:.policy.in=.policy)

check:
	$(POLKIT_POLICY_FILE_VALIDATE) $(polkit_DATA)


