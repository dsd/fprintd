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

#include "fprintd.h"

DBusGConnection *fprintd_dbus_conn = NULL;

GQuark fprint_error_quark(void)
{
	static GQuark quark = 0;
	if (!quark)
		quark = g_quark_from_static_string("fprintd-error-quark");
	return quark;
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

int main(int argc, char **argv)
{
	GMainLoop *loop;
	GError *error = NULL;
	FprintManager *manager;
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

	g_print("Launching FprintObject\n");

	/* Obtain a connection to the session bus */
	fprintd_dbus_conn = dbus_g_bus_get(DBUS_BUS_SESSION, &error);
	if (fprintd_dbus_conn == NULL)
		g_error("Failed to open connection to bus: %s", error->message);

	/* create the one instance of the Manager object to be shared between
	 * all fprintd users */
	manager = fprint_manager_new();

	driver_proxy = dbus_g_proxy_new_for_name(fprintd_dbus_conn,
		DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS);

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

