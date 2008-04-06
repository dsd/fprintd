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

#include <dbus/dbus-glib-bindings.h>
#include <glib.h>
#include <libfprint/fprint.h>
#include <glib-object.h>

#include "fprintd.h"
#include "storage.h"

static gboolean fprint_device_claim(FprintDevice *rdev,
	DBusGMethodInvocation *context);
static gboolean fprint_device_release(FprintDevice *rdev,
	DBusGMethodInvocation *context);
static gboolean fprint_device_list_enrolled_fingers(FprintDevice *rdev,
	GArray **fingers, GError **error);
static gboolean fprint_device_load_print_data(FprintDevice *rdev,
	guint32 finger_num, guint32 *print_id, GError **error);
static gboolean fprint_device_unload_print_data(FprintDevice *rdev,
	guint32 print_id, GError **error);
static gboolean fprint_device_verify_start(FprintDevice *rdev,
	guint32 print_id, GError **error);
static gboolean fprint_device_verify_stop(FprintDevice *rdev,
	DBusGMethodInvocation *context);
static gboolean fprint_device_enroll_start(FprintDevice *rdev,
	guint32 finger_num, GError **error);
static gboolean fprint_device_enroll_stop(FprintDevice *rdev,
	DBusGMethodInvocation *context);
static gboolean fprint_device_set_storage_type(FprintDevice *rdev,
	gint type);
static gboolean fprint_device_list_enrolled_fingers_from_storage(FprintDevice *rdev, 
	gchar *username, GArray **fingers, GError **error);
static gboolean fprint_device_load_print_data_from_storage(FprintDevice *rdev,
	guint32 finger_num, gchar *username, guint32 *print_id, GError **error);


#include "device-dbus-glue.h"

struct session_data {
	/* finger being enrolled */
	int enroll_finger;

	/* method invocation for async ClaimDevice() */
	DBusGMethodInvocation *context_claim_device;

	/* method invocation for async ReleaseDevice() */
	DBusGMethodInvocation *context_release_device;

	/* a list of loaded prints */
	GSList *loaded_prints;

};

struct loaded_print {
	guint32 id;
	struct fp_print_data *data;
};

struct FprintDevicePrivate {
	guint32 id;
	struct fp_dscv_dev *ddev;
	struct fp_dev *dev;
	struct session_data *session;

	/* type of storage */
	int storage_type;
};

typedef struct FprintDevicePrivate FprintDevicePrivate;

#define DEVICE_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE((o), FPRINT_TYPE_DEVICE, FprintDevicePrivate))

enum fprint_device_properties {
	FPRINT_DEVICE_CONSTRUCT_DDEV = 1,
};

enum fprint_device_signals {
	SIGNAL_VERIFY_STATUS,
	SIGNAL_ENROLL_STATUS,
	NUM_SIGNALS,
};

static GObjectClass *parent_class = NULL;
static guint32 last_id = ~0;
static guint signals[NUM_SIGNALS] = { 0, };

static void device_finalize(GObject *object)
{
	/* FIXME close and stuff */
}

