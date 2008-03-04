/*
 * fprint D-Bus daemon
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

#include <poll.h>
#include <stdlib.h>

#include <dbus/dbus-glib-bindings.h>
#include <glib.h>
#include <libfprint/fprint.h>
#include <glib-object.h>

#define FPRINT_TYPE            (fprint_get_type())
#define FPRINT(object)         (G_TYPE_CHECK_INSTANCE_CAST((object), FPRINT_TYPE, Fprint))
#define FPRINT_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass), FPRINT_TYPE, FprintClass))
#define IS_FPRINT(object)      (G_TYPE_CHECK_INSTANCE_TYPE((object), FPRINT_TYPE))
#define IS_FPRINT_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), FPRINT_TYPE))
#define FPRINT_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS((obj), FPRINT_TYPE, FprintClass))

struct Fprint {
	GObject parent;
};

struct FprintClass {
	GObjectClass parent;
};

typedef struct Fprint Fprint;
typedef struct FprintClass FprintClass;

/* Generate the GObject boilerplate */
G_DEFINE_TYPE(Fprint, fprint, G_TYPE_OBJECT)

static gboolean fprint_list_devices(Fprint *fprint, GArray **ids,
	char ***drivers, char ***fullnames, GError **error);
static gboolean fprint_list_enrolled_fingers(Fprint *fprint, guint32 device_id,
	GArray **fingers, GError **error);
static gboolean fprint_load_print_data(Fprint *fprint, guint32 device_id,
	guint32 finger_num, guint32 *print_id, GError **error);
static gboolean fprint_unload_print_data(Fprint *fprint, guint32 device_id,
	guint32 print_id, GError **error);
static gboolean fprint_claim_device(Fprint *fprint, guint32 device_id,
	GError **error);
static gboolean fprint_release_device(Fprint *fprint, guint32 device_id,
	GError **error);
static gboolean fprint_verify_start(Fprint *fprint, guint32 device_id,
	guint32 finger, GError **error);
static gboolean fprint_get_verify_result(Fprint *fprint, guint32 device_id,
	DBusGMethodInvocation *minv);
static gboolean fprint_verify_stop(Fprint *fprint, guint32 device_id,
	DBusGMethodInvocation *minv);

#include "server-bindings.h"

#define FPRINT_SERVICE_NAME "net.reactivated.Fprint"

#define FPRINT_ERROR fprint_error_quark()
typedef enum {
	FPRINT_ERROR_NO_SUCH_DEVICE,
	FPRINT_ERROR_DISCOVER_PRINTS,
	FPRINT_ERROR_PRINT_NOT_FOUND,
	FPRINT_ERROR_PRINT_LOAD,
	FPRINT_ERROR_NO_SUCH_LOADED_PRINT,
	FPRINT_ERROR_VERIFY_START,
	FPRINT_ERROR_VERIFY_STOP,
	FPRINT_ERROR_FAILED,
} FprintError;

struct verify_result {
	int result;
	struct fp_img *img;
};

struct loaded_print {
	guint32 id;
	struct fp_print_data *data;
};

struct session_data {
	/* a list of pending verify results to be returned via GetVerifyResult() */
	GList *verify_results;

	/* method invocation for async GetVerifyResult() */
	DBusGMethodInvocation *minv_get_verify_result;

	/* a list of loaded prints */
	GSList *loaded_prints;
};

/* known devices are "registered" in a catalogue when they become known.
 * each registered device may be simply discovered, but is later opened when
 * claimed by an application and closed upon release.
 */
struct registered_dev {
	guint32 id;
	struct fp_dscv_dev *ddev;
	struct fp_dev *dev;
	struct session_data *session;
};

static GSList *dev_catalogue;
static guint32 last_id = ~0;

static GQuark fprint_error_quark(void)
{
	static GQuark quark = 0;
	if (!quark)
		quark = g_quark_from_static_string("fprintd-error-quark");
	return quark;
}

static void fprint_class_init(FprintClass *fprint_class)
{
	dbus_g_object_type_install_info(FPRINT_TYPE, &dbus_glib_fprint_object_info);
}

static void fprint_init(Fprint *fprint)
{
}

static struct registered_dev *find_rdev_by_id(guint32 device_id)
{
	GSList *elem = dev_catalogue;

