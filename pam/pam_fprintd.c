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
#include <syslog.h>

#include <dbus/dbus-glib-bindings.h>
#include <dbus/dbus-glib-lowlevel.h>

#define PAM_SM_AUTH
#include <security/pam_modules.h>

#include "marshal.h"

#define MAX_TRIES 3
#define TIMEOUT 30

#define D(pamh, ...) {					\
	if (debug) {					\
		char *s;				\
		s = g_strdup_printf (__VA_ARGS__);	\
		send_debug_msg (pamh, s);		\
		g_free (s);				\
	}						\
}


static gboolean debug = FALSE;

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

static void send_debug_msg(pam_handle_t *pamh, const char *msg)
{
	gconstpointer item;
	const char *service;

	if (pam_get_item(pamh, PAM_SERVICE, &item) != PAM_SUCCESS || !item)
		service = "<unknown>";
	else
		service = item;

	openlog (service, LOG_CONS | LOG_PID, LOG_AUTHPRIV);

	syslog (LOG_AUTHPRIV|LOG_WARNING, "%s(%s): %s", "pam_fprintd", service, msg);

	closelog ();

}

struct {
	const char *dbus_name;
	const char *place_str;
	const char *swipe_str;
} fingers[11] = {
	{ "left-thumb", "Place your left thumb on %s", "Swipe your left thumb on %s" },
	{ "left-index-finger", "Place your left index finger on %s", "Swipe your left index finger on %s" },
	{ "left-middle-finger", "Place your left middle finger on %s", "Swipe your left middle finger on %s" },
	{ "left-ring-finger", "Place your left ring finger on %s", "Swipe your left ring finger on %s" },
	{ "left-little-finger", "Place your left little finger on %s", "Swipe your left little finger on %s" },
	{ "right-thumb", "Place your right thumb on %s", "Swipe your right thumb on %s" },
	{ "right-index-finger", "Place your right index finger on %s", "Swipe your right index finger on %s" },
	{ "right-middle-finger", "Place your right middle finger on %s", "Swipe your right middle finger on %s" },
	{ "right-ring-finger", "Place your right ring finger on %s", "Swipe your right ring finger on %s" },
	{ "right-little-finger" "Place your right little finger on %s", "Swipe your right little finger on %s" },
	{ NULL, NULL, NULL }
};

static const char *fingerstr(const char *finger_name, gboolean is_swipe)
{
	guint i;

	for (i = 0; fingers[i].dbus_name != NULL; i++) {
		if (g_str_equal (fingers[i].dbus_name, finger_name)) {
			if (is_swipe == FALSE)
				return fingers[i].place_str;
			else
				return fingers[i].swipe_str;
		}
	}

	g_assert_not_reached ();
}

static const char *resulstr(const char *result, gboolean is_swipe)
{
	if (g_str_equal (result, "verify-retry-scan")) {
		if (is_swipe == FALSE)
			return "Place your finger on the reader again";
		else
			return "Swipe your finger again";
	}
	if (g_str_equal (result, "verify-swipe-too-short"))
		return "Swipe was too short, try again";
	if (g_str_equal (result, "verify-finger-not-centered"))
		return "Your finger was not centered, try swiping your finger again";
	if (g_str_equal (result, "verify-remove-and-retry"))
		return "Remove your finger, and try swiping your finger again";
	g_assert_not_reached ();
}

static DBusGProxy *create_manager (pam_handle_t *pamh, DBusGConnection **ret_conn, GMainLoop **ret_loop)
{
	DBusGConnection *connection;
	DBusConnection *conn;
	DBusGProxy *manager;
	DBusError error;
	GMainLoop *loop;
	GMainContext *ctx;

	/* Otherwise dbus-glib doesn't setup it value types */
	connection = dbus_g_bus_get (DBUS_BUS_SYSTEM, NULL);

	if (connection != NULL)
		dbus_g_connection_unref (connection);

	/* And set us up a private D-Bus connection */
	dbus_error_init (&error);
	conn = dbus_bus_get_private (DBUS_BUS_SYSTEM, &error);
	if (conn == NULL) {
		D(pamh, "Error with getting the bus: %s", error.message);
		dbus_error_free (&error);
		return NULL;
	}

	/* Set up our own main loop context */
	ctx = g_main_context_new ();
	loop = g_main_loop_new (ctx, FALSE);
	dbus_connection_setup_with_g_main (conn, ctx);

	connection = dbus_connection_get_g_connection (conn);

	manager = dbus_g_proxy_new_for_name(connection,
					    "net.reactivated.Fprint",
					    "/net/reactivated/Fprint/Manager",
					    "net.reactivated.Fprint.Manager");
	*ret_conn = connection;
	*ret_loop = loop;

	return manager;
}

