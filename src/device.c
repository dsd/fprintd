/*
 * /net/reactivated/Fprint/Device/foo object implementation
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

#include "config.h"

#include <dbus/dbus-glib-bindings.h>
#include <dbus/dbus-glib-lowlevel.h>
#include <glib/gi18n.h>
#include <polkit/polkit.h>
#include <polkit-dbus/polkit-dbus.h>
#include <libfprint/fprint.h>

#include <sys/types.h>
#include <pwd.h>

#include "fprintd-marshal.h"
#include "fprintd.h"
#include "storage.h"
#include "egg-dbus-monitor.h"

static char *fingers[] = {
	"left-thumb",
	"left-index-finger",
	"left-middle-finger",
	"left-ring-finger",
	"left-little-finger",
	"right-thumb",
	"right-index-finger",
	"right-middle-finger",
	"right-ring-finger",
	"right-little-finger"
};

extern DBusGConnection *fprintd_dbus_conn;

static void fprint_device_claim(FprintDevice *rdev,
				const char *username,
				DBusGMethodInvocation *context);
static void fprint_device_release(FprintDevice *rdev,
	DBusGMethodInvocation *context);
static void fprint_device_verify_start(FprintDevice *rdev,
	const char *finger_name, DBusGMethodInvocation *context);
static void fprint_device_verify_stop(FprintDevice *rdev,
	DBusGMethodInvocation *context);
static void fprint_device_enroll_start(FprintDevice *rdev,
	const char *finger_name, DBusGMethodInvocation *context);
static void fprint_device_enroll_stop(FprintDevice *rdev,
	DBusGMethodInvocation *context);
static void fprint_device_list_enrolled_fingers(FprintDevice *rdev, 
						const char *username,
						DBusGMethodInvocation *context);
static void fprint_device_delete_enrolled_fingers(FprintDevice *rdev,
						  const char *username,
						  DBusGMethodInvocation *context);

#include "device-dbus-glue.h"

typedef enum {
	ACTION_NONE = 0,
	ACTION_IDENTIFY,
	ACTION_VERIFY,
	ACTION_ENROLL
} FprintDeviceAction;

struct session_data {
	/* finger being enrolled */
	int enroll_finger;

	/* method invocation for async ClaimDevice() */
	DBusGMethodInvocation *context_claim_device;

	/* method invocation for async ReleaseDevice() */
	DBusGMethodInvocation *context_release_device;
};

struct FprintDevicePrivate {
	guint32 id;
	struct fp_dscv_dev *ddev;
	struct fp_dev *dev;
	struct session_data *session;

	PolKitContext *pol_ctx;

	/* The current user of the device, if claimed */
	char *sender;

	/* The current user of the device, or if allowed,
	 * what was passed as a username argument */
	char *username;

	/* type of storage */
	int storage_type;

	/* Hashtable of connected clients */
	GHashTable *clients;

	/* The data passed to fp_async_verify_start or
	 * fp_async_identify_start */
	struct fp_print_data *verify_data;
	struct fp_print_data **identify_data;

	/* whether we're running an identify, or a verify */
	FprintDeviceAction current_action;
	/* Whether we should ignore new signals on the device */
	gboolean action_done;
};

typedef struct FprintDevicePrivate FprintDevicePrivate;

#define DEVICE_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE((o), FPRINT_TYPE_DEVICE, FprintDevicePrivate))

enum fprint_device_properties {
	FPRINT_DEVICE_CONSTRUCT_DDEV = 1,
	FPRINT_DEVICE_IN_USE,
	FPRINT_DEVICE_NAME,
	FPRINT_DEVICE_NUM_ENROLL,
	FPRINT_DEVICE_SCAN_TYPE
};

enum fprint_device_signals {
	SIGNAL_VERIFY_STATUS,
	SIGNAL_VERIFY_FINGER_SELECTED,
	SIGNAL_ENROLL_STATUS,
	NUM_SIGNALS,
};

static GObjectClass *parent_class = NULL;
static guint32 last_id = ~0;
static guint signals[NUM_SIGNALS] = { 0, };

static void fprint_device_finalize(GObject *object)
{
	FprintDevice *self = (FprintDevice *) object;
	FprintDevicePrivate *priv = DEVICE_GET_PRIVATE(self);

	g_hash_table_destroy (priv->clients);
	/* FIXME close and stuff */
}

