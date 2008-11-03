/*
 * pam_fprint: PAM module for fingerprint authentication through fprintd
 * Copyright (C) 2007 Daniel Drake <dsd@gentoo.org>
 * Copyright (C) 2008 Bastien Nocera <hadess@hadess.net>
 * 
 * Experimental code. This will be moved out of fprintd into it's own
 * package once the system has matured.
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
#include <unistd.h>
#include <sys/types.h>
#include <string.h>

#include <dbus/dbus-glib-bindings.h>

#define PAM_SM_AUTH
#include <security/pam_modules.h>

#define MAX_TRIES 3
#define TIMEOUT 30

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

static gboolean send_info_msg(pam_handle_t *pamh, const char *msg)
{
	const struct pam_message mymsg = {
		.msg_style = PAM_TEXT_INFO,
		.msg = msg,
	};
	const struct pam_message *msgp = &mymsg;
	const struct pam_conv *pc;
	struct pam_response *resp;
	int r;

	r = pam_get_item(pamh, PAM_CONV, (const void **) &pc);
	if (r != PAM_SUCCESS)
		return FALSE;

	if (!pc || !pc->conv)
		return FALSE;

	return (pc->conv(1, &msgp, &resp, pc->appdata_ptr) == PAM_SUCCESS);
}

static gboolean send_err_msg(pam_handle_t *pamh, const char *msg)
{
	const struct pam_message mymsg = {
		.msg_style = PAM_ERROR_MSG,
		.msg = msg,
	};
	const struct pam_message *msgp = &mymsg;
	const struct pam_conv *pc;
	struct pam_response *resp;
	int r;

	r = pam_get_item(pamh, PAM_CONV, (const void **) &pc);
	if (r != PAM_SUCCESS)
		return FALSE;

	if (!pc || !pc->conv)
		return FALSE;

	return (pc->conv(1, &msgp, &resp, pc->appdata_ptr) == PAM_SUCCESS);
}


static const char *fingerstr(enum fp_finger finger)
{
	const char *names[] = {
		[LEFT_THUMB] = "left thumb",
		[LEFT_INDEX] = "left index",
		[LEFT_MIDDLE] = "left middle",
		[LEFT_RING] = "left ring",
		[LEFT_LITTLE] = "left little",
		[RIGHT_THUMB] = "right thumb",
		[RIGHT_INDEX] = "right index",
		[RIGHT_MIDDLE] = "right middle",
		[RIGHT_RING] = "right ring",
		[RIGHT_LITTLE] = "right little",
	};
	if (finger < LEFT_THUMB || finger > RIGHT_LITTLE)
		return "UNKNOWN";
	return names[finger];
}

static DBusGProxy *create_manager (DBusGConnection **ret_conn)
{
	GError *error = NULL;
	DBusGConnection *connection;
	DBusGProxy *manager;

	connection = dbus_g_bus_get(DBUS_BUS_SYSTEM, &error);
	if (connection == NULL) {
		g_error_free (error);
		return NULL;
	}

	manager = dbus_g_proxy_new_for_name(connection,
					    "net.reactivated.Fprint", "/net/reactivated/Fprint/Manager",
					    "net.reactivated.Fprint.Manager");
	*ret_conn = connection;

	return manager;
}

static DBusGProxy *open_device(DBusGConnection *connection, DBusGProxy *manager, const char *username)
{
	GError *error = NULL;
	GPtrArray *devices;
	gchar *path;
	DBusGProxy *dev;

	if (!dbus_g_proxy_call (manager, "GetDevices", &error,
				G_TYPE_INVALID, dbus_g_type_get_collection ("GPtrArray", DBUS_TYPE_G_OBJECT_PATH),
				&devices, G_TYPE_INVALID)) {
		//g_print("list_devices failed: %s", error->message);
		g_error_free (error);
		return NULL;
	}
	
	if (devices->len == 0) {
		//g_print("No devices found\n");
		return NULL;
	}

	//g_print("found %d devices\n", devices->len);
	path = g_ptr_array_index(devices, 0);
	//g_print("Using device %s\n", path);

	dev = dbus_g_proxy_new_for_name(connection,
					"net.reactivated.Fprint",
					path,
					"net.reactivated.Fprint.Device");
	
	g_ptr_array_foreach(devices, (GFunc) g_free, NULL);
	g_ptr_array_free(devices, TRUE);

	if (!dbus_g_proxy_call (dev, "Claim", &error, G_TYPE_STRING, username, G_TYPE_INVALID, G_TYPE_INVALID)) {
		//g_print("failed to claim device: %s\n", error->message);
		g_error_free (error);
		g_object_unref (dev);
		return NULL;
	}
	return dev;
}

typedef struct {
	guint max_tries;
	int result;
	gboolean verify_completed;
	gboolean timed_out;
	pam_handle_t *pamh;
} verify_data;

static void verify_result(GObject *object, int result, gpointer user_data)
{
	verify_data *data = user_data;

	//g_print("Verify result: %s (%d)\n", verify_result_str(result), result);
	if (result == VERIFY_NO_MATCH || result == VERIFY_MATCH) {
		data->verify_completed = TRUE;
		data->result = result;
	}
}

static void verify_finger_selected(GObject *object, int finger, gpointer user_data)
{
	verify_data *data = user_data;
	char *msg;
	//FIXME
	const char *driver_name = "Fingerprint reader";

	if (finger == -1) {
		msg = g_strdup_printf ("Scan finger on %s", driver_name);
	} else {
		msg = g_strdup_printf ("Scan %s finger on %s", fingerstr(finger), driver_name);
	}
	send_info_msg (data->pamh, msg);
	g_free (msg);
}

static gboolean verify_timeout_cb (gpointer user_data)
{
	verify_data *data = user_data;

	data->timed_out = TRUE;
	data->verify_completed = TRUE;

	send_info_msg (data->pamh, "Verification timed out");

	return FALSE;
}

static int do_verify(pam_handle_t *pamh, DBusGProxy *dev)
{
	GError *error;
	verify_data *data;
	int ret;

	data = g_new0 (verify_data, 1);
	data->max_tries = MAX_TRIES;
	data->pamh = pamh;

	dbus_g_proxy_add_signal(dev, "VerifyStatus", G_TYPE_INT, NULL);
	dbus_g_proxy_add_signal(dev, "VerifyFingerSelected", G_TYPE_INT, NULL);
	dbus_g_proxy_connect_signal(dev, "VerifyStatus", G_CALLBACK(verify_result),
				    data, NULL);
	dbus_g_proxy_connect_signal(dev, "VerifyFingerSelected", G_CALLBACK(verify_finger_selected),
				    data, NULL);


	ret = PAM_AUTH_ERR;

	while (ret == PAM_AUTH_ERR && data->max_tries > 0) {
		guint timeout_id;

		timeout_id = g_timeout_add_seconds (TIMEOUT, verify_timeout_cb, data);

		if (!dbus_g_proxy_call (dev, "VerifyStart", &error, G_TYPE_UINT, -1, G_TYPE_INVALID, G_TYPE_INVALID)) {
			//g_print("VerifyStart failed: %s", error->message);
			g_error_free (error);
			break;
		}

		while (!data->verify_completed)
			g_main_context_iteration(NULL, TRUE);

		/* Ignore errors from VerifyStop */
		dbus_g_proxy_call (dev, "VerifyStop", NULL, G_TYPE_INVALID, G_TYPE_INVALID);

		g_source_remove (timeout_id);

		if (data->timed_out)
			ret = PAM_AUTHINFO_UNAVAIL;
		else {
			if (data->result == VERIFY_NO_MATCH)
				ret = PAM_AUTH_ERR;
			else if (data->result == VERIFY_MATCH)
				ret = PAM_SUCCESS;
			else if (data->result < 0)
				ret = PAM_AUTHINFO_UNAVAIL;
			else {
				send_info_msg (data->pamh, verify_result_str (data->result));
				ret = PAM_AUTH_ERR;
			}
		}
		data->max_tries--;
	}

	dbus_g_proxy_disconnect_signal(dev, "VerifyStatus", G_CALLBACK(verify_result), data);
	dbus_g_proxy_disconnect_signal(dev, "VerifyFingerSelected", G_CALLBACK(verify_finger_selected), data);

	g_free (data);

	return ret;
}