	if (!elem)
		return NULL;

	do {
		struct registered_dev *rdev = elem->data;
		if (rdev->id == device_id)
			return rdev;
	} while ((elem = g_slist_next(elem)) != NULL);

	return NULL;
}

static gboolean fprint_list_devices(Fprint *fprint, GArray **ids,
	char ***drivers, char ***fullnames, GError **error)
{
	int num_open = g_slist_length(dev_catalogue);
	gchar **drvlist = g_malloc(sizeof(gchar *) * (num_open + 1));
	gchar **namelist = g_malloc(sizeof(gchar *) * (num_open + 1));
	GArray *idlist = g_array_sized_new(FALSE, FALSE, sizeof(guint32), num_open);
	GSList *elem = dev_catalogue;
	guint32 i = 0;

	g_message("list_devices request, returning %d results", num_open);
	drvlist[num_open] = NULL;
	namelist[num_open] = NULL;
	if (num_open == 0)
		goto out;

	do {
		struct registered_dev *rdev = elem->data;
		struct fp_dscv_dev *ddev = rdev->ddev;
		struct fp_driver *drv = fp_dscv_dev_get_driver(ddev);
		idlist = g_array_append_val(idlist, rdev->id);
		drvlist[i] = g_strdup(fp_driver_get_name(drv));
		namelist[i++] = g_strdup(fp_driver_get_full_name(drv));
	} while ((elem = g_slist_next(elem)) != NULL);

out:
	*ids = idlist;
	*drivers = drvlist;
	*fullnames = namelist;
	return TRUE;
}

static gboolean fprint_claim_device(Fprint *fprint, guint32 device_id,
	GError **error)
{
	struct registered_dev *rdev = find_rdev_by_id(device_id);
	if (!rdev) {
		g_set_error(error, FPRINT_ERROR, FPRINT_ERROR_NO_SUCH_DEVICE,
			"No such device %d", device_id);
		return FALSE;
	}

	/* FIXME async? */
	/* FIXME more error checking */
	rdev->dev = fp_dev_open(rdev->ddev);
	rdev->session = g_slice_new0(struct session_data);
	g_message("claimed device %d", device_id);
	return TRUE;
}

static gboolean fprint_release_device(Fprint *fprint, guint32 device_id,
	GError **error)
{
	struct registered_dev *rdev = find_rdev_by_id(device_id);
	struct session_data *session;
	GSList *elem;

	if (!rdev) {
		g_set_error(error, FPRINT_ERROR, FPRINT_ERROR_NO_SUCH_DEVICE,
			"No such device %d", device_id);
		return FALSE;
	}

	/* FIXME make async? */
	fp_dev_close(rdev->dev);
	rdev->dev = NULL;

	/* Unload any loaded prints */
	session = rdev->session;
	elem = session->loaded_prints;

	if (elem) {
		do
			g_slice_free(struct loaded_print, elem->data);
		while ((elem = g_slist_next(elem)) != NULL);
		g_slist_free(session->loaded_prints);
	}

	g_slice_free(struct session_data, rdev->session);
	rdev->session = NULL;
	g_message("released device %d", device_id);
	return TRUE;
}

static gboolean fprint_list_enrolled_fingers(Fprint *fprint, guint32 device_id,
	GArray **fingers, GError **error)
{
	struct registered_dev *rdev = find_rdev_by_id(device_id);
	struct fp_dscv_print **prints;
	struct fp_dscv_print **print;
	GArray *ret;

	if (!rdev) {
		g_set_error(error, FPRINT_ERROR, FPRINT_ERROR_NO_SUCH_DEVICE,
			"No such device %d", device_id);
		return FALSE;
	}

	prints = fp_discover_prints();
	if (!prints) {
		g_set_error(error, FPRINT_ERROR, FPRINT_ERROR_DISCOVER_PRINTS,
			"Failed to discover prints");
		return FALSE;
	}

	ret = g_array_new(FALSE, FALSE, sizeof(int));
	for (print = prints; *print; print++)
		if (fp_dev_supports_dscv_print(rdev->dev, *print)) {
			int finger = fp_dscv_print_get_finger(*print);
			ret = g_array_append_val(ret, finger);
		}

	fp_dscv_prints_free(prints);
	*fingers = ret;
	return TRUE;
}

