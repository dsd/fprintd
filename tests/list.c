/*
 * fprintd example to verify a fingerprint
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

enum fp_finger {
	LEFT_THUMB = 1, /** thumb (left hand) */
	LEFT_INDEX, /** index finger (left hand) */
	LEFT_MIDDLE, /** middle finger (left hand) */
	LEFT_RING, /** ring finger (left hand) */
	LEFT_LITTLE, /** little finger (left hand) */
	RIGHT_THUMB, /** thumb (right hand) */
	RIGHT_INDEX, /** index finger (right hand) */
	RIGHT_MIDDLE, /** middle finger (right hand) */
	RIGHT_RING, /** ring finger (right hand) */
	RIGHT_LITTLE, /** little finger (right hand) */
};

static const char *fingerstr(guint32 fingernum)
{
	switch (fingernum) {
	case LEFT_THUMB:
		return "Left thumb";
	case LEFT_INDEX:
		return "Left index finger";
	case LEFT_MIDDLE:
		return "Left middle finger";
	case LEFT_RING:
		return "Left ring finger";
	case LEFT_LITTLE:
		return "Left little finger";
	case RIGHT_THUMB:
		return "Right thumb";
	case RIGHT_INDEX:
		return "Right index finger";
	case RIGHT_MIDDLE:
		return "Right middle finger";
	case RIGHT_RING:
		return "Right ring finger";
	case RIGHT_LITTLE:
		return "Right little finger";
	default:
		return "Unknown finger";
	}
}

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

static DBusGProxy *open_device(void)
{
	GError *error = NULL;
	GPtrArray *devices;
	gchar *path;
	DBusGProxy *dev;
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

	path = g_ptr_array_index(devices, 0);
	g_print("Using device %s\n", path);

	/* FIXME use for_name_owner?? */
	dev = dbus_g_proxy_new_for_name(connection, "net.reactivated.Fprint",
		path, "net.reactivated.Fprint.Device");
	
	g_ptr_array_foreach(devices, (GFunc) g_free, NULL);
	g_ptr_array_free(devices, TRUE);

	return dev;
}

static void list_fingerprints(DBusGProxy *dev, const char *username)
{
	GError *error = NULL;
	GArray *fingers;
	GHashTable *props;
	guint i;
	int fingernum;

	if (!net_reactivated_Fprint_Device_list_enrolled_fingers(dev, username, &fingers, &error))
		g_error("ListEnrolledFingers failed: %s", error->message);

	if (fingers->len == 0) {
		g_print("User %s has no fingers enrolled for this device.\n", username);
		return;
	}

	if (!net_reactivated_Fprint_Device_get_properties(dev, &props, &error))
		g_error("GetProperties failed: %s", error->message);

	g_print("Fingerprints for user %s on %s:\n", username, (char *) g_hash_table_lookup (props, "Name"));
	for (i = 0; i < fingers->len; i++) {
		fingernum = g_array_index(fingers, guint32, i);
		g_print(" - #%d: %s\n", fingernum, fingerstr(fingernum));
	}

	fingernum = g_array_index(fingers, guint32, 0);
	g_array_free(fingers, TRUE);
	g_hash_table_destroy (props);
}

int main(int argc, char **argv)
{
	GMainLoop *loop;
	DBusGProxy *dev;
	guint32 i;

	g_type_init();
	loop = g_main_loop_new(NULL, FALSE);
	create_manager();

	if (argc < 2) {
		g_print ("Usage: %s <username> [usernames...]\n", argv[0]);
		return 1;
	}

	dev = open_device();
	for (i = 1; argv[i] != NULL; i++) {
		list_fingerprints (dev, argv[i]);
	}

	return 0;
}

