/*
 * This file is part of the sigrok project.
 *
 * Copyright (C) 2011 Uwe Hermann <uwe@hermann-uwe.de>
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include <ftdi.h>
#include <glib.h>
#include <string.h>
#include <sigrok.h>
#include <sigrok-internal.h>

#define USB_VENDOR_ID			0x0403
#define USB_PRODUCT_ID			0x6001
#define USB_DESCRIPTION			"ChronoVu LA8"
#define USB_VENDOR_NAME			"ChronoVu"
#define USB_MODEL_NAME			"LA8"
#define USB_MODEL_VERSION		""

#define NUM_PROBES			8
#define TRIGGER_TYPES			"01"
#define SDRAM_SIZE			(8 * 1024 * 1024)
#define MIN_NUM_SAMPLES			1

#define BS				4096 /* Block size */
#define NUM_BLOCKS			2048 /* Number of blocks */

static GSList *device_instances = NULL;

struct la8 {
	/** FTDI device context (used by libftdi). */
	struct ftdi_context *ftdic;

	/** The currently configured samplerate of the device. */
	uint64_t cur_samplerate;

	/** period in picoseconds corresponding to the samplerate */
	uint64_t period_ps;

	/** The current sampling limit (in ms). */
	uint64_t limit_msec;

	/** The current sampling limit (in number of samples). */
	uint64_t limit_samples;

	/** TODO */
	gpointer session_id;

	/**
	 * A buffer containing some (mangled) samples from the device.
	 * Format: Pretty mangled-up (due to hardware reasons), see code.
	 */
	uint8_t mangled_buf[BS];

	/**
	 * An 8MB buffer where we'll store the de-mangled samples.
	 * Format: Each sample is 1 byte, MSB is channel 7, LSB is channel 0.
	 */
	uint8_t *final_buf;

	/**
	 * Trigger pattern (MSB = channel 7, LSB = channel 0).
	 * A 1 bit matches a high signal, 0 matches a low signal on a probe.
	 * Only low/high triggers (but not e.g. rising/falling) are supported.
	 */
	uint8_t trigger_pattern;

	/**
	 * Trigger mask (MSB = channel 7, LSB = channel 0).
	 * A 1 bit means "must match trigger_pattern", 0 means "don't care".
	 */
	uint8_t trigger_mask;

	/** Time (in seconds) before the trigger times out. */
	uint64_t trigger_timeout;

	/** Tells us whether an SR_DF_TRIGGER packet was already sent. */
	int trigger_found;

	/** TODO */
	time_t done;

	/** Counter/index for the data block to be read. */
	int block_counter;

	/** The divcount value (determines the sample period) for the LA8. */
	uint8_t divcount;
};

/* This will be initialized via hw_get_device_info()/SR_DI_SAMPLERATES. */
static uint64_t supported_samplerates[255 + 1] = { 0 };

/*
 * Min: 1 sample per 0.01us -> sample time is 0.084s, samplerate 100MHz
 * Max: 1 sample per 2.55us -> sample time is 21.391s, samplerate 392.15kHz
 */
static struct sr_samplerates samplerates = {
	.low  = 0,
	.high = 0,
	.step = 0,
	.list = supported_samplerates,
};

/* Note: Continuous sampling is not supported by the hardware. */
static int capabilities[] = {
	SR_HWCAP_LOGIC_ANALYZER,
	SR_HWCAP_SAMPLERATE,
	SR_HWCAP_LIMIT_MSEC, /* TODO: Not yet implemented. */
	SR_HWCAP_LIMIT_SAMPLES, /* TODO: Not yet implemented. */
	0,
};

/* Function prototypes. */
static int la8_close_usb_reset_sequencer(struct la8 *la8);
static void hw_stop_acquisition(int device_index, gpointer session_data);
static int la8_reset(struct la8 *la8);

static void fill_supported_samplerates_if_needed(void)
{
	int i;

	/* Do nothing if supported_samplerates[] is already filled. */
	if (supported_samplerates[0] != 0)
		return;

	/* Fill supported_samplerates[] with the proper values. */
	for (i = 0; i < 255; i++)
		supported_samplerates[254 - i] = SR_MHZ(100) / (i + 1);
	supported_samplerates[255] = 0;
}

/**
 * Check if the given samplerate is supported by the LA8 hardware.
 *
 * @param samplerate The samplerate (in Hz) to check.
 * @return 1 if the samplerate is supported/valid, 0 otherwise.
 */
static int is_valid_samplerate(uint64_t samplerate)
{
	int i;

	fill_supported_samplerates_if_needed();

	for (i = 0; i < 255; i++) {
		if (supported_samplerates[i] == samplerate)
			return 1;
	}

	sr_warn("la8: %s: invalid samplerate (%" PRIu64 "Hz)",
		__func__, samplerate);

	return 0;
}