static void release_device(DBusGProxy *dev)
{
	GError *error = NULL;
	if (!dbus_g_proxy_call (dev, "Release", &error, G_TYPE_INVALID, G_TYPE_INVALID)) {
		//g_print ("ReleaseDevice failed: %s\n", error->message);
		g_error_free (error);
	}
}

static int do_auth(pam_handle_t *pamh, const char *username)
{
	DBusGProxy *manager;
	DBusGConnection *connection;
	GMainLoop *loop;
	DBusGProxy *dev;
	int ret;

	loop = g_main_loop_new(NULL, FALSE);
	manager = create_manager (&connection);
	if (manager == NULL)
		return PAM_AUTHINFO_UNAVAIL;

	dev = open_device(connection, manager, username);
	g_object_unref (manager);
	if (!dev)
		return PAM_AUTHINFO_UNAVAIL;
	ret = do_verify(pamh, dev);
	release_device(dev);
	g_object_unref (dev);

	return ret;
}

PAM_EXTERN int pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc,
				   const char **argv)
{
	const char *rhost = NULL;
	const char *username;
	int r;

	g_type_init ();

	pam_get_item(pamh, PAM_RHOST, (const void **)(const void*) &rhost);
	if (rhost != NULL && strlen(rhost) > 0) {
		/* remote login (e.g. over SSH) */
		return PAM_AUTHINFO_UNAVAIL;
	}

	r = pam_get_user(pamh, &username, NULL);
	if (r != PAM_SUCCESS)
		return PAM_AUTHINFO_UNAVAIL;

	r = do_auth(pamh, username);

	return r;
}

PAM_EXTERN int pam_sm_setcred(pam_handle_t *pamh, int flags, int argc,
			      const char **argv)
{
	return PAM_SUCCESS;
}

PAM_EXTERN int pam_sm_chauthtok(pam_handle_t *pamh, int flags, int argc,
				const char **argv)
{
	return PAM_SUCCESS;
}

