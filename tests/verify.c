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
static int finger_num = -1;
static gboolean g_fatal_warnings = FALSE;
static char **usernames = NULL;

enum fp_verify_result {
	VERIFY_NO_MATCH = 0,
	VERIFY_MATCH = 1,
	VERIFY_RETRY = 100,
	VERIFY_RETRY_TOO_SHORT = 101,
	VERIFY_RETRY_CENTER_FINGER = 102,
	VERIFY_RETRY_REMOVE_FINGER = 103,
};

static const char *verify_result_str(int result)
{
	switch (result) {
	case VERIFY_NO_MATCH:
		return "No match";
	case VERIFY_MATCH:
		return "Match!";
	case VERIFY_RETRY:
		return "Retry scan";
	case VERIFY_RETRY_TOO_SHORT:
		return "Swipe too short, please retry";
	case VERIFY_RETRY_CENTER_FINGER:
		return "Finger not centered, please retry";
	case VERIFY_RETRY_REMOVE_FINGER:
		return "Please remove finger and retry";
	default:
		return "Unknown";
	}
}

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
	case -1:
		return "First fingerprint available";
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

static void find_finger(DBusGProxy *dev, const char *username)
{
	GError *error = NULL;
	GArray *fingers;
	guint i;
	int fingernum;

	if (!net_reactivated_Fprint_Device_list_enrolled_fingers(dev, username, &fingers, &error))
		g_error("ListEnrolledFingers failed: %s", error->message);

	if (fingers->len == 0) {
		g_print("No fingers enrolled for this device.\n");
		exit(1);
	}

	g_print("Listing enrolled fingers:\n");
	for (i = 0; i < fingers->len; i++) {
		fingernum = g_array_index(fingers, guint32, i);
		g_print(" - #%d: %s\n", fingernum, fingerstr(fingernum));
	}

	fingernum = g_array_index(fingers, guint32, 0);
	g_array_free(fingers, TRUE);
}

static void verify_result(GObject *object, int result, void *user_data)
{
	gboolean *verify_completed = user_data;
	g_print("Verify result: %s (%d)\n", verify_result_str(result), result);
	if (result == VERIFY_NO_MATCH || result == VERIFY_MATCH)
		*verify_completed = TRUE;
}

static void verify_finger_selected(GObject *object, int finger, void *user_data)
{
	g_print("Verifying: %s\n", fingerstr(finger));
}

static void do_verify(DBusGProxy *dev)
{
	GError *error;
	gboolean verify_completed = FALSE;

	dbus_g_proxy_add_signal(dev, "VerifyStatus", G_TYPE_INT, NULL);
	dbus_g_proxy_add_signal(dev, "VerifyFingerSelected", G_TYPE_INT, NULL);
	dbus_g_proxy_connect_signal(dev, "VerifyStatus", G_CALLBACK(verify_result),
		&verify_completed, NULL);
	dbus_g_proxy_connect_signal(dev, "VerifyFingerSelected", G_CALLBACK(verify_finger_selected),
		NULL, NULL);

	if (!net_reactivated_Fprint_Device_verify_start(dev, finger_num, &error))
		g_error("VerifyStart failed: %s", error->message);

	while (!verify_completed)
		g_main_context_iteration(NULL, TRUE);

	dbus_g_proxy_disconnect_signal(dev, "VerifyStatus", G_CALLBACK(verify_result), &verify_completed);
	dbus_g_proxy_disconnect_signal(dev, "VerifyFingerSelected", G_CALLBACK(verify_finger_selected), NULL);

	if (!net_reactivated_Fprint_Device_verify_stop(dev, &error))
		g_error("VerifyStop failed: %s", error->message);
}

static void release_device(DBusGProxy *dev)
{
	GError *error = NULL;
	if (!net_reactivated_Fprint_Device_release(dev, &error))
		g_error("ReleaseDevice failed: %s", error->message);
}

static const GOptionEntry entries[] = {
	{ "finger", 'f',  0, G_OPTION_ARG_INT, &finger_num, "Finger selected to verify (default is automatic)", NULL },
	{"g-fatal-warnings", 0, 0, G_OPTION_ARG_NONE, &g_fatal_warnings, "Make all warnings fatal", NULL},
 	{ G_OPTION_REMAINING, '\0', 0, G_OPTION_ARG_STRING_ARRAY, &usernames, NULL, "[username]" },
	{ NULL }
};

int main(int argc, char **argv)
{
	GOptionContext *context;
	GMainLoop *loop;
	GError *err = NULL;
	DBusGProxy *dev;
	char *username;

	g_type_init();

	context = g_option_context_new ("Verify a fingerprint");
	g_option_context_add_main_entries (context, entries, NULL);

	if (g_option_context_parse (context, &argc, &argv, &err) == FALSE) {
		g_print ("couldn't parse command-line options: %s\n", err->message);
		g_error_free (err);
		return 1;
	}

	if (usernames == NULL) {
		username = "";
	} else {
		username = usernames[0];
	}

	if (g_fatal_warnings) {
		GLogLevelFlags fatal_mask;

		fatal_mask = g_log_set_always_fatal (G_LOG_FATAL_MASK);
		fatal_mask |= G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL;
		g_log_set_always_fatal (fatal_mask);
	}

	loop = g_main_loop_new(NULL, FALSE);
	create_manager();

	dev = open_device(username);
	find_finger(dev, username);
	do_verify(dev);
	release_device(dev);
	return 0;
}