static gboolean fprint_load_print_data(Fprint *fprint, guint32 device_id,
	guint32 finger_num, guint32 *print_id, GError **error)
{
	struct registered_dev *rdev = find_rdev_by_id(device_id);
	struct session_data *session;
	struct loaded_print *lprint;
	struct fp_dscv_print **dprints;
	struct fp_dscv_print **dprint;
	struct fp_dscv_print *selected_print = NULL;
	struct fp_print_data *data;
	int r;

	if (!rdev) {
		g_set_error(error, FPRINT_ERROR, FPRINT_ERROR_NO_SUCH_DEVICE,
			"No such device %d", device_id);
		return FALSE;
	}

	session = rdev->session;
	dprints = fp_discover_prints();
	if (!dprints) {
		g_set_error(error, FPRINT_ERROR, FPRINT_ERROR_DISCOVER_PRINTS,
			"Failed to discover prints");
		return FALSE;
	}

	for (dprint = dprints; *dprint; dprint++)
		if (fp_dev_supports_dscv_print(rdev->dev, *dprint)
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
		finger_num, device_id, lprint->id);
	*print_id = lprint->id;
	return TRUE;
}

static gboolean fprint_unload_print_data(Fprint *fprint, guint32 device_id,
	guint32 print_id, GError **error)
{
	struct registered_dev *rdev = find_rdev_by_id(device_id);
	struct session_data *session;
	GSList *elem;

	if (!rdev) {
		g_set_error(error, FPRINT_ERROR, FPRINT_ERROR_NO_SUCH_DEVICE,
			"No such device %d", device_id);
		return FALSE;
	}

	session = rdev->session;
	elem = session->loaded_prints;

	g_message("unload print data %d for device %d", print_id, device_id);
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
	struct registered_dev *rdev = user_data;
	struct session_data *session = rdev->session;
	g_message("verify_cb: result %d", r);

	if (session->minv_get_verify_result) {
		/* if we have an app waiting on a verify result, report it
		 * immediately */
		dbus_g_method_return(session->minv_get_verify_result, r, FALSE);
		fp_img_free(img);
		session->minv_get_verify_result = NULL;
	} else {
		/* otherwise nobody is listening. add it to the queue of pending
		 * results */
		struct verify_result *result = g_slice_new(struct verify_result);
		result->result = r;
		result->img = img;
		session->verify_results = g_list_append(session->verify_results,
			result);
	}
}

static gboolean fprint_verify_start(Fprint *fprint, guint32 device_id,
	guint32 print_id, GError **error)
{
	int r;
	struct registered_dev *rdev = find_rdev_by_id(device_id);
	struct session_data *session;
	struct fp_print_data *data = NULL;
	GSList *elem;

	if (!rdev) {
		g_set_error(error, FPRINT_ERROR, FPRINT_ERROR_NO_SUCH_DEVICE,
			"No such device %d", device_id);
		return FALSE;
	}

	session = rdev->session;
	elem = session->loaded_prints;

	g_message("start verification device %d print %d", device_id, print_id);
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
	r = fp_async_verify_start(rdev->dev, data, verify_cb, rdev);
	if (r < 0) {
		g_set_error(error, FPRINT_ERROR, FPRINT_ERROR_VERIFY_START,
			"Verify start failed with error %d", r);
		return FALSE;
	}
	
	return TRUE;
}

static gboolean fprint_get_verify_result(Fprint *fprint, guint32 device_id,
	DBusGMethodInvocation *minv)
{
	struct registered_dev *rdev = find_rdev_by_id(device_id);
	struct session_data *session;
	GList *elem;

	if (!rdev) {
		GError *error;
		g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_NO_SUCH_DEVICE,
			"No such device %d", device_id);
		dbus_g_method_return_error(minv, error);
		return FALSE;
	}

	session = rdev->session;
	elem = session->verify_results;

	if (elem == NULL) {
		/* no pending results, asynchronously wait for the next one */
		session->minv_get_verify_result = minv;
		g_message("get_verify_result: none pending, waiting for next one");
	} else {
		struct verify_result *result = elem->data;
		gboolean has_next = (g_list_next(elem) != NULL);
		g_message("GetVerifyResult: returning pending result %d",
			result->result);
		dbus_g_method_return(minv, result->result, has_next);
		fp_img_free(result->img);
		session->verify_results = g_list_delete_link(session->verify_results,
			elem);
		g_slice_free(struct verify_result, result);
	}

	return TRUE;
}