static void fprint_device_set_property(GObject *object, guint property_id,
	const GValue *value, GParamSpec *pspec)
{
	FprintDevice *self = (FprintDevice *) object;
	FprintDevicePrivate *priv = DEVICE_GET_PRIVATE(self);

	switch (property_id) {
	case FPRINT_DEVICE_CONSTRUCT_DDEV:
		priv->ddev = g_value_get_pointer(value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
		break;
	}
}

static void fprint_device_get_property(GObject *object, guint property_id,
				       GValue *value, GParamSpec *pspec)
{
	FprintDevice *self = (FprintDevice *) object;
	FprintDevicePrivate *priv = DEVICE_GET_PRIVATE(self);

	switch (property_id) {
	case FPRINT_DEVICE_IN_USE:
		g_value_set_boolean(value, g_hash_table_size (priv->clients) != 0);
		break;
	case FPRINT_DEVICE_NAME:
		g_value_set_static_string (value, fp_driver_get_full_name (fp_dscv_dev_get_driver (priv->ddev)));
		break;
	case FPRINT_DEVICE_NUM_ENROLL:
		if (priv->dev)
			g_value_set_int (value, fp_dev_get_nr_enroll_stages (priv->dev));
		else
			g_value_set_int (value, -1);
		break;
	case FPRINT_DEVICE_SCAN_TYPE: {
		const char *type;

		if (fp_driver_get_scan_type (fp_dscv_dev_get_driver (priv->ddev)) == FP_SCAN_TYPE_PRESS)
			type = "press";
		else
			type = "swipe";

		g_value_set_static_string (value, type);
		break;
	}
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
		break;
	}
}

static void fprint_device_class_init(FprintDeviceClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
	GParamSpec *pspec;

	dbus_g_object_type_install_info(FPRINT_TYPE_DEVICE,
		&dbus_glib_fprint_device_object_info);
	parent_class = g_type_class_peek_parent(klass);

	gobject_class->finalize = fprint_device_finalize;
	gobject_class->set_property = fprint_device_set_property;
	gobject_class->get_property = fprint_device_get_property;
	g_type_class_add_private(klass, sizeof(FprintDevicePrivate));

	pspec = g_param_spec_pointer("discovered-dev", "Discovered device",
				     "Set discovered device construction property",
				     G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE);
	g_object_class_install_property(gobject_class,
					FPRINT_DEVICE_CONSTRUCT_DDEV, pspec);

	pspec = g_param_spec_boolean("in-use", "In use",
				     "Whether the device is currently in use", FALSE,
				     G_PARAM_READABLE);
	g_object_class_install_property(gobject_class,
					FPRINT_DEVICE_IN_USE, pspec);

	pspec = g_param_spec_string("name", "Name",
				    "The product name of the device", NULL,
				    G_PARAM_READABLE);
	g_object_class_install_property(gobject_class,
					FPRINT_DEVICE_NAME, pspec);

	pspec = g_param_spec_string("scan-type", "Scan Type",
				    "The scan type of the device", "press",
				    G_PARAM_READABLE);
	g_object_class_install_property(gobject_class,
					FPRINT_DEVICE_SCAN_TYPE, pspec);

	pspec = g_param_spec_int("num-enroll-stages", "Number of enrollments stages",
				  "Number of enrollment stages for the device.",
				  -1, G_MAXINT, -1, G_PARAM_READABLE);
	g_object_class_install_property(gobject_class,
					FPRINT_DEVICE_NUM_ENROLL, pspec);

	signals[SIGNAL_VERIFY_STATUS] = g_signal_new("verify-status",
		G_TYPE_FROM_CLASS(gobject_class), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
		fprintd_marshal_VOID__STRING_BOOLEAN, G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_BOOLEAN);
	signals[SIGNAL_ENROLL_STATUS] = g_signal_new("enroll-status",
		G_TYPE_FROM_CLASS(gobject_class), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
		fprintd_marshal_VOID__STRING_BOOLEAN, G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_BOOLEAN);
	signals[SIGNAL_VERIFY_FINGER_SELECTED] = g_signal_new("verify-finger-selected",
		G_TYPE_FROM_CLASS(gobject_class), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
		g_cclosure_marshal_VOID__STRING, G_TYPE_NONE, 1, G_TYPE_STRING);
}

static gboolean
pk_io_watch_have_data (GIOChannel *channel, GIOCondition condition, gpointer user_data)
{
	int fd;
	PolKitContext *pk_context = user_data;
	fd = g_io_channel_unix_get_fd (channel);
	polkit_context_io_func (pk_context, fd);
	return TRUE;
}

static int 
pk_io_add_watch (PolKitContext *pk_context, int fd)
{
	guint id = 0;
	GIOChannel *channel;
	channel = g_io_channel_unix_new (fd);
	if (channel == NULL)
		goto out;
	id = g_io_add_watch (channel, G_IO_IN, pk_io_watch_have_data, pk_context);
	if (id == 0) {
		g_io_channel_unref (channel);
		goto out;
	}
	g_io_channel_unref (channel);
out:
	return id;
}

static void 
pk_io_remove_watch (PolKitContext *pk_context, int watch_id)
{
	g_source_remove (watch_id);
}

static void fprint_device_init(FprintDevice *device)
{
	FprintDevicePrivate *priv = DEVICE_GET_PRIVATE(device);
	priv->id = ++last_id;

	/* Setup PolicyKit */
	priv->pol_ctx = polkit_context_new ();
	polkit_context_set_io_watch_functions (priv->pol_ctx, pk_io_add_watch, pk_io_remove_watch);
	if (!polkit_context_init (priv->pol_ctx, NULL)) {
		g_critical ("cannot initialize libpolkit");
		polkit_context_unref (priv->pol_ctx);
		priv->pol_ctx = NULL;
	}
	priv->clients = g_hash_table_new_full (g_str_hash,
					       g_str_equal,
					       g_free,
					       g_object_unref);
}

G_DEFINE_TYPE(FprintDevice, fprint_device, G_TYPE_OBJECT);

FprintDevice *fprint_device_new(struct fp_dscv_dev *ddev)
{
	return g_object_new(FPRINT_TYPE_DEVICE, "discovered-dev", ddev, NULL);	
}

guint32 _fprint_device_get_id(FprintDevice *rdev)
{
	return DEVICE_GET_PRIVATE(rdev)->id;
}

static const char *
finger_num_to_name (int finger_num)
{
	if (finger_num == -1)
		return "any";
	if (finger_num < LEFT_THUMB || finger_num > RIGHT_LITTLE)
		return NULL;
	return fingers[finger_num - 1];
}

static int
finger_name_to_num (const char *finger_name)
{
	guint i;

	if (finger_name == NULL || *finger_name == '\0' || g_str_equal (finger_name, "any"))
		return -1;

	for (i = 0; i < G_N_ELEMENTS (fingers); i++) {
		if (g_str_equal (finger_name, fingers[i]))
			return i + 1;
	}

	/* Invalid, let's try that */
	return -1;
}

static const char *
verify_result_to_name (int result)
{
	switch (result) {
	case FP_VERIFY_NO_MATCH:
		return "verify-no-match";
	case FP_VERIFY_MATCH:
		return "verify-match";
	case FP_VERIFY_RETRY:
		return "verify-retry-scan";
	case FP_VERIFY_RETRY_TOO_SHORT:
		return "verify-swipe-too-short";
	case FP_VERIFY_RETRY_CENTER_FINGER:
		return "verify-finger-not-centered";
	case FP_VERIFY_RETRY_REMOVE_FINGER:
		return "verify-remove-and-retry";
	default:
		return "verify-unknown-error";
	}
}

static const char *
enroll_result_to_name (int result)
{
	switch (result) {
	case FP_ENROLL_COMPLETE:
		return "enroll-completed";
	case FP_ENROLL_FAIL:
		return "enroll-failed";
	case FP_ENROLL_PASS:
		return "enroll-stage-passed";
	case FP_ENROLL_RETRY:
		return "enroll-retry-scan";
	case FP_ENROLL_RETRY_TOO_SHORT:
		return "enroll-swipe-too-short";
	case FP_ENROLL_RETRY_CENTER_FINGER:
		return "enroll-finger-not-centered";
	case FP_ENROLL_RETRY_REMOVE_FINGER:
		return "enroll-remove-and-retry";
	default:
		return "enroll-unknown-error";
	}
}

static gboolean
_fprint_device_check_claimed (FprintDevice *rdev,
			      DBusGMethodInvocation *context,
			      GError **error)
{
	FprintDevicePrivate *priv = DEVICE_GET_PRIVATE(rdev);
	DBusConnection *conn;
	char *sender;
	gboolean retval;

	/* The device wasn't claimed, exit */
	if (priv->sender == NULL) {
		g_set_error (error, FPRINT_ERROR, FPRINT_ERROR_CLAIM_DEVICE,
			     _("Device was not claimed before use"));
		return FALSE;
	}

	conn = dbus_g_connection_get_connection (fprintd_dbus_conn);
	sender = dbus_g_method_get_sender (context);
	retval = g_str_equal (sender, priv->sender);
	g_free (sender);

	if (retval == FALSE) {
		g_set_error (error, FPRINT_ERROR, FPRINT_ERROR_ALREADY_IN_USE,
			     _("Device already in use by another user"));
	}

	return retval;
}

static gboolean
_fprint_device_check_polkit_for_action (FprintDevice *rdev, DBusGMethodInvocation *context, const char *action, GError **error)
{
	FprintDevicePrivate *priv = DEVICE_GET_PRIVATE(rdev);
	const char *sender;
	DBusError dbus_error;
	PolKitCaller *pk_caller;
	PolKitAction *pk_action;
	PolKitResult pk_result;
	uid_t uid;

	/* Check that caller is privileged */
	sender = dbus_g_method_get_sender (context);
	dbus_error_init (&dbus_error);
	pk_caller = polkit_caller_new_from_dbus_name (
	    dbus_g_connection_get_connection (fprintd_dbus_conn),
	    sender, 
	    &dbus_error);
	if (pk_caller == NULL) {
		g_set_error (error, FPRINT_ERROR,
			     FPRINT_ERROR_INTERNAL,
			     "Error getting information about caller: %s: %s",
			     dbus_error.name, dbus_error.message);
		dbus_error_free (&dbus_error);
		return FALSE;
	}

	/* XXX Hack?
	 * We'd like to allow root to set the username by default, so
	 * it can authenticate users through PAM
	 * https://bugzilla.redhat.com/show_bug.cgi?id=447266 */
	if ((polkit_caller_get_uid (pk_caller, &uid) && uid == 0) &&
	    (g_str_equal (action, "net.reactivated.fprint.device.setusername") ||
	     g_str_equal (action, "net.reactivated.fprint.device.verify"))) {
		polkit_caller_unref (pk_caller);
		return TRUE;
	}

	pk_action = polkit_action_new ();
	polkit_action_set_action_id (pk_action, action);
	pk_result = polkit_context_is_caller_authorized (priv->pol_ctx, pk_action, pk_caller,
							 TRUE, NULL);
	polkit_caller_unref (pk_caller);
	polkit_action_unref (pk_action);

	if (pk_result != POLKIT_RESULT_YES) {
		g_set_error (error, FPRINT_ERROR,
			     FPRINT_ERROR_PERMISSION_DENIED,
			     "%s %s <-- (action, result)",
			     action,
			     polkit_result_to_string_representation (pk_result));
		dbus_error_free (&dbus_error);
		return FALSE;
	}

	return TRUE;
}

static gboolean
_fprint_device_check_polkit_for_actions (FprintDevice *rdev,
					 DBusGMethodInvocation *context,
					 const char *action1,
					 const char *action2,
					 GError **error)
{
	if (_fprint_device_check_polkit_for_action (rdev, context, action1, error) != FALSE)
		return TRUE;

	g_error_free (*error);
	*error = NULL;

	return _fprint_device_check_polkit_for_action (rdev, context, action2, error);
}

static char *
_fprint_device_check_for_username (FprintDevice *rdev,
				   DBusGMethodInvocation *context,
				   const char *username,
				   char **ret_sender,
				   GError **error)
{
	DBusConnection *conn;
	DBusError dbus_error;
	char *sender;
	unsigned long uid;
	struct passwd *user;
	char *client_username;

	/* Get details about the current sender, and username/uid */
	conn = dbus_g_connection_get_connection (fprintd_dbus_conn);
	sender = dbus_g_method_get_sender (context);
	dbus_error_init (&dbus_error);
	uid = dbus_bus_get_unix_user (conn, sender, &dbus_error);

	if (dbus_error_is_set(&dbus_error)) {
		g_free (sender);
		dbus_set_g_error (error, &dbus_error);
		return NULL;
	}

	user = getpwuid (uid);
	if (user == NULL) {
		g_free (sender);
		g_set_error(error, FPRINT_ERROR, FPRINT_ERROR_INTERNAL,
			    "Failed to get information about user UID %lu", uid);
		return NULL;
	}
	client_username = g_strdup (user->pw_name);

	/* The current user is usually allowed to access their
	 * own data, this should be followed by PolicyKit checks
	 * anyway */
	if (username == NULL || *username == '\0' || g_str_equal (username, client_username)) {
		if (ret_sender != NULL)
			*ret_sender = sender;
		else
			g_free (sender);
		return client_username;
	}

	/* If we're not allowed to set a different username,
	 * then fail */
	if (_fprint_device_check_polkit_for_action (rdev, context, "net.reactivated.fprint.device.setusername", error) == FALSE) {
		g_free (sender);
		return NULL;
	}

	if (ret_sender != NULL)
		*ret_sender = sender;
	else
		g_free (sender);

	return g_strdup (username);
}

static void action_stop_cb(struct fp_dev *dev, void *user_data)
{
	gboolean *done = (gboolean *) user_data;
	*done = TRUE;
}

static void
_fprint_device_client_disconnected (EggDbusMonitor *monitor, gboolean connected, FprintDevice *rdev)
{
	FprintDevicePrivate *priv = DEVICE_GET_PRIVATE(rdev);

	if (connected == FALSE) {
		const char *sender;
		sender = egg_dbus_monitor_get_service (monitor);

		/* Was that the client that claimed the device? */
		if (priv->sender != NULL) {
			gboolean done = FALSE;
			switch (priv->current_action) {
			case ACTION_NONE:
				break;
			case ACTION_IDENTIFY:
				fp_async_identify_stop(priv->dev, action_stop_cb, &done);
				while (done == FALSE)
					g_main_context_iteration (NULL, TRUE);
				break;
			case ACTION_VERIFY:
				fp_async_verify_stop(priv->dev, action_stop_cb, &done);
				while (done == FALSE)
					g_main_context_iteration (NULL, TRUE);
				break;
			case ACTION_ENROLL:
				fp_async_enroll_stop(priv->dev, action_stop_cb, &done);
				while (done == FALSE)
					g_main_context_iteration (NULL, TRUE);
				break;
			}
			priv->current_action = ACTION_NONE;
			done = FALSE;

			/* Close the claimed device as well */
			fp_async_dev_close (priv->dev, action_stop_cb, &done);
			while (done == FALSE)
				g_main_context_iteration (NULL, TRUE);

			g_free (priv->sender);
			priv->sender = NULL;
			g_free (priv->username);
			priv->username = NULL;
		}
		g_hash_table_remove (priv->clients, sender);
	}

	if (g_hash_table_size (priv->clients) == 0) {
		g_object_notify (G_OBJECT (rdev), "in-use");
	}
}

static void
_fprint_device_add_client (FprintDevice *rdev, const char *sender)
{
	EggDbusMonitor *monitor;
	FprintDevicePrivate *priv = DEVICE_GET_PRIVATE(rdev);

	monitor = g_hash_table_lookup (priv->clients, sender);
	if (monitor == NULL) {
		monitor = egg_dbus_monitor_new ();
		egg_dbus_monitor_assign (monitor, fprintd_dbus_conn, sender);
		//FIXME handle replaced
		g_signal_connect (G_OBJECT (monitor), "connection-changed",
					 G_CALLBACK (_fprint_device_client_disconnected), rdev);
		g_hash_table_insert (priv->clients, g_strdup (sender), monitor);
		g_object_notify (G_OBJECT (rdev), "in-use");
	}
}

static void dev_open_cb(struct fp_dev *dev, int status, void *user_data)
{
	FprintDevice *rdev = user_data;
	FprintDevicePrivate *priv = DEVICE_GET_PRIVATE(rdev);
	struct session_data *session = priv->session;

	g_message("device %d claim status %d", priv->id, status);

	if (status != 0) {
		GError *error;

		g_free (priv->sender);
		priv->sender = NULL;

		g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_INTERNAL,
			"Open failed with error %d", status);
		dbus_g_method_return_error(session->context_claim_device, error);
		return;
	}

	priv->dev = dev;
	dbus_g_method_return(session->context_claim_device);
}

