/*
 * /net/reactivated/Fprint/Manager object implementation
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

#include <unistd.h>
#include <stdlib.h>
#include <dbus/dbus-glib-bindings.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <libfprint/fprint.h>
#include <glib-object.h>

#include "fprintd.h"

DBusGConnection *fprintd_dbus_conn;

static gboolean fprint_manager_get_devices(FprintManager *manager,
	GPtrArray **devices, GError **error);
static gboolean fprint_manager_get_default_device(FprintManager *manager,
	const char **device, GError **error);
#include "manager-dbus-glue.h"

static GObjectClass *parent_class = NULL;

G_DEFINE_TYPE(FprintManager, fprint_manager, G_TYPE_OBJECT);

typedef struct
{
	GSList *dev_registry;
	gboolean no_timeout;
	guint timeout_id;
} FprintManagerPrivate;

#define FPRINT_MANAGER_GET_PRIVATE(o)  \
	(G_TYPE_INSTANCE_GET_PRIVATE ((o), FPRINT_TYPE_MANAGER, FprintManagerPrivate))

static void fprint_manager_finalize(GObject *object)
{
	FprintManagerPrivate *priv = FPRINT_MANAGER_GET_PRIVATE (object);

	g_slist_free(priv->dev_registry);

	G_OBJECT_CLASS(parent_class)->finalize(object);
}

static void fprint_manager_class_init(FprintManagerClass *klass)
{
	dbus_g_object_type_install_info(FPRINT_TYPE_MANAGER,
					&dbus_glib_fprint_manager_object_info);
	dbus_g_error_domain_register (FPRINT_ERROR, FPRINT_ERROR_DBUS_INTERFACE, FPRINT_TYPE_ERROR);

	g_type_class_add_private ((GObjectClass *) klass, sizeof (FprintManagerPrivate));

	G_OBJECT_CLASS(klass)->finalize = fprint_manager_finalize;
	parent_class = g_type_class_peek_parent(klass);
}

static gchar *get_device_path(FprintDevice *rdev)
{
	return g_strdup_printf("/net/reactivated/Fprint/Device/%d",
		_fprint_device_get_id(rdev));
}

static gboolean
fprint_manager_timeout_cb (FprintManager *manager)
{
	g_message ("No devices in use, exit");
	//FIXME kill all the devices
	exit(0);
	return FALSE;
}

static void
fprint_manager_in_use_notified (FprintDevice *rdev, GParamSpec *spec, FprintManager *manager)
{
	FprintManagerPrivate *priv = FPRINT_MANAGER_GET_PRIVATE (manager);
	guint num_devices_used = 0;
	GSList *l;
	gboolean in_use;

	if (priv->timeout_id > 0) {
		g_source_remove (priv->timeout_id);
		priv->timeout_id = 0;
	}
	if (priv->no_timeout)
		return;

	for (l = priv->dev_registry; l != NULL; l = l->next) {
		FprintDevice *dev = l->data;

		g_object_get (G_OBJECT(dev), "in-use", &in_use, NULL);
		if (in_use != FALSE)
			num_devices_used++;
	}

	if (num_devices_used == 0)
		priv->timeout_id = g_timeout_add_seconds (TIMEOUT, (GSourceFunc) fprint_manager_timeout_cb, manager);
}

static void
fprint_manager_init (FprintManager *manager)
{
	FprintManagerPrivate *priv = FPRINT_MANAGER_GET_PRIVATE (manager);
	struct fp_dscv_dev **discovered_devs = fp_discover_devs();
	struct fp_dscv_dev *ddev;
	int i = 0;

	dbus_g_connection_register_g_object(fprintd_dbus_conn,
		"/net/reactivated/Fprint/Manager", G_OBJECT(manager));

	if (discovered_devs == NULL)
		return;

	while ((ddev = discovered_devs[i++]) != NULL) {
		FprintDevice *rdev = fprint_device_new(ddev);
		gchar *path;

		g_signal_connect (G_OBJECT(rdev), "notify::in-use",
				  G_CALLBACK (fprint_manager_in_use_notified), manager);

		priv->dev_registry = g_slist_prepend(priv->dev_registry, rdev);
		path = get_device_path(rdev);
		dbus_g_connection_register_g_object(fprintd_dbus_conn, path,
			G_OBJECT(rdev));
		g_free(path);
	}
}

FprintManager *fprint_manager_new(gboolean no_timeout)
{
	FprintManagerPrivate *priv;
	GObject *object;

	object = g_object_new(FPRINT_TYPE_MANAGER, NULL);
	priv = FPRINT_MANAGER_GET_PRIVATE (object);
	priv->no_timeout = no_timeout;

	if (!priv->no_timeout)
		priv->timeout_id = g_timeout_add_seconds (TIMEOUT, (GSourceFunc) fprint_manager_timeout_cb, object);

	return FPRINT_MANAGER (object);
}

static gboolean fprint_manager_get_devices(FprintManager *manager,
	GPtrArray **devices, GError **error)
{
	FprintManagerPrivate *priv = FPRINT_MANAGER_GET_PRIVATE (manager);
	GSList *elem = priv->dev_registry;
	int num_open = g_slist_length(elem);
	GPtrArray *devs = g_ptr_array_sized_new(num_open);

	if (num_open > 0)
		do {
			FprintDevice *rdev = elem->data;
			g_ptr_array_add(devs, get_device_path(rdev));
		} while ((elem = g_slist_next(elem)) != NULL);

	*devices = devs;
	return TRUE;
}

static gboolean fprint_manager_get_default_device(FprintManager *manager,
	const char **device, GError **error)
{
	FprintManagerPrivate *priv = FPRINT_MANAGER_GET_PRIVATE (manager);
	GSList *elem = priv->dev_registry;
	int num_open = g_slist_length(elem);

	if (num_open > 0) {
		*device = get_device_path (elem->data);
		return TRUE;
	} else {
		g_set_error (error, FPRINT_ERROR, FPRINT_ERROR_NO_SUCH_DEVICE,
			     "No devices available");
		*device = NULL;
		return FALSE;
	}
}

GQuark fprint_error_quark(void)
{
	static GQuark quark = 0;
	if (!quark)
		quark = g_quark_from_static_string("fprintd-error-quark");
	return quark;
}

#define ENUM_ENTRY(NAME, DESC) { NAME, "" #NAME "", DESC }
GType
fprint_error_get_type (void)
{
	static GType etype = 0;

	if (etype == 0) {
		static const GEnumValue values[] =
		{
			ENUM_ENTRY (FPRINT_ERROR_CLAIM_DEVICE, "ClaimDevice"),
			ENUM_ENTRY (FPRINT_ERROR_ALREADY_IN_USE, "AlreadyInUse"),
			ENUM_ENTRY (FPRINT_ERROR_INTERNAL, "Internal"),
			ENUM_ENTRY (FPRINT_ERROR_PERMISSION_DENIED, "PermissionDenied"),
			ENUM_ENTRY (FPRINT_ERROR_NO_ENROLLED_PRINTS, "NoEnrolledPrints"),
			ENUM_ENTRY (FPRINT_ERROR_NO_ACTION_IN_PROGRESS, "NoActionInProgress"),
			ENUM_ENTRY (FPRINT_ERROR_INVALID_FINGERNAME, "InvalidFingername"),
			ENUM_ENTRY (FPRINT_ERROR_NO_SUCH_DEVICE, "NoSuchDevice"),
			{ 0, 0, 0 }
		};
		etype = g_enum_register_static ("FprintError", values);
	}
	return etype;
}
