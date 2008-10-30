/*
 * Simple file storage for fprintd
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

#ifndef FILE_STORAGE_H

#define FILE_STORAGE_H

int file_storage_print_data_save(struct fp_print_data *data,
	enum fp_finger finger, const char *username);


int file_storage_print_data_load(struct fp_dev *dev,
	enum fp_finger finger, struct fp_print_data **data, const char *username);

int file_storage_print_data_delete(struct fp_dscv_dev *dev,
	enum fp_finger finger, const char *username);

int file_storage_init(void);

int file_storage_deinit(void);

GSList *file_storage_discover_prints(struct fp_dscv_dev *dev, const char *username);

#endif
