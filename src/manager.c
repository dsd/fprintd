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

#include <dbus/dbus-glib-bindings.h>
#include <glib.h>
#include <libfprint/fprint.h>
#include <glib-object.h>

#include "fprintd.h"

static gboolean fprint_manager_get_devices(FprintManager *manager,
	GPtrArray **devices, GError **error);

#include "manager-dbus-glue.h"

static GObjectClass *parent_class = NULL;

static void manager_finalize(GObject *object)
{
	FprintManager *self = (FprintManager *) object;
	g_slist_free(self->dev_registry);
	G_OBJECT_CLASS(parent_class)->finalize(object);
}

static void manager_class_init(FprintManagerClass *klass)
{
	dbus_g_object_type_install_info(FPRINT_TYPE_MANAGER,
		&dbus_glib_fprint_manager_object_info);

	G_OBJECT_CLASS(klass)->finalize = manager_finalize;
	parent_class = g_type_class_peek_parent(klass);
}

GType fprint_manager_get_type(void)
{
	static GType type = 0;
	if (type == 0) {
		static const GTypeInfo info = {
			sizeof(FprintManagerClass),
			NULL,   /* base_init */
			NULL,   /* base_finalize */
			(GClassInitFunc) manager_class_init,
			NULL,   /* class_finalize */
			NULL,   /* class_data */
			sizeof(FprintManager),
			0,      /* n_preallocs */
			NULL,
		};
		type = g_type_register_static(G_TYPE_OBJECT, "FprintManagerType",
			&info, 0);
	}
	return type;
}

static gchar *get_device_path(FprintDevice *rdev)
{
	return g_strdup_printf("/net/reactivated/Fprint/Device/%d",
		_fprint_device_get_id(rdev));
}

FprintManager *fprint_manager_new(void)
{
	FprintManager *manager;
	struct fp_dscv_dev **discovered_devs = fp_discover_devs();
	struct fp_dscv_dev *ddev;
	int i = 0;

	/* FIXME some or all of this should probably be moved into FprintManager
	 * ctor. however i'm not sure how to raise errors from a constructor... */

	if (!discovered_devs)
		return NULL;

	manager = g_object_new(FPRINT_TYPE_MANAGER, NULL);
	dbus_g_connection_register_g_object(fprintd_dbus_conn,
		"/net/reactivated/Fprint/Manager", G_OBJECT(manager));

	while ((ddev = discovered_devs[i++]) != NULL) {
		FprintDevice *rdev = fprint_device_new(ddev);
		gchar *path;

		manager->dev_registry = g_slist_prepend(manager->dev_registry, rdev);
		path = get_device_path(rdev);
		dbus_g_connection_register_g_object(fprintd_dbus_conn, path,
			G_OBJECT(rdev));
		g_free(path);
	}

	return manager;
}

static gboolean fprint_manager_get_devices(FprintManager *manager,
	GPtrArray **devices, GError **error)
{
	GSList *elem = manager->dev_registry;
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

