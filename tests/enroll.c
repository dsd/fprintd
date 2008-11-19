/*
 * fprintd example to enroll right index finger
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

#include <stdlib.h>
#include <dbus/dbus-glib-bindings.h>
#include "manager-dbus-glue.h"
#include "device-dbus-glue.h"

static DBusGProxy *manager = NULL;
static DBusGConnection *connection = NULL;

enum enroll_result {
	ENROLL_COMPLETE = 1,
	ENROLL_FAIL,
	ENROLL_PASS,
	ENROLL_RETRY = 100,
	ENROLL_RETRY_TOO_SHORT = 101,
	ENROLL_RETRY_CENTER_FINGER = 102,
	ENROLL_RETRY_REMOVE_FINGER = 103,
};

static const char *enroll_result_str(int result)
{
	switch (result) {
	case ENROLL_COMPLETE:
		return "Enroll completed.";
	case ENROLL_FAIL:
		return "Enroll failed :(";
	case ENROLL_PASS:
		return "Enroll stage passed. Please scan again for next stage.";
	case ENROLL_RETRY:
		return "Retry scan";
	case ENROLL_RETRY_TOO_SHORT:
		return "Swipe too short, please retry";
	case ENROLL_RETRY_CENTER_FINGER:
		return "Finger not centered, please retry";
	case ENROLL_RETRY_REMOVE_FINGER:
		return "Please remove finger and retry";
	default:
		return "Unknown";
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

static DBusGProxy *open_device(const char *username)
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

	if (!net_reactivated_Fprint_Device_claim(dev, username, &error))
		g_error("failed to claim device: %s", error->message);
	return dev;
}

static void enroll_result(GObject *object, int result, void *user_data)
{
	gboolean *enroll_completed = user_data;
	g_print("Enroll result: %s (%d)\n", enroll_result_str(result), result);
	if (result == ENROLL_COMPLETE || result == ENROLL_FAIL)
		*enroll_completed = TRUE;
}

static void do_enroll(DBusGProxy *dev)
{
	GError *error;
	gboolean enroll_completed = FALSE;

	dbus_g_proxy_add_signal(dev, "EnrollStatus", G_TYPE_INT, NULL);
	dbus_g_proxy_connect_signal(dev, "EnrollStatus", G_CALLBACK(enroll_result),
		&enroll_completed, NULL);

	g_print("Enrolling right index finger.\n");
	if (!net_reactivated_Fprint_Device_enroll_start(dev, "right-index-finger", &error))
		g_error("EnrollStart failed: %s", error->message);

	while (!enroll_completed)
		g_main_context_iteration(NULL, TRUE);

	dbus_g_proxy_disconnect_signal(dev, "EnrollStatus",
		G_CALLBACK(enroll_result), &enroll_completed);

	if (!net_reactivated_Fprint_Device_enroll_stop(dev, &error))
		g_error("VerifyStop failed: %s", error->message);
}

static void release_device(DBusGProxy *dev)
{
	GError *error = NULL;
	if (!net_reactivated_Fprint_Device_release(dev, &error))
		g_error("ReleaseDevice failed: %s", error->message);
}

int main(int argc, char **argv)
{
	GMainLoop *loop;
	DBusGProxy *dev;
	char *username;

	g_type_init();
	loop = g_main_loop_new(NULL, FALSE);
	create_manager();

	username = NULL;
	if (argc == 2)
		username = argv[1];
	dev = open_device(username);
	do_enroll(dev);
	release_device(dev);
	return 0;
}