/**
 * Convert a samplerate (in Hz) to the 'divcount' value the LA8 wants.
 *
 * LA8 hardware: sample period = (divcount + 1) * 10ns.
 * Min. value for divcount: 0x00 (10ns sample period, 100MHz samplerate).
 * Max. value for divcount: 0xfe (2550ns sample period, 392.15kHz samplerate).
 *
 * @param samplerate The samplerate in Hz.
 * @return The divcount value as needed by the hardware, or 0xff upon errors.
 */
static uint8_t samplerate_to_divcount(uint64_t samplerate)
{
	if (samplerate == 0) {
		sr_err("la8: %s: samplerate was 0", __func__);
		return 0xff;
	}

	if (!is_valid_samplerate(samplerate)) {
		sr_err("la8: %s: can't get divcount, samplerate invalid",
		       __func__);
		return 0xff;
	}

	return (SR_MHZ(100) / samplerate) - 1;
}

/**
 * Write data of a certain length to the LA8's FTDI device.
 *
 * @param la8 The LA8 struct containing private per-device-instance data.
 * @param buf The buffer containing the data to write.
 * @param size The number of bytes to write.
 * @return The number of bytes written, or a negative value upon errors.
 */
static int la8_write(struct la8 *la8, uint8_t *buf, int size)
{
	int bytes_written;

	if (!la8) {
		sr_err("la8: %s: la8 was NULL", __func__);
		return SR_ERR_ARG;
	}

	if (!la8->ftdic) {
		sr_err("la8: %s: la8->ftdic was NULL", __func__);
		return SR_ERR_ARG;
	}

	if (!buf) {
		sr_err("la8: %s: buf was NULL", __func__);
		return SR_ERR_ARG;
	}

	if (size < 0) {
		sr_err("la8: %s: size was < 0", __func__);
		return SR_ERR_ARG;
	}

	bytes_written = ftdi_write_data(la8->ftdic, buf, size);

	if (bytes_written < 0) {
		sr_warn("la8: %s: ftdi_write_data: (%d) %s", __func__,
			bytes_written, ftdi_get_error_string(la8->ftdic));
		(void) la8_close_usb_reset_sequencer(la8); /* Ignore errors. */
	} else if (bytes_written != size) {
		sr_warn("la8: %s: bytes to write: %d, bytes written: %d",
			__func__, size, bytes_written);
		(void) la8_close_usb_reset_sequencer(la8); /* Ignore errors. */
	}

	return bytes_written;
}

/**
 * Read a certain amount of bytes from the LA8's FTDI device.
 *
 * @param la8 The LA8 struct containing private per-device-instance data.
 * @param buf The buffer where the received data will be stored.
 * @param size The number of bytes to read.
 * @return The number of bytes read, or a negative value upon errors.
 */
static int la8_read(struct la8 *la8, uint8_t *buf, int size)
{
	int bytes_read;

	if (!la8) {
		sr_err("la8: %s: la8 was NULL", __func__);
		return SR_ERR_ARG;
	}

	if (!la8->ftdic) {
		sr_err("la8: %s: la8->ftdic was NULL", __func__);
		return SR_ERR_ARG;
	}

	if (!buf) {
		sr_err("la8: %s: buf was NULL", __func__);
		return SR_ERR_ARG;
	}

	if (size <= 0) {
		sr_err("la8: %s: size was <= 0", __func__);
		return SR_ERR_ARG;
	}

	bytes_read = ftdi_read_data(la8->ftdic, buf, size);

	if (bytes_read < 0) {
		sr_warn("la8: %s: ftdi_read_data: (%d) %s", __func__,
			bytes_read, ftdi_get_error_string(la8->ftdic));
	} else if (bytes_read != size) {
		// sr_warn("la8: %s: bytes to read: %d, bytes read: %d",
		//	__func__, size, bytes_read);
	}

	return bytes_read;
}

static int la8_close(struct la8 *la8)
{
	int ret;

	if (!la8) {
		sr_err("la8: %s: la8 was NULL", __func__);
		return SR_ERR_ARG;
	}

	if (!la8->ftdic) {
		sr_err("la8: %s: la8->ftdic was NULL", __func__);
		return SR_ERR_ARG;
	}

	if ((ret = ftdi_usb_close(la8->ftdic)) < 0) {
		sr_warn("la8: %s: ftdi_usb_close: (%d) %s",
			__func__, ret, ftdi_get_error_string(la8->ftdic));
	}

	return ret;
}

/**
 * Close the ChronoVu LA8 USB port and reset the LA8 sequencer logic.
 *
 * @param la8 The LA8 struct containing private per-device-instance data.
 * @return SR_OK upon success, SR_ERR upon failure.
 */