static void verify_stop_cb(struct fp_dev *dev, void *user_data)
{
	dbus_g_method_return((DBusGMethodInvocation *) user_data);
}

static gboolean fprint_verify_stop(Fprint *fprint, guint32 device_id,
	DBusGMethodInvocation *minv)
{
	struct registered_dev *rdev = find_rdev_by_id(device_id);
	struct session_data *session;
	GList *elem;
	int r;

	if (!rdev) {
		GError *error;
		g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_NO_SUCH_DEVICE,
			"No such device %d", device_id);
		dbus_g_method_return_error(minv, error);
		return FALSE;
	}

	/* Free all unreaped verify results */
	session = rdev->session;
	elem = session->verify_results;

	if (elem) {
		do {
			struct verify_result *result = elem->data;
			fp_img_free(result->img);
			g_slice_free(struct verify_result, result);
		} while ((elem = g_list_next(elem)) != NULL);
		g_list_free(session->verify_results);
		session->verify_results = NULL;
	}

	r = fp_async_verify_stop(rdev->dev, verify_stop_cb, minv);
	if (r < 0) {
		GError *error;
		g_set_error(&error, FPRINT_ERROR, FPRINT_ERROR_VERIFY_STOP,
			"Verify stop failed with error %d", r);
		dbus_g_method_return_error(minv, error);
		return FALSE;
	}

	return TRUE;
}

struct fdsource {
	GSource source;
	GSList *pollfds;
};