static void fprint_device_claim(FprintDevice *rdev,
				const char *username,
				DBusGMethodInvocation *context)
{
	FprintDevicePrivate *priv = DEVICE_GET_PRIVATE(rdev);
	GError *error = NULL;
	char *sender, *user;
	int r;

	/* Is it already claimed? */
	if (priv->sender != NULL) {
		g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_ALREADY_IN_USE,
			    "Device was already claimed");
		dbus_g_method_return_error(context, error);
		return;
	}

	g_assert (priv->username == NULL);
	g_assert (priv->sender == NULL);

	sender = NULL;
	user = _fprint_device_check_for_username (rdev,
						  context,
						  username,
						  &sender,
						  &error);
	if (user == NULL) {
		g_free (sender);
		dbus_g_method_return_error (context, error);
		g_error_free (error);
		return;
	}

	if (_fprint_device_check_polkit_for_actions (rdev, context,
						     "net.reactivated.fprint.device.verify",
						     "net.reactivated.fprint.device.enroll",
						     &error) == FALSE) {
		g_free (sender);
		g_free (user);
		dbus_g_method_return_error (context, error);
		return;
	}

	_fprint_device_add_client (rdev, sender);

	priv->username = user;
	priv->sender = sender;

	g_message ("user '%s' claiming the device: %d", priv->username, priv->id);

	priv->session = g_slice_new0(struct session_data);
	priv->session->context_claim_device = context;

	r = fp_async_dev_open(priv->ddev, dev_open_cb, rdev);
	if (r < 0) {
		g_slice_free(struct session_data, priv->session);
		priv->session = NULL;

		g_free (priv->username);
		priv->username = NULL;
		g_free (priv->sender);
		priv->sender = NULL;

		g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_INTERNAL,
			"Could not attempt device open, error %d", r);
		dbus_g_method_return_error(context, error);
	}
}