static int la8_close_usb_reset_sequencer(struct la8 *la8)
{
	/* Magic sequence of bytes for resetting the LA8 sequencer logic. */
	uint8_t buf[8] = {0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01};
	int ret;

	sr_spew("la8: entering %s", __func__);

	if (!la8) {
		sr_err("la8: %s: la8 was NULL", __func__);
		return SR_ERR_ARG;
	}

	if (!la8->ftdic) {
		sr_err("la8: %s: la8->ftdic was NULL", __func__);
		return SR_ERR_ARG;
	}

	if (la8->ftdic->usb_dev) {
		/* Reset the LA8 sequencer logic, then wait 100ms. */
		sr_dbg("la8: resetting sequencer logic");
		(void) la8_write(la8, buf, 8); /* Ignore errors. */
		g_usleep(100 * 1000);

		/* Purge FTDI buffers, then reset and close the FTDI device. */
		sr_dbg("la8: purging buffers, resetting+closing FTDI device");

		/* Log errors, but ignore them (i.e., don't abort). */
		if ((ret = ftdi_usb_purge_buffers(la8->ftdic)) < 0)
			sr_warn("la8: %s: ftdi_usb_purge_buffers: (%d) %s",
			    __func__, ret, ftdi_get_error_string(la8->ftdic));
		if ((ret = ftdi_usb_reset(la8->ftdic)) < 0)
			sr_warn("la8: %s: ftdi_usb_reset: (%d) %s", __func__,
				ret, ftdi_get_error_string(la8->ftdic));
		if ((ret = ftdi_usb_close(la8->ftdic)) < 0)
			sr_warn("la8: %s: ftdi_usb_close: (%d) %s", __func__,
				ret, ftdi_get_error_string(la8->ftdic));
	} else {
		sr_spew("la8: %s: usb_dev was NULL, nothing to do", __func__);
	}

	ftdi_free(la8->ftdic); /* Returns void. */
	la8->ftdic = NULL;

	return SR_OK;
}

/**
 * Reset the ChronoVu LA8.
 *
 * The LA8 must be reset after a failed read/write operation or upon timeouts.
 *
 * @param la8 The LA8 struct containing private per-device-instance data.
 * @return SR_OK upon success, SR_ERR upon failure.
 */
static int la8_reset(struct la8 *la8)
{
	uint8_t buf[BS];
	time_t done, now;
	int bytes_read;

	if (!la8) {
		sr_err("la8: %s: la8 was NULL", __func__);
		return SR_ERR_ARG;
	}

	if (!la8->ftdic) {
		sr_err("la8: %s: la8->ftdic was NULL", __func__);
		return SR_ERR_ARG;
	}

	sr_dbg("la8: resetting the device");

	/*
	 * Purge pending read data from the FTDI hardware FIFO until
	 * no more data is left, or a timeout occurs (after 20s).
	 */
	done = 20 + time(NULL);
	do {
		/* TODO: Ignore errors? Check for < 0 at least! */
		bytes_read = la8_read(la8, (uint8_t *)&buf, BS);
		now = time(NULL);
	} while ((done > now) && (bytes_read > 0));

	/* Reset the LA8 sequencer logic and close the USB port. */
	(void) la8_close_usb_reset_sequencer(la8); /* Ignore errors. */

	sr_dbg("la8: device reset finished");

	return SR_OK;
}

static int configure_probes(struct la8 *la8, GSList *probes)
{
	struct sr_probe *probe;
	GSList *l;
	uint8_t probe_bit;
	char *tc;

	la8->trigger_pattern = 0;
	la8->trigger_mask = 0; /* Default to "don't care" for all probes. */

	for (l = probes; l; l = l->next) {
		probe = (struct sr_probe *)l->data;

		if (!probe) {
			sr_err("la8: %s: probe was NULL", __func__);
			return SR_ERR;
		}

		/* Skip disabled probes. */
		if (!probe->enabled)
			continue;

		/* Skip (enabled) probes with no configured trigger. */
		if (!probe->trigger)
			continue;

		/* Note: Must only be run if probe->trigger != NULL. */
		if (probe->index < 0 || probe->index > 7) {
			sr_err("la8: %s: invalid probe index %d, must be "
			       "between 0 and 7", __func__, probe->index);
			return SR_ERR;
		}

		probe_bit = (1 << (probe->index - 1));

		/* Configure the probe's trigger mask and trigger pattern. */
		for (tc = probe->trigger; tc && *tc; tc++) {
			la8->trigger_mask |= probe_bit;

			/* Sanity check, LA8 only supports low/high trigger. */
			if (*tc != '0' && *tc != '1') {
				sr_err("la8: %s: invalid trigger '%c', only "
				       "'0'/'1' supported", __func__, *tc);
				return SR_ERR;
			}

			if (*tc == '1')
				la8->trigger_pattern |= probe_bit;
		}
	}

	sr_dbg("la8: %s: trigger_mask = 0x%x, trigger_pattern = 0x%x",
	       __func__, la8->trigger_mask, la8->trigger_pattern);

	return SR_OK;
}

