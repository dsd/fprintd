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

#include "config.h"

#include <poll.h>
#include <stdlib.h>

#include <dbus/dbus-glib-bindings.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <libfprint/fprint.h>
#include <glib-object.h>
#include <gmodule.h>

#include "fprintd.h"
#include "storage.h"
#include "file_storage.h"

extern DBusGConnection *fprintd_dbus_conn;
static gboolean no_timeout = FALSE;
static gboolean g_fatal_warnings = FALSE;

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

static void
set_storage_file (void)
{
	store.init = &file_storage_init;
	store.deinit = &file_storage_deinit;
	store.print_data_save = &file_storage_print_data_save;
	store.print_data_load = &file_storage_print_data_load;
	store.print_data_delete = &file_storage_print_data_delete;
	store.discover_prints = &file_storage_discover_prints;
}

static gboolean
load_storage_module (const char *module_name)
{
	GModule *module;
	char *filename;

	filename = g_module_build_path (PLUGINDIR, module_name);
	module = g_module_open (filename, 0);
	g_free (filename);
	if (module == NULL)
		return FALSE;

	if (!g_module_symbol (module, "init", (gpointer *) &store.init) ||
	    !g_module_symbol (module, "deinit", (gpointer *) &store.deinit) ||
	    !g_module_symbol (module, "print_data_save", (gpointer *) &store.print_data_save) ||
	    !g_module_symbol (module, "print_data_load", (gpointer *) &store.print_data_load) ||
	    !g_module_symbol (module, "print_data_delete", (gpointer *) &store.print_data_delete) ||
	    !g_module_symbol (module, "discover_prints", (gpointer *) &store.discover_prints)) {
	    	g_module_close (module);
	    	return FALSE;
	}

	g_module_make_resident (module);

	return TRUE;
}

static gboolean
load_conf (void)
{
	GKeyFile *file;
	char *filename;
	char *module_name;
	GError *error = NULL;
	gboolean ret;

	filename = g_build_filename (SYSCONFDIR, "fprintd.conf", NULL);
	file = g_key_file_new ();
	if (!g_key_file_load_from_file (file, filename, G_KEY_FILE_NONE, &error)) {
		g_print ("Could not open fprintd.conf: %s\n", error->message);
		goto bail;
	}

	g_free (filename);
	filename = NULL;

	module_name = g_key_file_get_string (file, "storage", "type", &error);
	if (module_name == NULL)
		goto bail;

	g_key_file_free (file);

	if (g_str_equal (module_name, "file")) {
		g_free (module_name);
		set_storage_file ();
		return TRUE;
	}

	ret = load_storage_module (module_name);
	g_free (module_name);

	return ret;

bail:
	g_key_file_free (file);
	g_free (filename);
	g_error_free (error);

	return FALSE;
}

static const GOptionEntry entries[] = {
	{"g-fatal-warnings", 0, 0, G_OPTION_ARG_NONE, &g_fatal_warnings, "Make all warnings fatal", NULL},
	{"no-timeout", 't', 0, G_OPTION_ARG_NONE, &no_timeout, "Do not exit after unused for a while", NULL},
	{ NULL }
};

int main(int argc, char **argv)
{
	GOptionContext *context;
	GMainLoop *loop;
	GError *error = NULL;
	FprintManager *manager;
	DBusGProxy *driver_proxy;
	guint32 request_name_ret;
	int r = 0;

	bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	context = g_option_context_new ("Fingerprint handler daemon");
	g_option_context_add_main_entries (context, entries, GETTEXT_PACKAGE);
	g_type_init();

	if (g_option_context_parse (context, &argc, &argv, &error) == FALSE) {
		g_print ("couldn't parse command-line options: %s\n", error->message);
		g_error_free (error);
		return 1;
	}

	if (g_fatal_warnings) {
		GLogLevelFlags fatal_mask;

		fatal_mask = g_log_set_always_fatal (G_LOG_FATAL_MASK);
		fatal_mask |= G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL;
		g_log_set_always_fatal (fatal_mask);
	}

	/* Load the configuration file,
	 * and the default storage plugin */
	if (!load_conf())
		set_storage_file ();
	store.init ();

	r = fp_init();
	if (r < 0) {
		g_error("fprint init failed with error %d\n", r);
		return r;
	}

	loop = g_main_loop_new(NULL, FALSE);

	r = setup_pollfds();
	if (r < 0) {
		g_print("pollfd setup failed\n");
		goto err;
	}

	g_print("Launching FprintObject\n");

	/* Obtain a connection to the session bus */
	fprintd_dbus_conn = dbus_g_bus_get(DBUS_BUS_SYSTEM, &error);
	if (fprintd_dbus_conn == NULL)
		g_error("Failed to open connection to bus: %s", error->message);

	/* create the one instance of the Manager object to be shared between
	 * all fprintd users */
	manager = fprint_manager_new(no_timeout);

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