static gboolean source_prepare(GSource *source, gint *timeout)
{
	int r;
	struct timeval tv;

	r = fp_get_next_timeout(&tv);
	if (r == 0) {
		*timeout = -1;
		return FALSE;
	}

	if (!timerisset(&tv))
		return TRUE;

	*timeout = (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
	return FALSE;
}

static gboolean source_check(GSource *source)
{
	struct fdsource *_fdsource = (struct fdsource *) source;
	GSList *elem = _fdsource->pollfds;
	struct timeval tv;
	int r;

	if (!elem)
		return FALSE;

	do {
		GPollFD *pollfd = elem->data;
		if (pollfd->revents)
			return TRUE;
	} while ((elem = g_slist_next(elem)));

	r = fp_get_next_timeout(&tv);
	if (r == 1 && !timerisset(&tv))
		return TRUE;

	return FALSE;
}

static gboolean source_dispatch(GSource *source, GSourceFunc callback,
	gpointer data)
{
	struct timeval zerotimeout = {
		.tv_sec = 0,
		.tv_usec = 0,
	};

	/* FIXME error handling */
	fp_handle_events_timeout(&zerotimeout);

	/* FIXME whats the return value used for? */
	return TRUE;
}

static void source_finalize(GSource *source)
{
	struct fdsource *_fdsource = (struct fdsource *) source;
	GSList *elem = _fdsource->pollfds;

	if (elem)
		do {
			GPollFD *pollfd = elem->data;
			g_source_remove_poll((GSource *) _fdsource, pollfd);
			g_slice_free(GPollFD, pollfd);
			_fdsource->pollfds = g_slist_delete_link(_fdsource->pollfds, elem);
		} while ((elem = g_slist_next(elem)));

	g_slist_free(_fdsource->pollfds);
}

static GSourceFuncs sourcefuncs = {
	.prepare = source_prepare,
	.check = source_check,
	.dispatch = source_dispatch,
	.finalize = source_finalize,
};

static struct fdsource *fdsource = NULL;

static void pollfd_add(int fd, short events)
{
	GPollFD *pollfd = g_slice_new(GPollFD);
	pollfd->fd = fd;
	pollfd->events = 0;
	pollfd->revents = 0;
	if (events & POLLIN)
		pollfd->events |= G_IO_IN;
	if (events & POLLOUT)
		pollfd->events |= G_IO_OUT;

	fdsource->pollfds = g_slist_prepend(fdsource->pollfds, pollfd);
	g_source_add_poll((GSource *) fdsource, pollfd);
}

static void pollfd_added_cb(int fd, short events)
{
	g_message("now monitoring fd %d", fd);
	pollfd_add(fd, events);
}

static void pollfd_removed_cb(int fd)
{
	GSList *elem = fdsource->pollfds;
	g_message("no longer monitoring fd %d", fd);

	if (!elem) {
		g_warning("cannot remove from list as list is empty?");
		return;
	}

	do {
		GPollFD *pollfd = elem->data;
		if (pollfd->fd != fd)
			continue;

		g_source_remove_poll((GSource *) fdsource, pollfd);
		g_slice_free(GPollFD, pollfd);
		fdsource->pollfds = g_slist_delete_link(fdsource->pollfds, elem);
		return;
	} while ((elem = g_slist_next(elem)));
	
	g_error("couldn't find fd %d in list\n", fd);
}

static int setup_pollfds(void)
{
	size_t numfds;
	size_t i;
	struct fp_pollfd *fpfds;
	GSource *gsource = g_source_new(&sourcefuncs, sizeof(struct fdsource));

	fdsource = (struct fdsource *) gsource;
	fdsource->pollfds = NULL;

	numfds = fp_get_pollfds(&fpfds);
	if (numfds < 0) {
		if (fpfds)
			free(fpfds);
		return (int) numfds;
	} else if (numfds > 0) {
		for (i = 0; i < numfds; i++) {
			struct fp_pollfd *fpfd = &fpfds[i];
			pollfd_add(fpfd->fd, fpfd->events);
		}
	}

	free(fpfds);
	fp_set_pollfd_notifiers(pollfd_added_cb, pollfd_removed_cb);
	g_source_attach(gsource, NULL);
	return 0;
}

static int find_devices(void)
{
	struct fp_dscv_dev **discovered_devs = fp_discover_devs();
	struct fp_dscv_dev *ddev;
	int i = 0;

	if (!discovered_devs)
		return -1;

	while ((ddev = discovered_devs[i++]) != NULL) {
		struct fp_driver *drv = fp_dscv_dev_get_driver(ddev);
		struct registered_dev *rdev = g_slice_new0(struct registered_dev);
		g_message("found device %s", fp_driver_get_full_name(drv));
		rdev->id = ++last_id;
		rdev->ddev = ddev;
		dev_catalogue = g_slist_prepend(dev_catalogue, rdev);
	}
	return 0;
}

int main(int argc, char **argv)
{
	GMainLoop *loop;
	DBusGConnection *connection;
	GError *error = NULL;
	GObject *obj;
	DBusGProxy *driver_proxy;
	guint32 request_name_ret;
	int r = 0;

	r = fp_init();
	if (r < 0) {
		g_error("fprint init failed with error %d\n", r);
		return r;
	}

	g_type_init();
	loop = g_main_loop_new(NULL, FALSE);

	r = setup_pollfds();
	if (r < 0) {
		g_print("pollfd setup failed\n");
		goto err;
	}

	r = find_devices();
	if (r < 0) {
		g_print("open devices failed\n");
		goto err;
	}

	g_print("Launching FprintObject\n");

	/* Obtain a connection to the session bus */
	connection = dbus_g_bus_get(DBUS_BUS_SESSION, &error);
	if (connection == NULL)
		g_error("Failed to open connection to bus: %s", error->message);

	obj = g_object_new(FPRINT_TYPE, NULL);
	dbus_g_connection_register_g_object(connection, "/net/reactivated/Fprint",
		obj);

	driver_proxy = dbus_g_proxy_new_for_name(connection, DBUS_SERVICE_DBUS,
		DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS);

	if (!org_freedesktop_DBus_request_name(driver_proxy, FPRINT_SERVICE_NAME,
			0, &request_name_ret, &error))
		g_error("Failed to get name: %s", error->message);

	if (request_name_ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
		g_error ("Got result code %u from requesting name", request_name_ret);
		exit(1);
	}

	g_message("D-Bus service launched with name: %s", FPRINT_SERVICE_NAME);

	g_message("entering main loop");
	g_main_loop_run(loop);
	g_message("main loop completed");

err:
	fp_exit();
	return 0;
}