static int hw_init(const char *deviceinfo)
{
	int ret;
	struct sr_device_instance *sdi;
	struct la8 *la8;

	sr_spew("la8: entering %s", __func__);

	/* Avoid compiler errors. */
	deviceinfo = deviceinfo;

	/* Allocate memory for our private driver context. */
	if (!(la8 = g_try_malloc(sizeof(struct la8)))) {
		sr_err("la8: %s: struct la8 malloc failed", __func__);
		goto err_free_nothing;
	}

	/* Set some sane defaults. */
	la8->ftdic = NULL;
	la8->cur_samplerate = SR_MHZ(100); /* 100MHz == max. samplerate */
	la8->period_ps = 10000;
	la8->limit_msec = 0;
	la8->limit_samples = 0;
	la8->session_id = NULL;
	memset(la8->mangled_buf, 0, BS);
	la8->final_buf = NULL;
	la8->trigger_pattern = 0x00; /* Value irrelevant, see trigger_mask. */
	la8->trigger_mask = 0x00; /* All probes are "don't care". */
	la8->trigger_timeout = 10; /* Default to 10s trigger timeout. */
	la8->trigger_found = 0;
	la8->done = 0;
	la8->block_counter = 0;
	la8->divcount = 0; /* 10ns sample period == 100MHz samplerate */

	/* Allocate memory where we'll store the de-mangled data. */
	if (!(la8->final_buf = g_try_malloc(SDRAM_SIZE))) {
		sr_err("la8: %s: final_buf malloc failed", __func__);
		goto err_free_la8;
	}

	/* Allocate memory for the FTDI context (ftdic) and initialize it. */
	if (!(la8->ftdic = ftdi_new())) {
		sr_err("la8: %s: ftdi_new failed", __func__);
		goto err_free_final_buf;
	}

	/* Check for the device and temporarily open it. */
	if ((ret = ftdi_usb_open_desc(la8->ftdic, USB_VENDOR_ID,
			USB_PRODUCT_ID, USB_DESCRIPTION, NULL)) < 0) {
		sr_dbg("la8: %s: ftdi_usb_open_desc: (%d) %s",
		       __func__, ret, ftdi_get_error_string(la8->ftdic));
		(void) la8_close_usb_reset_sequencer(la8); /* Ignore errors. */
		goto err_free_ftdic;
	}
	sr_dbg("la8: found device");

	/* Register the device with libsigrok. */
	sdi = sr_device_instance_new(0, SR_ST_INITIALIZING,
			USB_VENDOR_NAME, USB_MODEL_NAME, USB_MODEL_VERSION);
	if (!sdi) {
		sr_err("la8: %s: sr_device_instance_new failed", __func__);
		goto err_close_ftdic;
	}

	sdi->priv = la8;

	device_instances = g_slist_append(device_instances, sdi);

	sr_spew("la8: %s finished successfully", __func__);

	/* Close device. We'll reopen it again when we need it. */
	(void) la8_close(la8); /* Log, but ignore errors. */

	return 1;

err_close_ftdic:
	(void) la8_close(la8); /* Log, but ignore errors. */
err_free_ftdic:
	free(la8->ftdic); /* NOT g_free()! */
err_free_final_buf:
	g_free(la8->final_buf);
err_free_la8:
	g_free(la8);
err_free_nothing:

	return 0;
}

static int hw_opendev(int device_index)
{
	int ret;
	struct sr_device_instance *sdi;
	struct la8 *la8;

	if (!(sdi = sr_get_device_instance(device_instances, device_index))) {
		sr_err("la8: %s: sdi was NULL", __func__);
		return SR_ERR; /* TODO: SR_ERR_ARG? */
	}

	if (!(la8 = sdi->priv)) {
		sr_err("la8: %s: sdi->priv was NULL", __func__);
		return SR_ERR; /* TODO: SR_ERR_ARG? */
	}

	sr_dbg("la8: opening device");

	/* Open the device. */
	if ((ret = ftdi_usb_open_desc(la8->ftdic, USB_VENDOR_ID,
			USB_PRODUCT_ID, USB_DESCRIPTION, NULL)) < 0) {
		sr_err("la8: %s: ftdi_usb_open_desc: (%d) %s",
		       __func__, ret, ftdi_get_error_string(la8->ftdic));
		(void) la8_close_usb_reset_sequencer(la8); /* Ignore errors. */
		return SR_ERR;
	}
	sr_dbg("la8: device opened successfully");

	/* Purge RX/TX buffers in the FTDI chip. */
	if ((ret = ftdi_usb_purge_buffers(la8->ftdic)) < 0) {
		sr_err("la8: %s: ftdi_usb_purge_buffers: (%d) %s",
		       __func__, ret, ftdi_get_error_string(la8->ftdic));
		(void) la8_close_usb_reset_sequencer(la8); /* Ignore errors. */
		goto err_opendev_close_ftdic;
	}
	sr_dbg("la8: FTDI buffers purged successfully");

	/* Enable flow control in the FTDI chip. */
	if ((ret = ftdi_setflowctrl(la8->ftdic, SIO_RTS_CTS_HS)) < 0) {
		sr_err("la8: %s: ftdi_setflowcontrol: (%d) %s",
		       __func__, ret, ftdi_get_error_string(la8->ftdic));
		(void) la8_close_usb_reset_sequencer(la8); /* Ignore errors. */
		goto err_opendev_close_ftdic;
	}
	sr_dbg("la8: FTDI flow control enabled successfully");

	/* Wait 100ms. */
	g_usleep(100 * 1000);

	sdi->status = SR_ST_ACTIVE;

	return SR_OK;

err_opendev_close_ftdic:
	(void) la8_close(la8); /* Log, but ignore errors. */
	return SR_ERR;
}