static void dev_close_cb(struct fp_dev *dev, void *user_data)
{
	FprintDevice *rdev = user_data;
	FprintDevicePrivate *priv = DEVICE_GET_PRIVATE(rdev);
	struct session_data *session = priv->session;
	DBusGMethodInvocation *context = session->context_release_device;

	priv->dev = NULL;
	g_slice_free(struct session_data, session);
	priv->session = NULL;

	g_free (priv->sender);
	priv->sender = NULL;

	g_free (priv->username);
	priv->username = NULL;

	g_message("released device %d", priv->id);
	dbus_g_method_return(context);
}

static void fprint_device_release(FprintDevice *rdev,
	DBusGMethodInvocation *context)
{
	FprintDevicePrivate *priv = DEVICE_GET_PRIVATE(rdev);
	struct session_data *session = priv->session;
	GError *error = NULL;

	if (_fprint_device_check_claimed(rdev, context, &error) == FALSE) {
		dbus_g_method_return_error (context, error);
		return;
	}

	/* People that can claim can also release */
	if (_fprint_device_check_polkit_for_actions (rdev, context,
						     "net.reactivated.fprint.device.verify",
						     "net.reactivated.fprint.device.enroll",
						     &error) == FALSE) {
		dbus_g_method_return_error (context, error);
		return;
	}

	session->context_release_device = context;
	fp_async_dev_close(priv->dev, dev_close_cb, rdev);
}

