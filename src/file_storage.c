/*
 * Simple file storage for fprintd
 * Copyright (C) 2007 Daniel Drake <dsd@gentoo.org>
 * Copyright (C) 2008 Vasily Khoruzhick <anarsoul@gmail.com>
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
 *
 */


/* FIXME:
 * This file almost duplicate data.c from libfprint
 * Maybe someday data.c will be upgraded to this one ;)
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gstdio.h>

#include <libfprint/fprint.h>

#include "file_storage.h"

#define DIR_PERMS 0700

#ifndef FILE_STORAGE_PATH
#define FILE_STORAGE_PATH "/var/lib/fprint/"
#endif

#define FP_FINGER_IS_VALID(finger) \
	((finger) >= LEFT_THUMB && (finger) <= RIGHT_LITTLE)

static char *get_path_to_storedir(uint16_t driver_id, uint32_t devtype, char *base_store)
{
	char idstr[5];
	char devtypestr[9];

	g_snprintf(idstr, sizeof(idstr), "%04x", driver_id);
	g_snprintf(devtypestr, sizeof(devtypestr), "%08x", devtype);

	return g_build_filename(base_store, idstr, devtypestr, NULL);
}

static char *__get_path_to_print(uint16_t driver_id, uint32_t devtype,
	enum fp_finger finger, char *base_store)
{
	char *dirpath;
	char *path;
	char fingername[2];

	g_snprintf(fingername, 2, "%x", finger);

	dirpath = get_path_to_storedir(driver_id, devtype, base_store);
	path = g_build_filename(dirpath, fingername, NULL);
	g_free(dirpath);
	return path;
}

static char *get_path_to_print(struct fp_dev *dev, enum fp_finger finger, char *base_store)
{
	return __get_path_to_print(fp_driver_get_driver_id(fp_dev_get_driver(dev)), 
		fp_dev_get_devtype(dev), finger, base_store);
}

static char *get_path_to_print_dscv(struct fp_dscv_dev *dev, enum fp_finger finger, char *base_store)
{
	return __get_path_to_print(fp_driver_get_driver_id(fp_dscv_dev_get_driver(dev)), 
		fp_dscv_dev_get_devtype(dev), finger, base_store);
}

static int file_storage_get_basestore_for_username(const char *username, char **base_store)
{
	char *dirpath = FILE_STORAGE_PATH;

	*base_store = g_build_filename(dirpath, username, NULL);
	return 0;
}

/* if username == NULL function will use current username */
int file_storage_print_data_save(struct fp_print_data *data,
	enum fp_finger finger, const char *username)
{
	GError *err = NULL;
	char *path, *dirpath, *buf;
	size_t len;
	int r;
	char *base_store = NULL;

	r = file_storage_get_basestore_for_username(username, &base_store);

	if (r < 0) {
		return r;
	}

	len = fp_print_data_get_data(data, (guchar **) &buf);
	if (!len) {
		g_free(base_store);
		return -ENOMEM;
	}

	path = __get_path_to_print(fp_print_data_get_driver_id(data), fp_print_data_get_devtype(data), finger, base_store);
	dirpath = g_path_get_dirname(path);
	r = g_mkdir_with_parents(dirpath, DIR_PERMS);
	if (r < 0) {
		g_free(base_store);
		g_free(path);
		g_free(dirpath);
		return r;
	}

	//fp_dbg("saving to %s", path);
	g_file_set_contents(path, buf, len, &err);
	free(buf);
	g_free(dirpath);
	g_free(path);
	if (err) {
		r = err->code;
		//fp_err("save failed: %s", err->message);
		g_error_free(err);
		/* FIXME interpret error codes */
		return r;
	}

	return 0;
}

static int load_from_file(char *path, struct fp_print_data **data)
{
	gsize length;
	char *contents;
	GError *err = NULL;
	struct fp_print_data *fdata;

	//fp_dbg("from %s", path);
	g_file_get_contents(path, &contents, &length, &err);
	if (err) {
		int r = err->code;
		g_error_free(err);
		/* FIXME interpret more error codes */
		if (r == G_FILE_ERROR_NOENT)
			return -ENOENT;
		else
			return r;
	}

	fdata = fp_print_data_from_data((guchar *) contents, length);
	g_free(contents);
	if (!fdata)
		return -EIO;
	*data = fdata;
	return 0;
}

int file_storage_print_data_load(struct fp_dev *dev,
	enum fp_finger finger, struct fp_print_data **data, const char *username)
{
	gchar *path;
	struct fp_print_data *fdata = NULL;
	int r;
	char *base_store = NULL;

	r = file_storage_get_basestore_for_username(username, &base_store);

	if (r < 0) {
		return r;
	}

	path = get_path_to_print(dev, finger, base_store);
	r = load_from_file(path, &fdata);
	g_free(path);
	g_free(base_store);
	if (r)
		return r;

	if (!fp_dev_supports_print_data(dev, fdata)) {
		fp_print_data_free(fdata);
		return -EINVAL;
	}

	*data = fdata;
	return 0;
}

int file_storage_print_data_delete(struct fp_dscv_dev *dev,
	enum fp_finger finger, const char *username)
{
	int r;
	char *base_store;

	r = file_storage_get_basestore_for_username(username, &base_store);

	if (r < 0) {
		return r;
	}

	gchar *path = get_path_to_print_dscv(dev, finger, base_store);

	r = g_unlink(path);
	g_free(path);
	g_free(base_store);

	/* FIXME: cleanup empty directory */
	return r;
}

static GSList *scan_dev_storedir(char *devpath, uint16_t driver_id,
	uint32_t devtype, GSList *list)
{
	GError *err = NULL;
	const gchar *ent;

	GDir *dir = g_dir_open(devpath, 0, &err);
	if (!dir) {
		//fp_err("opendir %s failed: %s", devpath, err->message);
		g_error_free(err);
		return list;
	}

	while ((ent = g_dir_read_name(dir))) {
		/* ent is an 1 hex character fp_finger code */
		guint64 val;
		gchar *endptr;

		if (*ent == 0 || strlen(ent) != 1)
			continue;

		val = g_ascii_strtoull(ent, &endptr, 16);
		if (endptr == ent || !FP_FINGER_IS_VALID(val)) {
			//fp_dbg("skipping print file %s", ent);
			continue;
		}

		list = g_slist_prepend(list, GINT_TO_POINTER(val));
	}

	g_dir_close(dir);
	return list;
}

GSList *file_storage_discover_prints(struct fp_dscv_dev *dev, const char *username)
{
	GSList *list = NULL;
	char *base_store = NULL;
	char *storedir = NULL;
	int r;

	
	r = file_storage_get_basestore_for_username(username, &base_store);

	if (r < 0) {
		return NULL;
	}

	storedir = get_path_to_storedir(fp_driver_get_driver_id(fp_dscv_dev_get_driver(dev)), 
		fp_dscv_dev_get_devtype(dev), base_store);

	list = scan_dev_storedir(storedir, fp_driver_get_driver_id(fp_dscv_dev_get_driver(dev)), 
		fp_dscv_dev_get_devtype(dev), list);

	g_free(base_store);
	g_free(storedir);

	return list;
}

int file_storage_init(void)
{
	/* Nothing to do */
	return 0;
}

int file_storage_deinit(void)
{
	/* Nothing to do */
	return 0;
}