static int set_samplerate(struct sr_device_instance *sdi, uint64_t samplerate)
{
	struct la8 *la8;

	if (!sdi) {
		sr_err("la8: %s: sdi was NULL", __func__);
		return SR_ERR_ARG;
	}

	if (!(la8 = sdi->priv)) {
		sr_err("la8: %s: sdi->priv was NULL", __func__);
		return SR_ERR_ARG;
	}

	sr_spew("la8: setting samplerate");

	fill_supported_samplerates_if_needed();

	/* Check if this is a samplerate supported by the hardware. */
	if (!is_valid_samplerate(samplerate))
		return SR_ERR;

	/* Set the new samplerate. */
	la8->cur_samplerate = samplerate;
	la8->period_ps = 1000000000000 / samplerate;

	sr_dbg("la8: samplerate set to %" PRIu64 "Hz", la8->cur_samplerate);

	return SR_OK;
}

static int hw_closedev(int device_index)
{
	struct sr_device_instance *sdi;
	struct la8 *la8;

	if (!(sdi = sr_get_device_instance(device_instances, device_index))) {
		sr_err("la8: %s: sdi was NULL", __func__);
		return SR_ERR; /* TODO: SR_ERR_ARG? */
	}

	if (!(la8 = sdi->priv)) {
		sr_err("la8: %s: sdi->priv was NULL", __func__);
		return SR_ERR; /* TODO: SR_ERR_ARG? */
	}

	sr_dbg("la8: closing device");

	if (sdi->status == SR_ST_ACTIVE) {
		sr_dbg("la8: %s: status ACTIVE, closing device", __func__);
		/* TODO: Really ignore errors here, or return SR_ERR? */
		(void) la8_close_usb_reset_sequencer(la8); /* Ignore errors. */
	} else {
		sr_spew("la8: %s: status not ACTIVE, nothing to do", __func__);
	}

	sdi->status = SR_ST_INACTIVE;

	sr_dbg("la8: %s: freeing sample buffers", __func__);
	g_free(la8->final_buf);

	return SR_OK;
}

static void hw_cleanup(void)
{
	GSList *l;
	struct sr_device_instance *sdi;

	sr_spew("la8: entering %s", __func__);

	/* Properly close all devices. */
	for (l = device_instances; l; l = l->next) {
		if ((sdi = l->data) == NULL) {
			sr_warn("la8: %s: sdi was NULL, continuing", __func__);
			continue;
		}
		if (sdi->priv != NULL)
			free(sdi->priv);
		else
			sr_warn("la8: %s: sdi->priv was NULL, nothing "
				"to do", __func__);
		sr_device_instance_free(sdi); /* Returns void. */
	}
	g_slist_free(device_instances); /* Returns void. */
	device_instances = NULL;
}

static void *hw_get_device_info(int device_index, int device_info_id)
{
	struct sr_device_instance *sdi;
	struct la8 *la8;
	void *info;

	sr_spew("la8: entering %s", __func__);

	if (!(sdi = sr_get_device_instance(device_instances, device_index))) {
		sr_err("la8: %s: sdi was NULL", __func__);
		return NULL;
	}

	if (!(la8 = sdi->priv)) {
		sr_err("la8: %s: sdi->priv was NULL", __func__);
		return NULL;
	}

	switch (device_info_id) {
	case SR_DI_INSTANCE:
		info = sdi;
		break;
	case SR_DI_NUM_PROBES:
		info = GINT_TO_POINTER(NUM_PROBES);
		break;
	case SR_DI_SAMPLERATES:
		fill_supported_samplerates_if_needed();
		info = &samplerates;
		break;
	case SR_DI_TRIGGER_TYPES:
		info = (char *)TRIGGER_TYPES;
		break;
	case SR_DI_CUR_SAMPLERATE:
		info = &la8->cur_samplerate;
		break;
	default:
		/* Unknown device info ID, return NULL. */
		sr_err("la8: %s: Unknown device info ID", __func__);
		info = NULL;
		break;
	}

	return info;
}