static void verify_cb(struct fp_dev *dev, int r, struct fp_img *img,
		      void *user_data)
{
	struct FprintDevice *rdev = user_data;
	FprintDevicePrivate *priv = DEVICE_GET_PRIVATE(rdev);
	const char *name = verify_result_to_name (r);

	if (priv->action_done != FALSE)
		return;

	g_message("verify_cb: result %s (%d)", name, r);

	if (r == FP_VERIFY_NO_MATCH || r == FP_VERIFY_MATCH || r < 0)
		priv->action_done = TRUE;
	g_signal_emit(rdev, signals[SIGNAL_VERIFY_STATUS], 0, name, priv->action_done);
	fp_img_free(img);

	if (priv->action_done && priv->verify_data) {
		fp_print_data_free (priv->verify_data);
		priv->verify_data = NULL;
	}
}

static void identify_cb(struct fp_dev *dev, int r,
			 size_t match_offset, struct fp_img *img, void *user_data)
{
	struct FprintDevice *rdev = user_data;
	FprintDevicePrivate *priv = DEVICE_GET_PRIVATE(rdev);
	const char *name = verify_result_to_name (r);

	if (priv->action_done != FALSE)
		return;

	g_message("identify_cb: result %s (%d)", name, r);

	if (r == FP_VERIFY_NO_MATCH || r == FP_VERIFY_MATCH || r < 0)
		priv->action_done = TRUE;
	g_signal_emit(rdev, signals[SIGNAL_VERIFY_STATUS], 0, name, priv->action_done);
	fp_img_free(img);

	if (priv->action_done && priv->identify_data != NULL) {
		guint i;
		for (i = 0; priv->identify_data[i] != NULL; i++)
			fp_print_data_free(priv->identify_data[i]);
		g_free (priv->identify_data);
		priv->identify_data = NULL;
	}
}

