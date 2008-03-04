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

#include <stdlib.h>
#include <dbus/dbus-glib-bindings.h>
#include "client-bindings.h"

static DBusGProxy *proxy = NULL;
static DBusGConnection *connection = NULL;

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
	default:
		return "Unknown finger";
	}
}

static void create_proxy(void)
{
	GError *error = NULL;

	connection = dbus_g_bus_get(DBUS_BUS_SESSION, &error);
	if (connection == NULL)
		g_error("Failed to connect to session bus: %s", error->message);

	proxy = dbus_g_proxy_new_for_name_owner(connection,
		"net.reactivated.Fprint", "/net/reactivated/Fprint",
		"net.reactivated.Fprint", &error);
	if (proxy == NULL)
		g_error("Failed to create proxy: %s", error->message);
}

static guint32 open_device(void)
{
	GError *error = NULL;
	GArray *device_ids;
	char **drvnames;
	char **fullnames;
	guint i;
	guint32 device_id;

	if (!net_reactivated_Fprint_list_devices(proxy, &device_ids, &drvnames,
			&fullnames, &error))
		g_error("list_devices failed: %s", error->message);
	
	if (device_ids->len == 0) {
		g_print("No devices found\n");
		exit(1);
	}

	g_print("found %d devices\n", device_ids->len);
	for (i = 0; i < device_ids->len; i++) {
		g_print("Device #%d: %s (%s)\n", g_array_index(device_ids, guint32, i),
			fullnames[i], drvnames[i]);
	}

	device_id = g_array_index(device_ids, int, 0);
	g_strfreev(drvnames);
	g_strfreev(fullnames);
	g_array_free(device_ids, TRUE);

	g_print("using device #%d\n", device_id);
	if (!net_reactivated_Fprint_claim_device(proxy, device_id, &error))
		g_error("failed to claim device: %s", error->message);
	return device_id;
}

static guint32 find_finger(guint32 device_id)
{
	GError *error = NULL;
	GArray *fingers;
	guint i;
	int fingernum;
	guint32 print_id;

	if (!net_reactivated_Fprint_list_enrolled_fingers(proxy, 0, &fingers, &error))
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

	g_print("Verifying: %s\n", fingerstr(fingernum));
	if (!net_reactivated_Fprint_load_print_data(proxy, device_id, fingernum, &print_id, &error))
		g_error("LoadPrintData failed: %s", error->message);

	return print_id;
}

static int do_verify(guint32 device_id, guint32 print_id)
{
	GError *error;
	gboolean more_results;
	int result;

	if (!net_reactivated_Fprint_verify_start(proxy, device_id, print_id, &error))
		g_error("VerifyStart failed: %s", error->message);

	do {
		if (!net_reactivated_Fprint_get_verify_result(proxy, device_id, &result, &more_results, &error))
			g_error("GetVerifyResult failed: %s", error->message);

		g_print("Verify result: %s (%d)\n", verify_result_str(result), result);
	} while (result != VERIFY_NO_MATCH && result != VERIFY_MATCH);

	if (!net_reactivated_Fprint_verify_stop(proxy, device_id, &error))
		g_error("VerifyStop failed: %s", error->message);

	return result;
}

static void unload_print(guint32 device_id, guint32 print_id)
{
	GError *error = NULL;
	if (!net_reactivated_Fprint_unload_print_data(proxy, device_id, print_id, &error))
		g_error("UnloadPrint failed: %s", error->message);
}

static void release_device(guint32 device_id)
{
	GError *error = NULL;
	if (!net_reactivated_Fprint_release_device(proxy, device_id, &error))
		g_error("ReleaseDevice failed: %s", error->message);
}

int main(int argc, char **argv)
{
	GMainLoop *loop;
	guint32 device_id;
	guint32 print_id;
	int verify_result;

	g_type_init();
	loop = g_main_loop_new(NULL, FALSE);
	create_proxy();

	device_id = open_device();
	print_id = find_finger(device_id);
	verify_result = do_verify(device_id, print_id);
	unload_print(device_id, print_id);
	release_device(device_id);
	return 0;
}