static int hw_get_status(int device_index)
{
	struct sr_device_instance *sdi;

	if (!(sdi = sr_get_device_instance(device_instances, device_index))) {
		sr_warn("la8: %s: sdi was NULL, device not found", __func__);
		return SR_ST_NOT_FOUND;
	}

	sr_dbg("la8: %s: returning status %d", __func__, sdi->status);

	return sdi->status;
}

static int *hw_get_capabilities(void)
{
	sr_spew("la8: entering %s", __func__);

	return capabilities;
}

static int hw_set_configuration(int device_index, int capability, void *value)
{
	struct sr_device_instance *sdi;
	struct la8 *la8;

	sr_spew("la8: entering %s", __func__);

	if (!(sdi = sr_get_device_instance(device_instances, device_index))) {
		sr_err("la8: %s: sdi was NULL", __func__);
		return SR_ERR; /* TODO: SR_ERR_ARG? */
	}

	if (!(la8 = sdi->priv)) {
		sr_err("la8: %s: sdi->priv was NULL", __func__);
		return SR_ERR; /* TODO: SR_ERR_ARG? */
	}

	switch (capability) {
	case SR_HWCAP_SAMPLERATE:
		if (set_samplerate(sdi, *(uint64_t *)value) == SR_ERR)
			return SR_ERR;
		sr_dbg("la8: SAMPLERATE = %" PRIu64, la8->cur_samplerate);
		break;
	case SR_HWCAP_PROBECONFIG:
		if (configure_probes(la8, (GSList *)value) != SR_OK) {
			sr_err("la8: %s: probe config failed", __func__);
			return SR_ERR;
		}
		break;
	case SR_HWCAP_LIMIT_MSEC:
		if (*(uint64_t *)value == 0) {
			sr_err("la8: %s: LIMIT_MSEC can't be 0", __func__);
			return SR_ERR;
		}
		la8->limit_msec = *(uint64_t *)value;
		sr_dbg("la8: LIMIT_MSEC = %" PRIu64, la8->limit_msec);
		break;
	case SR_HWCAP_LIMIT_SAMPLES:
		if (*(uint64_t *)value < MIN_NUM_SAMPLES) {
			sr_err("la8: %s: LIMIT_SAMPLES too small", __func__);
			return SR_ERR;
		}
		la8->limit_samples = *(uint64_t *)value;
		sr_dbg("la8: LIMIT_SAMPLES = %" PRIu64, la8->limit_samples);
		break;
	default:
		/* Unknown capability, return SR_ERR. */
		sr_err("la8: %s: Unknown capability", __func__);
		return SR_ERR;
		break;
	}

	return SR_OK;
}

/**
 * Get a block of data from the LA8.
 *
 * @param la8 The LA8 struct containing private per-device-instance data.
 * @return SR_OK upon success, or SR_ERR upon errors.
 */
static int la8_read_block(struct la8 *la8)
{
	int i, byte_offset, m, mi, p, index, bytes_read;
	time_t now;

	if (!la8) {
		sr_err("la8: %s: la8 was NULL", __func__);
		return SR_ERR_ARG;
	}

	if (!la8->ftdic) {
		sr_err("la8: %s: la8->ftdic was NULL", __func__);
		return SR_ERR_ARG;
	}

	sr_spew("la8: %s: reading block %d", __func__, la8->block_counter);

	bytes_read = la8_read(la8, la8->mangled_buf, BS);

	/* If first block read got 0 bytes, retry until success or timeout. */
	if ((bytes_read == 0) && (la8->block_counter == 0)) {
		do {
			sr_spew("la8: %s: reading block 0 again", __func__);
			bytes_read = la8_read(la8, la8->mangled_buf, BS);
			/* TODO: How to handle read errors here? */
			now = time(NULL);
		} while ((la8->done > now) && (bytes_read == 0));
	}

	/* Check if block read was successful or a timeout occured. */
	if (bytes_read != BS) {
		sr_warn("la8: %s: trigger timed out", __func__);
		(void) la8_reset(la8); /* Ignore errors. */
		return SR_ERR;
	}

	/* De-mangle the data. */
	sr_spew("la8: de-mangling samples of block %d", la8->block_counter);
	byte_offset = la8->block_counter * BS;
	m = byte_offset / (1024 * 1024);
	mi = m * (1024 * 1024);
	for (i = 0; i < BS; i++) {
		p = i & (1 << 0);
		index = m * 2 + (((byte_offset + i) - mi) / 2) * 16;
		index += (la8->divcount == 0) ? p : (1 - p);
		la8->final_buf[index] = la8->mangled_buf[i];
	}

	return SR_OK;
}

