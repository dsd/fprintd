/*
 * fprintd example to delete fingerprints
 * Copyright (C) 2008 Daniel Drake <dsd@gentoo.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <dbus/dbus-glib-bindings.h>
#include "manager-dbus-glue.h"
#include "device-dbus-glue.h"

static DBusGProxy *manager = NULL;
static DBusGConnection *connection = NULL;

static void create_manager(void)
{
	GError *error = NULL;

	connection = dbus_g_bus_get(DBUS_BUS_SYSTEM, &error);
	if (connection == NULL)
		g_error("Failed to connect to session bus: %s", error->message);

	manager = dbus_g_proxy_new_for_name(connection,
		"net.reactivated.Fprint", "/net/reactivated/Fprint/Manager",
		"net.reactivated.Fprint.Manager");
}

static void delete_fingerprints(DBusGProxy *dev, const char *username)
{
	GError *error = NULL;
	GHashTable *props;
	DBusGProxy *p;

	p = dbus_g_proxy_new_from_proxy(dev, "org.freedesktop.DBus.Properties", NULL);
	if (!dbus_g_proxy_call (p, "GetAll", &error, G_TYPE_STRING, "net.reactivated.Fprint.Device", G_TYPE_INVALID,
			   dbus_g_type_get_map ("GHashTable", G_TYPE_STRING, G_TYPE_VALUE), &props, G_TYPE_INVALID))
		g_error("GetAll on the Properties interface failed: %s", error->message);

	if (!net_reactivated_Fprint_Device_delete_enrolled_fingers(dev, username, &error)) {
		if (dbus_g_error_has_name (error, "net.reactivated.Fprint.Error.NoEnrolledPrints") == FALSE)
			g_error("ListEnrolledFingers failed: %s", error->message);
		else
			g_print ("No fingerprints to delete on %s\n", g_value_get_string (g_hash_table_lookup (props, "name")));
	} else {
			g_print ("Fingerprints deleted on %s\n", g_value_get_string (g_hash_table_lookup (props, "name")));
	}
	g_hash_table_destroy (props);
	g_object_unref (p);
}

static void process_devices(char **argv)
{
	GError *error = NULL;
	GPtrArray *devices;
	char *path;
	guint i;

	if (!net_reactivated_Fprint_Manager_get_devices(manager, &devices, &error))
		g_error("list_devices failed: %s", error->message);
	
	if (devices->len == 0) {
		g_print("No devices found\n");
		exit(1);
	}

	g_print("found %d devices\n", devices->len);
	for (i = 0; i < devices->len; i++) {
		path = g_ptr_array_index(devices, i);
		g_print("Device at %s\n", path);
	}

	for (i = 0; i < devices->len; i++) {
		guint j;
		DBusGProxy *dev;

		path = g_ptr_array_index(devices, 0);
		g_print("Using device %s\n", path);

		/* FIXME use for_name_owner?? */
		dev = dbus_g_proxy_new_for_name(connection, "net.reactivated.Fprint",
						path, "net.reactivated.Fprint.Device");

		for (j = 1; argv[j] != NULL; j++)
			delete_fingerprints (dev, argv[j]);

		g_object_unref (dev);
	}
	
	g_ptr_array_foreach(devices, (GFunc) g_free, NULL);
	g_ptr_array_free(devices, TRUE);
}

int main(int argc, char **argv)
{
	GMainLoop *loop;

	g_type_init();
	loop = g_main_loop_new(NULL, FALSE);
	create_manager();

	if (argc < 2) {
		g_print ("Usage: %s <username> [usernames...]\n", argv[0]);
		return 1;
	}

	process_devices (argv);

	return 0;
}