static DBusGProxy *open_device(pam_handle_t *pamh, DBusGConnection *connection, DBusGProxy *manager, const char *username)
{
	GError *error = NULL;
	gchar *path;
	DBusGProxy *dev;

	if (!dbus_g_proxy_call (manager, "GetDefaultDevice", &error,
				G_TYPE_INVALID, DBUS_TYPE_G_OBJECT_PATH,
				&path, G_TYPE_INVALID)) {
		D(pamh, "get_default_devices failed: %s", error->message);
		g_error_free (error);
		return NULL;
	}
	
	if (path == NULL) {
		D(pamh, "No devices found\n");
		return NULL;
	}

	D(pamh, "Using device %s\n", path);

	dev = dbus_g_proxy_new_for_name(connection,
					"net.reactivated.Fprint",
					path,
					"net.reactivated.Fprint.Device");
	
	g_free (path);

	if (!dbus_g_proxy_call (dev, "Claim", &error, G_TYPE_STRING, username, G_TYPE_INVALID, G_TYPE_INVALID)) {
		D(pamh, "failed to claim device: %s\n", error->message);
		g_error_free (error);
		g_object_unref (dev);
		return NULL;
	}
	return dev;
}

typedef struct {
	guint max_tries;
	char *result;
	gboolean timed_out;
	gboolean is_swipe;
	pam_handle_t *pamh;
	GMainLoop *loop;

	char *driver;
} verify_data;

static void verify_result(GObject *object, const char *result, gboolean done, gpointer user_data)
{
	verify_data *data = user_data;
	const char *msg;

	D(data->pamh, "Verify result: %s\n", result);
	if (done != FALSE) {
		data->result = g_strdup (result);
		g_main_loop_quit (data->loop);
		return;
	}

	msg = resulstr (result, data->is_swipe);
	send_err_msg (data->pamh, msg);
}

static void verify_finger_selected(GObject *object, const char *finger_name, gpointer user_data)
{
	verify_data *data = user_data;
	char *msg;

	if (g_str_equal (finger_name, "any")) {
		if (data->is_swipe == FALSE)
			msg = g_strdup_printf ("Place your finger on %s", data->driver);
		else
			msg = g_strdup_printf ("Swipe your finger on %s", data->driver);
	} else {
		msg = g_strdup_printf (fingerstr(finger_name, data->is_swipe), data->driver);
	}
	D(data->pamh, "verify_finger_selected %s", msg);
	send_info_msg (data->pamh, msg);
	g_free (msg);
}

static gboolean verify_timeout_cb (gpointer user_data)
{
	verify_data *data = user_data;

	data->timed_out = TRUE;
	send_info_msg (data->pamh, "Verification timed out");
	g_main_loop_quit (data->loop);

	return FALSE;
}