static void send_block_to_session_bus(struct la8 *la8, int block)
{
	int i;
	uint8_t sample, expected_sample;
	struct sr_datafeed_packet packet;
	struct sr_datafeed_logic logic;
	int trigger_point; /* Relative trigger point (in this block). */

	/* Note: No sanity checks on la8/block, caller is responsible. */

	/* Check if we can find the trigger condition in this block. */
	trigger_point = -1;
	expected_sample = la8->trigger_pattern & la8->trigger_mask;
	for (i = 0; i < BS; i++) {
		/* Don't continue if the trigger was found previously. */
		if (la8->trigger_found)
			break;

		/*
		 * Also, don't continue if triggers are "don't care", i.e. if
		 * no trigger conditions were specified by the user. In that
		 * case we don't want to send an SR_DF_TRIGGER packet at all.
		 */
		if (la8->trigger_mask == 0x00)
			break;

		sample = *(la8->final_buf + (block * BS) + i);

		if ((sample & la8->trigger_mask) == expected_sample) {
			trigger_point = i;
			la8->trigger_found = 1;
			break;
		}
	}

	/* If no trigger was found, send one SR_DF_LOGIC packet. */
	if (trigger_point == -1) {
		/* Send an SR_DF_LOGIC packet to the session bus. */
		sr_spew("la8: sending SR_DF_LOGIC packet (%d bytes) for "
		        "block %d", BS, block);
		packet.type = SR_DF_LOGIC;
		packet.timeoffset = block * BS * la8->period_ps;
		packet.duration = BS * la8->period_ps;
		packet.payload = &logic;
		logic.length = BS;
		logic.unitsize = 1;
		logic.data = la8->final_buf + (block * BS);
		sr_session_bus(la8->session_id, &packet);
		return;
	}

	/*
	 * We found the trigger, so some special handling is needed. We have
	 * to send an SR_DF_LOGIC packet with the samples before the trigger
	 * (if any), then the SD_DF_TRIGGER packet itself, then another
	 * SR_DF_LOGIC packet with the samples after the trigger (if any).
	 */

	/* TODO: Send SR_DF_TRIGGER packet before or after the actual sample? */

	/* If at least one sample is located before the trigger... */
	if (trigger_point > 0) {
		/* Send pre-trigger SR_DF_LOGIC packet to the session bus. */
		sr_spew("la8: sending pre-trigger SR_DF_LOGIC packet, "
			"start = %d, length = %d", block * BS, trigger_point);
		packet.type = SR_DF_LOGIC;
		packet.timeoffset = block * BS * la8->period_ps;
		packet.duration = trigger_point * la8->period_ps;
		packet.payload = &logic;
		logic.length = trigger_point;
		logic.unitsize = 1;
		logic.data = la8->final_buf + (block * BS);
		sr_session_bus(la8->session_id, &packet);
	}

	/* Send the SR_DF_TRIGGER packet to the session bus. */
	sr_spew("la8: sending SR_DF_TRIGGER packet, sample = %d",
		(block * BS) + trigger_point);
	packet.type = SR_DF_TRIGGER;
	packet.timeoffset = (block * BS + trigger_point) * la8->period_ps;
	packet.duration = 0;
	packet.payload = NULL;
	sr_session_bus(la8->session_id, &packet);

	/* If at least one sample is located after the trigger... */
	if (trigger_point < (BS - 1)) {
		/* Send post-trigger SR_DF_LOGIC packet to the session bus. */
		sr_spew("la8: sending post-trigger SR_DF_LOGIC packet, "
			"start = %d, length = %d",
			(block * BS) + trigger_point, BS - trigger_point);
		packet.type = SR_DF_LOGIC;
		packet.timeoffset = (block * BS + trigger_point) * la8->period_ps;
		packet.duration = (BS - trigger_point) * la8->period_ps;
		packet.payload = &logic;
		logic.length = BS - trigger_point;
		logic.unitsize = 1;
		logic.data = la8->final_buf + (block * BS) + trigger_point;
		sr_session_bus(la8->session_id, &packet);
	}
}

