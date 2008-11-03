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

#include "fprintd.h"
#include "storage.h"

extern DBusGConnection *fprintd_dbus_conn;

static void fprint_device_claim(FprintDevice *rdev,
				const char *username,
				DBusGMethodInvocation *context);
static void fprint_device_release(FprintDevice *rdev,
	DBusGMethodInvocation *context);
static void fprint_device_verify_start(FprintDevice *rdev,
	guint32 print_id, DBusGMethodInvocation *context);
static void fprint_device_verify_stop(FprintDevice *rdev,
	DBusGMethodInvocation *context);
static void fprint_device_enroll_start(FprintDevice *rdev,
	guint32 finger_num, DBusGMethodInvocation *context);
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

	/* whether we're running an identify, or a verify */
	FprintDeviceAction current_action;
};

typedef struct FprintDevicePrivate FprintDevicePrivate;

#define DEVICE_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE((o), FPRINT_TYPE_DEVICE, FprintDevicePrivate))

enum fprint_device_properties {
	FPRINT_DEVICE_CONSTRUCT_DDEV = 1,
	FPRINT_DEVICE_ACTION,
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
	case FPRINT_DEVICE_ACTION:
		g_value_set_int(value, priv->current_action);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
		break;
	}
}

static void fprint_device_set_action(FprintDevice *device, FprintDeviceAction action)
{
	FprintDevicePrivate *priv = DEVICE_GET_PRIVATE(device);

	if (priv->current_action == action)
		return;

	priv->current_action = action;
	g_object_notify (G_OBJECT(device), "action");
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
	pspec = g_param_spec_int("action", "Current device action",
				 "The current device action", ACTION_NONE, ACTION_ENROLL,
				 ACTION_NONE, G_PARAM_READABLE);
	g_object_class_install_property(gobject_class,
					FPRINT_DEVICE_ACTION, pspec);

	signals[SIGNAL_VERIFY_STATUS] = g_signal_new("verify-status",
		G_TYPE_FROM_CLASS(gobject_class), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
		g_cclosure_marshal_VOID__INT, G_TYPE_NONE, 1, G_TYPE_INT);
	signals[SIGNAL_ENROLL_STATUS] = g_signal_new("enroll-status",
		G_TYPE_FROM_CLASS(gobject_class), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
		g_cclosure_marshal_VOID__INT, G_TYPE_NONE, 1, G_TYPE_INT);
	signals[SIGNAL_VERIFY_FINGER_SELECTED] = g_signal_new("verify-finger-selected",
		G_TYPE_FROM_CLASS(gobject_class), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
		g_cclosure_marshal_VOID__INT, G_TYPE_NONE, 1, G_TYPE_INT);
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

	pk_action = polkit_action_new ();
	polkit_action_set_action_id (pk_action, action);
	pk_result = polkit_context_is_caller_authorized (priv->pol_ctx, pk_action, pk_caller,
							 TRUE, NULL);
	polkit_caller_unref (pk_caller);
	polkit_action_unref (pk_action);

	if (pk_result != POLKIT_RESULT_YES) {
		g_set_error (error, FPRINT_ERROR,
			     FPRINT_ERROR_INTERNAL,
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
		g_set_error(error, FPRINT_ERROR, FPRINT_ERROR_CLAIM_DEVICE,
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

		g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_CLAIM_DEVICE,
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
		g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_CLAIM_DEVICE,
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

	priv->username = user;
	priv->sender = sender;

	g_message ("user '%s' claiming the device: %d", priv->username, priv->id);

	priv->session = g_slice_new0(struct session_data);
	priv->session->context_claim_device = context;

	r = fp_async_dev_open(priv->ddev, dev_open_cb, rdev);
	if (r < 0) {
		g_slice_free(struct session_data, priv->session);
		priv->session = NULL;
		g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_CLAIM_DEVICE,
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
	g_free (priv->sender);
	priv->sender = NULL;
	g_free (priv->username);
	priv->username = NULL;
}

static void verify_cb(struct fp_dev *dev, int r, struct fp_img *img,
		      void *user_data)
{
	struct FprintDevice *rdev = user_data;
	g_message("verify_cb: result %d", r);

	g_signal_emit(rdev, signals[SIGNAL_VERIFY_STATUS], 0, r);
	fp_img_free(img);
}

static void identify_cb(struct fp_dev *dev, int r,
			 size_t match_offset, struct fp_img *img, void *user_data)
{
	struct FprintDevice *rdev = user_data;
	g_message("identify_cb: result %d", r);

	g_signal_emit(rdev, signals[SIGNAL_VERIFY_STATUS], 0, r);
	fp_img_free(img);
}

static void fprint_device_verify_start(FprintDevice *rdev,
	guint32 finger_num, DBusGMethodInvocation *context)
{
	FprintDevicePrivate *priv = DEVICE_GET_PRIVATE(rdev);
	struct fp_print_data **gallery = NULL;
	struct fp_print_data *data = NULL;
	GError *error = NULL;
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

	if (finger_num == -1) {
		GSList *prints;

		prints = store.discover_prints(priv->ddev, priv->username);
		if (prints == NULL) {
			g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_NO_SUCH_LOADED_PRINT,
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
				//FIXME r < 0 ?
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
			//FIXME exit
			g_message ("NO GALLERY");
			return;
		}
		fprint_device_set_action (rdev, ACTION_IDENTIFY);

		g_message ("start identification device %d", priv->id);
		//FIXME we're supposed to free the gallery here?
		r = fp_async_identify_start (priv->dev, gallery, identify_cb, rdev);
	} else {
		fprint_device_set_action (rdev, ACTION_VERIFY);

		g_message("start verification device %d finger %d", priv->id, finger_num);

		r = store.print_data_load(priv->dev, (enum fp_finger)finger_num, 
					  &data, priv->username);

		if (r < 0 || !data) {
			g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_NO_SUCH_LOADED_PRINT,
				    "No such print %d", finger_num);
			dbus_g_method_return_error(context, error);
			return;
		}

		/* FIXME fp_async_verify_start should copy the fp_print_data */
		r = fp_async_verify_start(priv->dev, data, verify_cb, rdev);
	}

	/* Emit VerifyFingerSelected telling the front-end which finger
	 * we selected for auth */
	g_signal_emit(rdev, signals[SIGNAL_VERIFY_FINGER_SELECTED], 0, finger_num);


	if (r < 0) {
		if (data != NULL) {
			fp_print_data_free (data);
		} else if (gallery != NULL) {
			guint i;
			for (i = 0; gallery[i] != NULL; i++)
				fp_print_data_free(gallery[i]);
			g_free (gallery);
		}
		g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_VERIFY_START,
			"Verify start failed with error %d", r);
		dbus_g_method_return_error(context, error);
		return;
	}

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
		r = fp_async_verify_stop(priv->dev, verify_stop_cb, context);
	} else if (priv->current_action == ACTION_IDENTIFY) {
		r = fp_async_identify_stop(priv->dev, identify_stop_cb, context);
	} else {
		g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_VERIFY_STOP,
			    "No verification in progress");
		dbus_g_method_return_error(context, error);
		g_error_free (error);
		return;
	}

	if (r < 0) {
		g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_VERIFY_STOP,
			"Verify stop failed with error %d", r);
		dbus_g_method_return_error(context, error);
		g_error_free (error);
	}

	fprint_device_set_action (rdev, ACTION_NONE);
}