static void fprint_device_verify_start(FprintDevice *rdev,
	const char *finger_name, DBusGMethodInvocation *context)
{
	FprintDevicePrivate *priv = DEVICE_GET_PRIVATE(rdev);
	struct fp_print_data **gallery = NULL;
	struct fp_print_data *data = NULL;
	GError *error = NULL;
	guint finger_num = finger_name_to_num (finger_name);
	int r;

	if (_fprint_device_check_claimed(rdev, context, &error) == FALSE) {
		dbus_g_method_return_error (context, error);
		g_error_free (error);
		return;
	}

	if (_fprint_device_check_polkit_for_action (rdev, context, "net.reactivated.fprint.device.verify", &error) == FALSE) {
		dbus_g_method_return_error (context, error);
		g_error_free (error);
		return;
	}

	if (priv->current_action != ACTION_NONE) {
		if (priv->current_action == ACTION_ENROLL) {
			g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_ALREADY_IN_USE,
				    "Enrollment in progress");
		} else {
			g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_ALREADY_IN_USE,
				    "Verification already in progress");
		}
		dbus_g_method_return_error(context, error);
		g_error_free (error);
		return;
	}
	priv->action_done = FALSE;

	if (finger_num == -1) {
		GSList *prints;

		prints = store.discover_prints(priv->ddev, priv->username);
		if (prints == NULL) {
			g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_NO_ENROLLED_PRINTS,
				    "No fingerprints enrolled");
			dbus_g_method_return_error(context, error);
			return;
		}
		if (fp_dev_supports_identification(priv->dev)) {
			GSList *l;
			GPtrArray *array;

			array = g_ptr_array_new ();

			for (l = prints; l != NULL; l = l->next) {
				g_message ("adding finger %d to the gallery", GPOINTER_TO_INT (l->data));
				r = store.print_data_load(priv->dev, GPOINTER_TO_INT (l->data),
							  &data, priv->username);
				if (r == 0)
					g_ptr_array_add (array, data);
			}
			data = NULL;

			if (array->len > 0) {
				g_ptr_array_add (array,  NULL);
				gallery = (struct fp_print_data **) g_ptr_array_free (array, FALSE);
			} else {
				gallery = NULL;
			}
		} else {
			finger_num = GPOINTER_TO_INT (prints->data);
		}
		g_slist_free(prints);
	}

	if (fp_dev_supports_identification(priv->dev) && finger_num == -1) {
		if (gallery == NULL) {
			g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_NO_ENROLLED_PRINTS,
				    "No fingerprints on that device");
			dbus_g_method_return_error(context, error);
			g_error_free (error);
			return;
		}
		priv->current_action = ACTION_IDENTIFY;

		g_message ("start identification device %d", priv->id);
		r = fp_async_identify_start (priv->dev, gallery, identify_cb, rdev);
	} else {
		priv->current_action = ACTION_VERIFY;

		g_message("start verification device %d finger %d", priv->id, finger_num);

		r = store.print_data_load(priv->dev, (enum fp_finger)finger_num, 
					  &data, priv->username);

		if (r < 0 || !data) {
			g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_INTERNAL,
				    "No such print %d", finger_num);
			dbus_g_method_return_error(context, error);
			return;
		}

		r = fp_async_verify_start(priv->dev, data, verify_cb, rdev);
	}

	/* Emit VerifyFingerSelected telling the front-end which finger
	 * we selected for auth */
	g_signal_emit(rdev, signals[SIGNAL_VERIFY_FINGER_SELECTED],
		      0, finger_num_to_name (finger_num));


	if (r < 0) {
		if (data != NULL) {
			fp_print_data_free (data);
		} else if (gallery != NULL) {
			guint i;
			for (i = 0; gallery[i] != NULL; i++)
				fp_print_data_free(gallery[i]);
			g_free (gallery);
		}
		g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_INTERNAL,
			"Verify start failed with error %d", r);
		dbus_g_method_return_error(context, error);
		return;
	}
	priv->verify_data = data;
	priv->identify_data = gallery;

	dbus_g_method_return(context);
}