static void device_set_property(GObject *object, guint property_id,
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

static void device_class_init(FprintDeviceClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
	GParamSpec *pspec;

	dbus_g_object_type_install_info(FPRINT_TYPE_DEVICE,
		&dbus_glib_fprint_device_object_info);
	parent_class = g_type_class_peek_parent(klass);

	gobject_class->finalize = device_finalize;
	gobject_class->set_property = device_set_property;
	g_type_class_add_private(klass, sizeof(FprintDevicePrivate));

	pspec = g_param_spec_pointer("discovered-dev", "Discovered device",
		"Set discovered device construction property",
		G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE);
	g_object_class_install_property(gobject_class,
		FPRINT_DEVICE_CONSTRUCT_DDEV, pspec);

	signals[SIGNAL_VERIFY_STATUS] = g_signal_new("verify-status",
		G_TYPE_FROM_CLASS(gobject_class), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
		g_cclosure_marshal_VOID__INT, G_TYPE_NONE, 1, G_TYPE_INT);
	signals[SIGNAL_ENROLL_STATUS] = g_signal_new("enroll-status",
		G_TYPE_FROM_CLASS(gobject_class), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
		g_cclosure_marshal_VOID__INT, G_TYPE_NONE, 1, G_TYPE_INT);
}

static void device_init(GTypeInstance *instance, gpointer g_class)
{
	FprintDevice *self = (FprintDevice *) instance;
	FprintDevicePrivate *priv = DEVICE_GET_PRIVATE(self);
	priv->id = ++last_id;
	priv->storage_type = FP_FILE_STORAGE;
	storages[priv->storage_type].init();
}

GType fprint_device_get_type(void)
{
	static GType type = 0;
	if (type == 0) {
		static const GTypeInfo info = {
			sizeof(FprintDeviceClass),
			NULL,   /* base_init */
			NULL,   /* base_finalize */
			(GClassInitFunc) device_class_init,
			NULL,   /* class_finalize */
			NULL,   /* class_data */
			sizeof(FprintDevice),
			0,      /* n_preallocs */
			device_init,
		};
		type = g_type_register_static(G_TYPE_OBJECT, "FprintDeviceType",
			&info, 0);
	}
	return type;
}

FprintDevice *fprint_device_new(struct fp_dscv_dev *ddev)
{
	return g_object_new(FPRINT_TYPE_DEVICE, "discovered-dev", ddev, NULL);	
}

guint32 _fprint_device_get_id(FprintDevice *rdev)
{
	return DEVICE_GET_PRIVATE(rdev)->id;
}

static void dev_open_cb(struct fp_dev *dev, int status, void *user_data)
{
	FprintDevice *rdev = user_data;
	FprintDevicePrivate *priv = DEVICE_GET_PRIVATE(rdev);
	struct session_data *session = priv->session;

	g_message("device %d claim status %d", priv->id, status);

	if (status != 0) {
		GError *error;
		g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_CLAIM_DEVICE,
			"Open failed with error %d", status);
		dbus_g_method_return_error(session->context_claim_device, error);
		return;
	}

	priv->dev = dev;
	dbus_g_method_return(session->context_claim_device);
}

static gboolean fprint_device_claim(FprintDevice *rdev,
	DBusGMethodInvocation *context)
{
	FprintDevicePrivate *priv = DEVICE_GET_PRIVATE(rdev);
	GError *error = NULL;
	int r;

	g_message("claiming device %d", priv->id);
	priv->session = g_slice_new0(struct session_data);
	priv->session->context_claim_device = context;

	r = fp_async_dev_open(priv->ddev, dev_open_cb, rdev);
	if (r < 0) {
		g_slice_free(struct session_data, priv->session);
		priv->session = NULL;
		g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_CLAIM_DEVICE,
			"Could not attempt device open, error %d", r);
		dbus_g_method_return_error(context, error);
		return FALSE;
	}

	return TRUE;
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

	g_message("released device %d", priv->id);
	dbus_g_method_return(context);
}

static gboolean fprint_device_release(FprintDevice *rdev,
	DBusGMethodInvocation *context)
{
	FprintDevicePrivate *priv = DEVICE_GET_PRIVATE(rdev);
	struct session_data *session = priv->session;
	GSList *elem = session->loaded_prints;

	/* Unload any loaded prints */
	if (elem) {
		do
			g_slice_free(struct loaded_print, elem->data);
		while ((elem = g_slist_next(elem)) != NULL);
		g_slist_free(session->loaded_prints);
	}

	session->context_release_device = context;
	fp_async_dev_close(priv->dev, dev_close_cb, rdev);
	return TRUE;
}

static gboolean fprint_device_list_enrolled_fingers(FprintDevice *rdev,
	GArray **fingers, GError **error)
{
	FprintDevicePrivate *priv = DEVICE_GET_PRIVATE(rdev);
	struct fp_dscv_print **prints;
	struct fp_dscv_print **print;
	GArray *ret;

	prints = fp_discover_prints();
	if (!prints) {
		g_set_error(error, FPRINT_ERROR, FPRINT_ERROR_DISCOVER_PRINTS,
			"Failed to discover prints");
		return FALSE;
	}

	ret = g_array_new(FALSE, FALSE, sizeof(int));
	for (print = prints; *print; print++)
		if (fp_dev_supports_dscv_print(priv->dev, *print)) {
			int finger = fp_dscv_print_get_finger(*print);
			ret = g_array_append_val(ret, finger);
		}

	fp_dscv_prints_free(prints);
	*fingers = ret;
	return TRUE;
}