static void enroll_stage_cb(struct fp_dev *dev, int result,
	struct fp_print_data *print, struct fp_img *img, void *user_data)
{
	struct FprintDevice *rdev = user_data;
	FprintDevicePrivate *priv = DEVICE_GET_PRIVATE(rdev);
	struct session_data *session = priv->session;
	int r;

	g_message("enroll_stage_cb: result %d", result);
	if (result == FP_ENROLL_COMPLETE) {
		r = store.print_data_save(print, session->enroll_finger, priv->username);
		if (r < 0)
			result = FP_ENROLL_FAIL;
	}

	g_signal_emit(rdev, signals[SIGNAL_ENROLL_STATUS], 0, result);
	fp_img_free(img);
	fp_print_data_free(print);
}

static void fprint_device_enroll_start(FprintDevice *rdev,
	guint32 finger_num, DBusGMethodInvocation *context)
{
	FprintDevicePrivate *priv = DEVICE_GET_PRIVATE(rdev);
	struct session_data *session = priv->session;
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
	
	r = fp_async_enroll_start(priv->dev, enroll_stage_cb, rdev);
	if (r < 0) {
		g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_ENROLL_START,
			"Enroll start failed with error %d", r);
		dbus_g_method_return_error(context, error);
		return;
	}

	fprint_device_set_action (rdev, ACTION_ENROLL);

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
		g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_ENROLL_STOP,
			    "No enrollment in progress");
		dbus_g_method_return_error(context, error);
		g_error_free (error);
		return;
	}

	r = fp_async_enroll_stop(priv->dev, enroll_stop_cb, context);
	if (r < 0) {
		g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_ENROLL_STOP,
			"Enroll stop failed with error %d", r);
		dbus_g_method_return_error(context, error);
		g_error_free (error);
	}

	fprint_device_set_action (rdev, ACTION_NONE);
}

static void fprint_device_list_enrolled_fingers(FprintDevice *rdev,
						const char *username,
						DBusGMethodInvocation *context)
{
	FprintDevicePrivate *priv = DEVICE_GET_PRIVATE(rdev);
	GError *error = NULL;
	GSList *prints;
	GSList *item;
	GArray *ret;
	char *user;

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

	prints = store.discover_prints(priv->ddev, user);
	g_free (user);
	if (!prints) {
		g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_DISCOVER_PRINTS,
			"Failed to discover prints");
		dbus_g_method_return_error(context, error);
		return;
	}

	ret = g_array_new(FALSE, FALSE, sizeof(int));
	for (item = prints; item; item = item->next) {
		ret = g_array_append_val(ret, item->data);
	}

	g_slist_free(prints);

	dbus_g_method_return(context, ret);
}

static void fprint_device_delete_enrolled_fingers(FprintDevice *rdev,
						  const char *username,
						  DBusGMethodInvocation *context)
{
	FprintDevicePrivate *priv = DEVICE_GET_PRIVATE(rdev);
	GError *error = NULL;
	guint i;
	char *user;

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

	for (i = LEFT_THUMB; i <= RIGHT_LITTLE; i++) {
		store.print_data_delete(priv->ddev, i, user);
	}
	g_free (user);

	dbus_g_method_return(context);
}