static void verify_stop_cb(struct fp_dev *dev, void *user_data)
{
	dbus_g_method_return((DBusGMethodInvocation *) user_data);
}

static void identify_stop_cb(struct fp_dev *dev, void *user_data)
{
	dbus_g_method_return((DBusGMethodInvocation *) user_data);
}

static void fprint_device_verify_stop(FprintDevice *rdev,
	DBusGMethodInvocation *context)
{
	FprintDevicePrivate *priv = DEVICE_GET_PRIVATE(rdev);
	GError *error = NULL;
	int r;

	if (_fprint_device_check_claimed(rdev, context, &error) == FALSE) {
		dbus_g_method_return_error (context, error);
		return;
	}

	if (_fprint_device_check_polkit_for_action (rdev, context, "net.reactivated.fprint.device.verify", &error) == FALSE) {
		dbus_g_method_return_error (context, error);
		return;
	}

	if (priv->current_action == ACTION_VERIFY) {
		if (priv->verify_data) {
			fp_print_data_free (priv->verify_data);
			priv->verify_data = NULL;
		}
		r = fp_async_verify_stop(priv->dev, verify_stop_cb, context);
	} else if (priv->current_action == ACTION_IDENTIFY) {
		if (priv->identify_data != NULL) {
			guint i;
			for (i = 0; priv->identify_data[i] != NULL; i++)
				fp_print_data_free(priv->identify_data[i]);
			g_free (priv->identify_data);
			priv->identify_data = NULL;
		}
		r = fp_async_identify_stop(priv->dev, identify_stop_cb, context);
	} else {
		g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_NO_ACTION_IN_PROGRESS,
			    "No verification in progress");
		dbus_g_method_return_error(context, error);
		g_error_free (error);
		return;
	}

	if (r < 0) {
		g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_INTERNAL,
			"Verify stop failed with error %d", r);
		dbus_g_method_return_error(context, error);
		g_error_free (error);
	}

	priv->current_action = ACTION_NONE;
}

static void enroll_stage_cb(struct fp_dev *dev, int result,
	struct fp_print_data *print, struct fp_img *img, void *user_data)
{
	struct FprintDevice *rdev = user_data;
	FprintDevicePrivate *priv = DEVICE_GET_PRIVATE(rdev);
	struct session_data *session = priv->session;
	int r;

	/* We're done, ignore new events for the action */
	if (priv->action_done != FALSE)
		return;

	g_message("enroll_stage_cb: result %d", result);
	if (result == FP_ENROLL_COMPLETE) {
		r = store.print_data_save(print, session->enroll_finger, priv->username);
		if (r < 0)
			result = FP_ENROLL_FAIL;
	}

	if (result == FP_ENROLL_COMPLETE || result == FP_ENROLL_FAIL || result < 0)
		priv->action_done = TRUE;

	g_signal_emit(rdev, signals[SIGNAL_ENROLL_STATUS], 0, enroll_result_to_name (result), priv->action_done);

	fp_img_free(img);
	fp_print_data_free(print);
}