static int do_verify(GMainLoop *loop, pam_handle_t *pamh, DBusGProxy *dev)
{
	GError *error = NULL;
	GHashTable *props;
	DBusGProxy *p;
	verify_data *data;
	int ret;

	data = g_new0 (verify_data, 1);
	data->max_tries = MAX_TRIES;
	data->pamh = pamh;
	data->loop = loop;

	/* Get some properties for the device */
	p = dbus_g_proxy_new_from_proxy (dev, "org.freedesktop.DBus.Properties", NULL);

	if (dbus_g_proxy_call (p, "GetAll", NULL, G_TYPE_STRING, "net.reactivated.Fprint.Device", G_TYPE_INVALID,
			       dbus_g_type_get_map ("GHashTable", G_TYPE_STRING, G_TYPE_VALUE), &props, G_TYPE_INVALID)) {
		const char *scan_type;
		data->driver = g_value_dup_string (g_hash_table_lookup (props, "name"));
		scan_type = g_value_dup_string (g_hash_table_lookup (props, "scan-type"));
		if (g_str_equal (scan_type, "swipe"))
			data->is_swipe = TRUE;
		g_hash_table_destroy (props);
	}

	g_object_unref (p);

	if (!data->driver)
		data->driver = g_strdup ("Fingerprint reader");

	dbus_g_proxy_add_signal(dev, "VerifyStatus", G_TYPE_STRING, G_TYPE_BOOLEAN, NULL);
	dbus_g_proxy_add_signal(dev, "VerifyFingerSelected", G_TYPE_STRING, NULL);
	dbus_g_proxy_connect_signal(dev, "VerifyStatus", G_CALLBACK(verify_result),
				    data, NULL);
	dbus_g_proxy_connect_signal(dev, "VerifyFingerSelected", G_CALLBACK(verify_finger_selected),
				    data, NULL);

	ret = PAM_AUTH_ERR;

	while (ret == PAM_AUTH_ERR && data->max_tries > 0) {
		GSource *source;
		guint timeout_id;

		/* Set up the timeout on our non-default context */
		source = g_timeout_source_new_seconds (TIMEOUT);
		timeout_id = g_source_attach (source, g_main_loop_get_context (loop));
		g_source_set_callback (source, verify_timeout_cb, data, NULL);

		data->timed_out = FALSE;

		if (!dbus_g_proxy_call (dev, "VerifyStart", &error, G_TYPE_STRING, "any", G_TYPE_INVALID, G_TYPE_INVALID)) {
			D(pamh, "VerifyStart failed: %s", error->message);
			g_error_free (error);

			g_source_remove (timeout_id);
			g_source_unref (source);
			break;
		}

		g_main_loop_run (loop);

		g_source_remove (timeout_id);
		g_source_unref (source);

		/* Ignore errors from VerifyStop */
		dbus_g_proxy_call (dev, "VerifyStop", NULL, G_TYPE_INVALID, G_TYPE_INVALID);

		if (data->timed_out) {
			ret = PAM_AUTHINFO_UNAVAIL;
			break;
		} else {
			if (g_str_equal (data->result, "verify-no-match")) {
				send_err_msg (data->pamh, "Failed to match fingerprint");
				ret = PAM_AUTH_ERR;
			} else if (g_str_equal (data->result, "verify-match"))
				ret = PAM_SUCCESS;
			else if (g_str_equal (data->result, "verify-unknown-error"))
				ret = PAM_AUTHINFO_UNAVAIL;
			else {
				send_info_msg (data->pamh, "An unknown error occured");
				ret = PAM_AUTH_ERR;
				g_free (data->result);
				break;
			}
			g_free (data->result);
			data->result = NULL;
		}
		data->max_tries--;
	}

	dbus_g_proxy_disconnect_signal(dev, "VerifyStatus", G_CALLBACK(verify_result), data);
	dbus_g_proxy_disconnect_signal(dev, "VerifyFingerSelected", G_CALLBACK(verify_finger_selected), data);

	g_free (data->driver);
	g_free (data);

	return ret;
}

static void release_device(pam_handle_t *pamh, DBusGProxy *dev)
{
	GError *error = NULL;
	if (!dbus_g_proxy_call (dev, "Release", &error, G_TYPE_INVALID, G_TYPE_INVALID)) {
		D(pamh, "ReleaseDevice failed: %s\n", error->message);
		g_error_free (error);
	}
}

static int do_auth(pam_handle_t *pamh, const char *username)
{
	DBusGProxy *manager;
	DBusGConnection *connection;
	DBusGProxy *dev;
	GMainLoop *loop;
	int ret;

	manager = create_manager (pamh, &connection, &loop);
	if (manager == NULL)
		return PAM_AUTHINFO_UNAVAIL;

	dev = open_device(pamh, connection, manager, username);
	g_object_unref (manager);
	if (!dev)
		return PAM_AUTHINFO_UNAVAIL;
	ret = do_verify(loop, pamh, dev);
	g_main_loop_unref (loop);
	release_device(pamh, dev);
	g_object_unref (dev);

	return ret;
}

PAM_EXTERN int pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc,
				   const char **argv)
{
	const char *rhost = NULL;
	const char *username;
	guint i;
	int r;

	g_type_init ();

	dbus_g_object_register_marshaller (fprintd_marshal_VOID__STRING_BOOLEAN,
					   G_TYPE_NONE, G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_INVALID);

	pam_get_item(pamh, PAM_RHOST, (const void **)(const void*) &rhost);
	if (rhost != NULL && strlen(rhost) > 0) {
		/* remote login (e.g. over SSH) */
		return PAM_AUTHINFO_UNAVAIL;
	}

	r = pam_get_user(pamh, &username, NULL);
	if (r != PAM_SUCCESS)
		return PAM_AUTHINFO_UNAVAIL;

	for (i = 0; i < argc; i++) {
		if (argv[i] != NULL && g_str_equal (argv[i], "debug")) {
			g_message ("debug on");
			debug = TRUE;
		}
	}

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