static gboolean fprint_device_load_print_data(FprintDevice *rdev,
	guint32 finger_num, guint32 *print_id, GError **error)
{
	FprintDevicePrivate *priv = DEVICE_GET_PRIVATE(rdev);
	struct session_data *session = priv->session;
	struct loaded_print *lprint;
	struct fp_dscv_print **dprints = fp_discover_prints();
	struct fp_dscv_print **dprint;
	struct fp_dscv_print *selected_print = NULL;
	struct fp_print_data *data;
	int r;

	if (!dprints) {
		g_set_error(error, FPRINT_ERROR, FPRINT_ERROR_DISCOVER_PRINTS,
			"Failed to discover prints");
		return FALSE;
	}

	for (dprint = dprints; *dprint; dprint++)
		if (fp_dev_supports_dscv_print(priv->dev, *dprint)
				&& fp_dscv_print_get_finger(*dprint) == finger_num) {
			selected_print = *dprint;
			break;
		}
	
	if (!selected_print) {
		fp_dscv_prints_free(dprints);
		g_set_error(error, FPRINT_ERROR, FPRINT_ERROR_PRINT_NOT_FOUND,
			"Print not found");
		return FALSE;
	}

	r = fp_print_data_from_dscv_print(selected_print, &data);
	fp_dscv_prints_free(dprints);
	if (r < 0) {
		g_set_error(error, FPRINT_ERROR, FPRINT_ERROR_PRINT_LOAD,
			"Print load failed with error %d", r);
		return FALSE;
	}

	lprint = g_slice_new(struct loaded_print);
	lprint->data = data;
	lprint->id = ++last_id;
	session->loaded_prints = g_slist_prepend(session->loaded_prints, lprint);

	g_message("load print data finger %d for device %d = %d",
		finger_num, priv->id, lprint->id);
	*print_id = lprint->id;
	return TRUE;
}

static gboolean fprint_device_unload_print_data(FprintDevice *rdev,
	guint32 print_id, GError **error)
{
	FprintDevicePrivate *priv = DEVICE_GET_PRIVATE(rdev);
	struct session_data *session = priv->session;
	GSList *elem = session->loaded_prints;

	g_message("unload print data %d for device %d", print_id, priv->id);
	if (!elem) {
		g_set_error(error, FPRINT_ERROR, FPRINT_ERROR_NO_SUCH_LOADED_PRINT,
			"No such loaded print %d", print_id);
		return FALSE;
	}

	do {
		struct loaded_print *print = elem->data;
		if (print->id != print_id)
			continue;

		session->loaded_prints = g_slist_delete_link(session->loaded_prints,
			elem);
		g_slice_free(struct loaded_print, print);
		return TRUE;
	} while ((elem = g_slist_next(elem)) != NULL);

	g_set_error(error, FPRINT_ERROR, FPRINT_ERROR_NO_SUCH_LOADED_PRINT,
		"No such loaded print %d", print_id);
	return FALSE;
}

static void verify_cb(struct fp_dev *dev, int r, struct fp_img *img,
	void *user_data)
{
	struct FprintDevice *rdev = user_data;
	g_message("verify_cb: result %d", r);

	g_signal_emit(rdev, signals[SIGNAL_VERIFY_STATUS], 0, r);
	fp_img_free(img);
}

static gboolean fprint_device_verify_start(FprintDevice *rdev,
	guint32 print_id, GError **error)
{
	FprintDevicePrivate *priv = DEVICE_GET_PRIVATE(rdev);
	struct session_data *session = priv->session;
	GSList *elem = session->loaded_prints;
	struct fp_print_data *data = NULL;
	int r;

	g_message("start verification device %d print %d", priv->id, print_id);
	if (!elem) {
		g_set_error(error, FPRINT_ERROR, FPRINT_ERROR_NO_SUCH_LOADED_PRINT,
			"No such loaded print %d", print_id);
		return FALSE;
	}
	
	do {
		struct loaded_print *print = elem->data;
		if (print->id == print_id) {
			data = print->data;
			break;
		}
	} while ((elem = g_slist_next(elem)) != NULL);

	if (!data) {
		g_set_error(error, FPRINT_ERROR, FPRINT_ERROR_NO_SUCH_LOADED_PRINT,
			"No such loaded print %d", print_id);
		return FALSE;
	}

	/* FIXME check freeing/copying of data */
	r = fp_async_verify_start(priv->dev, data, verify_cb, rdev);
	if (r < 0) {
		g_set_error(error, FPRINT_ERROR, FPRINT_ERROR_VERIFY_START,
			"Verify start failed with error %d", r);
		return FALSE;
	}

	return TRUE;
}

static void verify_stop_cb(struct fp_dev *dev, void *user_data)
{
	dbus_g_method_return((DBusGMethodInvocation *) user_data);
}

static gboolean fprint_device_verify_stop(FprintDevice *rdev,
	DBusGMethodInvocation *context)
{
	FprintDevicePrivate *priv = DEVICE_GET_PRIVATE(rdev);
	int r;

	r = fp_async_verify_stop(priv->dev, verify_stop_cb, context);
	if (r < 0) {
		GError *error;
		g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_VERIFY_STOP,
			"Verify stop failed with error %d", r);
		dbus_g_method_return_error(context, error);
		return FALSE;
	}

	return TRUE;
}