static void fprint_device_enroll_start(FprintDevice *rdev,
	const char *finger_name, DBusGMethodInvocation *context)
{
	FprintDevicePrivate *priv = DEVICE_GET_PRIVATE(rdev);
	struct session_data *session = priv->session;
	int finger_num = finger_name_to_num (finger_name);
	GError *error = NULL;
	int r;

	if (finger_num == -1) {
		g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_INVALID_FINGERNAME,
			    "Invalid print name");
		dbus_g_method_return_error(context, error);
		g_error_free (error);
		return;
	}

	if (_fprint_device_check_claimed(rdev, context, &error) == FALSE) {
		dbus_g_method_return_error (context, error);
		return;
	}

	if (_fprint_device_check_polkit_for_action (rdev, context, "net.reactivated.fprint.device.enroll", &error) == FALSE) {
		dbus_g_method_return_error (context, error);
		return;
	}

	if (priv->current_action != ACTION_NONE) {
		if (priv->current_action == ACTION_ENROLL) {
			g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_ALREADY_IN_USE,
				    "Enrollment already in progress");
		} else {
			g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_ALREADY_IN_USE,
				    "Verification in progress");
		}
		dbus_g_method_return_error(context, error);
		g_error_free (error);
		return;
	}

	g_message("start enrollment device %d finger %d", priv->id, finger_num);
	session->enroll_finger = finger_num;
	priv->action_done = FALSE;
	
	r = fp_async_enroll_start(priv->dev, enroll_stage_cb, rdev);
	if (r < 0) {
		g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_INTERNAL,
			"Enroll start failed with error %d", r);
		dbus_g_method_return_error(context, error);
		return;
	}

	priv->current_action = ACTION_ENROLL;

	dbus_g_method_return(context);
}

static void enroll_stop_cb(struct fp_dev *dev, void *user_data)
{
	dbus_g_method_return((DBusGMethodInvocation *) user_data);
}

static void fprint_device_enroll_stop(FprintDevice *rdev,
	DBusGMethodInvocation *context)
{
	FprintDevicePrivate *priv = DEVICE_GET_PRIVATE(rdev);
	GError *error = NULL;
	int r;

	if (_fprint_device_check_claimed(rdev, context, &error) == FALSE) {
		dbus_g_method_return_error (context, error);
		return;
	}

	if (_fprint_device_check_polkit_for_action (rdev, context, "net.reactivated.fprint.device.enroll", &error) == FALSE) {
		dbus_g_method_return_error (context, error);
		return;
	}

	if (priv->current_action != ACTION_ENROLL) {
		g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_NO_ACTION_IN_PROGRESS,
			    "No enrollment in progress");
		dbus_g_method_return_error(context, error);
		g_error_free (error);
		return;
	}

	r = fp_async_enroll_stop(priv->dev, enroll_stop_cb, context);
	if (r < 0) {
		g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_INTERNAL,
			"Enroll stop failed with error %d", r);
		dbus_g_method_return_error(context, error);
		g_error_free (error);
	}

	priv->current_action = ACTION_NONE;
}

static void fprint_device_list_enrolled_fingers(FprintDevice *rdev,
						const char *username,
						DBusGMethodInvocation *context)
{
	FprintDevicePrivate *priv = DEVICE_GET_PRIVATE(rdev);
	GError *error = NULL;
	GSList *prints;
	GSList *item;
	GPtrArray *ret;
	char *user, *sender;

	user = _fprint_device_check_for_username (rdev,
						  context,
						  username,
						  NULL,
						  &error);
	if (user == NULL) {
		dbus_g_method_return_error (context, error);
		g_error_free (error);
		return;
	}

	if (_fprint_device_check_polkit_for_action (rdev, context, "net.reactivated.fprint.device.verify", &error) == FALSE) {
		g_free (user);
		dbus_g_method_return_error (context, error);
		return;
	}

	sender = dbus_g_method_get_sender (context);
	_fprint_device_add_client (rdev, sender);
	g_free (sender);

	prints = store.discover_prints(priv->ddev, user);
	g_free (user);
	if (!prints) {
		g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_NO_ENROLLED_PRINTS,
			"Failed to discover prints");
		dbus_g_method_return_error(context, error);
		return;
	}

	ret = g_ptr_array_new ();
	for (item = prints; item; item = item->next) {
		int finger_num = GPOINTER_TO_INT (item->data);
		g_ptr_array_add (ret, g_strdup (finger_num_to_name (finger_num)));
	}
	g_ptr_array_add (ret, NULL);

	g_slist_free(prints);

	dbus_g_method_return(context, g_ptr_array_free (ret, FALSE));
}

static void fprint_device_delete_enrolled_fingers(FprintDevice *rdev,
						  const char *username,
						  DBusGMethodInvocation *context)
{
	FprintDevicePrivate *priv = DEVICE_GET_PRIVATE(rdev);
	GError *error = NULL;
	guint i;
	char *user, *sender;

	user = _fprint_device_check_for_username (rdev,
						  context,
						  username,
						  NULL,
						  &error);
	if (user == NULL) {
		dbus_g_method_return_error (context, error);
		g_error_free (error);
		return;
	}

	if (_fprint_device_check_polkit_for_action (rdev, context, "net.reactivated.fprint.device.enroll", &error) == FALSE) {
		g_free (user);
		dbus_g_method_return_error (context, error);
		return;
	}

	sender = dbus_g_method_get_sender (context);
	_fprint_device_add_client (rdev, sender);
	g_free (sender);

	for (i = LEFT_THUMB; i <= RIGHT_LITTLE; i++) {
		store.print_data_delete(priv->ddev, i, user);
	}
	g_free (user);

	dbus_g_method_return(context);
}

