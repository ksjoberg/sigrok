/*
 * This file is part of the sigrok project.
 *
 * Copyright (C) 2011 Bert Vermeulen <bert@biot.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef SIGROK_SIGROK_INTERNAL_H
#define SIGROK_SIGROK_INTERNAL_H

/*--- hardware/common/serial.c ----------------------------------------------*/

GSList *list_serial_ports(void);
int serial_open(const char *pathname, int flags);
int serial_close(int fd);
int serial_flush(int fd);
int serial_write(int fd, const void *buf, size_t count);
int serial_read(int fd, void *buf, size_t count);
void *serial_backup_params(int fd);
void serial_restore_params(int fd, void *backup);
int serial_set_params(int fd, int speed, int bits, int parity, int stopbits,
		      int flowcontrol);

/*--- hardware/common/ezusb.c -----------------------------------------------*/

int ezusb_reset(struct libusb_device_handle *hdl, int set_clear);
int ezusb_install_firmware(libusb_device_handle *hdl, char *filename);
int ezusb_upload_firmware(libusb_device *dev, int configuration,
			  const char *filename);

/*--- hardware/common/misc.c ------------------------------------------------*/

int opendev2(int device_index, struct sigrok_device_instance **sdi,
	     libusb_device *dev, struct libusb_device_descriptor *des,
	     int *skip, uint16_t vid, uint16_t pid, int interface);
int opendev3(struct sigrok_device_instance **sdi, libusb_device *dev,
	     struct libusb_device_descriptor *des,
	     uint16_t vid, uint16_t pid, int interface);

#endif