static int receive_data(int fd, int revents, void *session_data)
{
	int i, ret;
	struct sr_device_instance *sdi;
	struct la8 *la8;

	/* Avoid compiler errors. */
	fd = fd;
	revents = revents;

	if (!(sdi = session_data)) {
		sr_err("la8: %s: user_data was NULL", __func__);
		return FALSE;
	}

	if (!(la8 = sdi->priv)) {
		sr_err("la8: %s: sdi->priv was NULL", __func__);
		return FALSE;
	}

	/* Get one block of data. */
	if ((ret = la8_read_block(la8)) < 0) {
		sr_err("la8: %s: la8_read_block error: %d", __func__, ret);
		hw_stop_acquisition(sdi->index, session_data);
		return FALSE;
	}

	/* We need to get exactly NUM_BLOCKS blocks (i.e. 8MB) of data. */
	if (la8->block_counter != (NUM_BLOCKS - 1)) {
		la8->block_counter++;
		return TRUE;
	}

	sr_dbg("la8: sampling finished, sending data to session bus now");

	/* All data was received and demangled, send it to the session bus. */
	for (i = 0; i < NUM_BLOCKS; i++)
		send_block_to_session_bus(la8, i);

	hw_stop_acquisition(sdi->index, session_data);

	// return FALSE; /* FIXME? */
	return TRUE;
}

static int hw_start_acquisition(int device_index, gpointer session_data)
{
	struct sr_device_instance *sdi;
	struct la8 *la8;
	struct sr_datafeed_packet packet;
	struct sr_datafeed_header header;
	uint8_t buf[4];
	int bytes_written;

	sr_spew("la8: entering %s", __func__);

	if (!(sdi = sr_get_device_instance(device_instances, device_index))) {
		sr_err("la8: %s: sdi was NULL", __func__);
		return SR_ERR; /* TODO: SR_ERR_ARG? */
	}

	if (!(la8 = sdi->priv)) {
		sr_err("la8: %s: sdi->priv was NULL", __func__);
		return SR_ERR; /* TODO: SR_ERR_ARG? */
	}

	if (!la8->ftdic) {
		sr_err("la8: %s: la8->ftdic was NULL", __func__);
		return SR_ERR_ARG;
	}

	la8->divcount = samplerate_to_divcount(la8->cur_samplerate);
	if (la8->divcount == 0xff) {
		sr_err("la8: %s: invalid divcount/samplerate", __func__);
		return SR_ERR;
	}

	/* Fill acquisition parameters into buf[]. */
	buf[0] = la8->divcount;
	buf[1] = 0xff; /* This byte must always be 0xff. */
	buf[2] = la8->trigger_pattern;
	buf[3] = la8->trigger_mask;

	/* Start acquisition. */
	bytes_written = la8_write(la8, buf, 4);

	if (bytes_written < 0) {
		sr_err("la8: acquisition failed to start");
		return SR_ERR;
	} else if (bytes_written != 4) {
		sr_err("la8: acquisition failed to start");
		return SR_ERR; /* TODO: Other error and return code? */
	}

	sr_dbg("la8: acquisition started successfully");

	la8->session_id = session_data;

	/* Send header packet to the session bus. */
	sr_dbg("la8: %s: sending SR_DF_HEADER", __func__);
	packet.type = SR_DF_HEADER;
	packet.payload = &header;
	header.feed_version = 1;
	gettimeofday(&header.starttime, NULL);
	header.samplerate = la8->cur_samplerate;
	header.num_logic_probes = NUM_PROBES;
	header.num_analog_probes = 0;
	sr_session_bus(session_data, &packet);

	/* Time when we should be done (for detecting trigger timeouts). */
	la8->done = (la8->divcount + 1) * 0.08388608 + time(NULL)
			+ la8->trigger_timeout;
	la8->block_counter = 0;
	la8->trigger_found = 0;

	/* Hook up a dummy handler to receive data from the LA8. */
	sr_source_add(-1, G_IO_IN, 0, receive_data, sdi);

	return SR_OK;
}

static void hw_stop_acquisition(int device_index, gpointer session_data)
{
	struct sr_device_instance *sdi;
	struct la8 *la8;
	struct sr_datafeed_packet packet;

	sr_dbg("la8: stopping acquisition");

	if (!(sdi = sr_get_device_instance(device_instances, device_index))) {
		sr_err("la8: %s: sdi was NULL", __func__);
		return;
	}

	if (!(la8 = sdi->priv)) {
		sr_err("la8: %s: sdi->priv was NULL", __func__);
		return;
	}

	/* Send end packet to the session bus. */
	sr_dbg("la8: %s: sending SR_DF_END", __func__);
	packet.type = SR_DF_END;
	sr_session_bus(session_data, &packet);
}

struct sr_device_plugin chronovu_la8_plugin_info = {
	.name = "chronovu-la8",
	.longname = "ChronoVu LA8",
	.api_version = 1,
	.init = hw_init,
	.cleanup = hw_cleanup,
	.opendev = hw_opendev,
	.closedev = hw_closedev,
	.get_device_info = hw_get_device_info,
	.get_status = hw_get_status,
	.get_capabilities = hw_get_capabilities,
	.set_configuration = hw_set_configuration,
	.start_acquisition = hw_start_acquisition,
	.stop_acquisition = hw_stop_acquisition,
};