static void enroll_stage_cb(struct fp_dev *dev, int result,
	struct fp_print_data *print, struct fp_img *img, void *user_data)
{
	struct FprintDevice *rdev = user_data;
	FprintDevicePrivate *priv = DEVICE_GET_PRIVATE(rdev);
	struct session_data *session = priv->session;

	g_message("enroll_stage_cb: result %d", result);
	if (result == FP_ENROLL_COMPLETE)
		fp_print_data_save(print, session->enroll_finger);

	g_signal_emit(rdev, signals[SIGNAL_ENROLL_STATUS], 0, result);
	fp_img_free(img);
	fp_print_data_free(print);
}

static gboolean fprint_device_enroll_start(FprintDevice *rdev,
	guint32 finger_num, GError **error)
{
	FprintDevicePrivate *priv = DEVICE_GET_PRIVATE(rdev);
	struct session_data *session = priv->session;
	int r;

	g_message("start enrollment device %d finger %d", priv->id, finger_num);
	session->enroll_finger = finger_num;
	
	r = fp_async_enroll_start(priv->dev, enroll_stage_cb, rdev);
	if (r < 0) {
		g_set_error(error, FPRINT_ERROR, FPRINT_ERROR_ENROLL_START,
			"Enroll start failed with error %d", r);
		return FALSE;
	}

	return TRUE;
}

static void enroll_stop_cb(struct fp_dev *dev, void *user_data)
{
	dbus_g_method_return((DBusGMethodInvocation *) user_data);
}

static gboolean fprint_device_enroll_stop(FprintDevice *rdev,
	DBusGMethodInvocation *context)
{
	FprintDevicePrivate *priv = DEVICE_GET_PRIVATE(rdev);
	int r;

	r = fp_async_enroll_stop(priv->dev, enroll_stop_cb, context);
	if (r < 0) {
		GError *error;
		g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_ENROLL_STOP,
			"Enroll stop failed with error %d", r);
		dbus_g_method_return_error(context, error);
		return FALSE;
	}

	return TRUE;
}

static gboolean fprint_device_set_storage_type(FprintDevice *rdev,
	gint type)
{
	FprintDevicePrivate *priv = DEVICE_GET_PRIVATE(rdev);

	if (type >= FP_STORAGES_COUNT) return FALSE;

	storages[priv->storage_type].deinit();
	priv->storage_type = type;
	storages[priv->storage_type].init();

	return TRUE;
}

static gboolean fprint_device_list_enrolled_fingers_from_storage(FprintDevice *rdev,
	gchar *username, GArray **fingers, GError **error)
{
	FprintDevicePrivate *priv = DEVICE_GET_PRIVATE(rdev);
	GSList *prints;
	GSList *item;
	GArray *ret;

	prints = storages[priv->storage_type].discover_prints(priv->dev, username);
	if (!prints) {
		g_set_error(error, FPRINT_ERROR, FPRINT_ERROR_DISCOVER_PRINTS,
			"Failed to discover prints");
		return FALSE;
	}

	ret = g_array_new(FALSE, FALSE, sizeof(int));
	for (item = prints; item; item = item->next) {
		int *fingerptr = (int *)item->data;
		ret = g_array_append_val(ret, *fingerptr);
	}

	g_slist_free(prints);
	*fingers = ret;
	return TRUE;
}

static gboolean fprint_device_load_print_data_from_storage(FprintDevice *rdev,
	guint32 finger_num, gchar *username, guint32 *print_id, GError **error)
{
	FprintDevicePrivate *priv = DEVICE_GET_PRIVATE(rdev);
	struct session_data *session = priv->session;
	struct loaded_print *lprint;
	struct fp_print_data *data;
	int r;

	r = storages[priv->storage_type].print_data_load(priv->dev, (enum fp_finger)finger_num, 
		&data, (char *)username);

	if (r < 0) {
		g_set_error(error, FPRINT_ERROR, FPRINT_ERROR_PRINT_LOAD,
			"Print load failed with error %d", r);
		return FALSE;
	}

	lprint = g_slice_new(struct loaded_print);
	lprint->data = data;
	lprint->id = ++last_id;
	session->loaded_prints = g_slist_prepend(session->loaded_prints, lprint);

	g_message("load print data finger %d for device %d = %d",
		finger_num, priv->id, lprint->id);
	*print_id = lprint->id;
	return TRUE;
}

