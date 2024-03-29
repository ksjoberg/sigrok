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

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <zip.h>
#include <sigrok.h>
#include <sigrok-internal.h>

/* size of payloads sent across the session bus */
#define CHUNKSIZE 4096

struct session_vdevice {
	char *capturefile;
	struct zip *archive;
	struct zip_file *capfile;
	uint64_t samplerate;
	int unitsize;
	int num_probes;
};

static char *sessionfile = NULL;
static GSList *device_instances = NULL;
static int capabilities[] = {
	SR_HWCAP_CAPTUREFILE,
	SR_HWCAP_CAPTURE_UNITSIZE,
	0,
};


static struct session_vdevice *get_vdevice_by_index(int device_index)
{
	struct sr_device_instance *sdi;
	struct session_vdevice *vdevice;

	if (!(sdi = sr_get_device_instance(device_instances, device_index)))
		return NULL;

	vdevice = sdi->priv;

	return vdevice;
}

static int feed_chunk(int fd, int revents, void *session_data)
{
	struct sr_device_instance *sdi;
	struct session_vdevice *vdevice;
	struct sr_datafeed_packet packet;
	struct sr_datafeed_logic logic;
	GSList *l;
	void *buf;
	int ret, got_data;

	/* avoid compiler warning */
	fd = fd;
	revents = revents;

	sr_dbg("session_driver: feed chunk");

	got_data = FALSE;
	for (l = device_instances; l; l = l->next) {
		sdi = l->data;
		vdevice = sdi->priv;
		if (!vdevice)
			/* already done with this instance */
			continue;

		if (!(buf = g_try_malloc(CHUNKSIZE))) {
			sr_err("session: %s: buf malloc failed", __func__);
			// return SR_ERR_MALLOC;
			return FALSE;
		}

		ret = zip_fread(vdevice->capfile, buf, CHUNKSIZE);
		if (ret > 0) {
			got_data = TRUE;
			packet.type = SR_DF_LOGIC;
			packet.timeoffset = 0;
			packet.duration = 0;
			packet.payload = &logic;
			logic.length = ret;
			logic.unitsize = vdevice->unitsize;
			logic.data = buf;
			sr_session_bus(session_data, &packet);
		} else {
			/* done with this capture file */
			zip_fclose(vdevice->capfile);
			g_free(vdevice->capturefile);
			g_free(vdevice);
			sdi->priv = NULL;
		}
	}

	if (!got_data) {
		packet.type = SR_DF_END;
		sr_session_bus(session_data, &packet);
	}

	return TRUE;
}

/* driver callbacks */
static void hw_cleanup(void);

static int hw_init(const char *deviceinfo)
{
	hw_cleanup();

	sessionfile = g_strdup(deviceinfo);

	return 0;
}

static void hw_cleanup(void)
{
	GSList *l;

	for (l = device_instances; l; l = l->next)
		sr_device_instance_free(l->data);

	g_slist_free(device_instances);
	device_instances = NULL;

	sr_session_source_remove(-1);

	g_free(sessionfile);

}

static int hw_opendev(int device_index)
{
	struct sr_device_instance *sdi;

	sdi = sr_device_instance_new(device_index, SR_ST_INITIALIZING,
		NULL, NULL, NULL);
	if (!sdi)
		return SR_ERR;

	if (!(sdi->priv = g_try_malloc0(sizeof(struct session_vdevice)))) {
		sr_err("session: %s: sdi->priv malloc failed", __func__);
		return SR_ERR_MALLOC;
	}

	device_instances = g_slist_append(device_instances, sdi);

	return SR_OK;
}

static void *hw_get_device_info(int device_index, int device_info_id)
{
	struct session_vdevice *vdevice;
	void *info;

	if (device_info_id != SR_DI_CUR_SAMPLERATE)
		return NULL;

	if (!(vdevice = get_vdevice_by_index(device_index)))
		return NULL;

	info = &vdevice->samplerate;

	return info;
}

static int hw_get_status(int device_index)
{

	/* avoid compiler warning */
	device_index = device_index;

	if (devices)
		return SR_OK;
	else
		return SR_ERR;
}

static int *hw_get_capabilities(void)
{

	return capabilities;
}

static int hw_set_configuration(int device_index, int capability, void *value)
{
	struct session_vdevice *vdevice;
	uint64_t *tmp_u64;

	if (!(vdevice = get_vdevice_by_index(device_index)))
		return SR_ERR;

	switch (capability) {
	case SR_HWCAP_SAMPLERATE:
		tmp_u64 = value;
		vdevice->samplerate = *tmp_u64;
		break;
	case SR_HWCAP_CAPTUREFILE:
		vdevice->capturefile = g_strdup(value);
		break;
	case SR_HWCAP_CAPTURE_UNITSIZE:
		tmp_u64 = value;
		vdevice->unitsize = *tmp_u64;
		break;
	case SR_HWCAP_CAPTURE_NUM_PROBES:
		tmp_u64 = value;
		vdevice->num_probes = *tmp_u64;
		break;
	default:
		return SR_ERR;
	}

	return SR_OK;
}

static int hw_start_acquisition(int device_index, gpointer session_device_id)
{
	struct zip_stat zs;
	struct session_vdevice *vdevice;
	struct sr_datafeed_header *header;
	struct sr_datafeed_packet *packet;
	int err;

	/* avoid compiler warning */
	session_device_id = session_device_id;

	if (!(vdevice = get_vdevice_by_index(device_index)))
		return SR_ERR;

	sr_info("session_driver: opening archive %s file %s", sessionfile,
		vdevice->capturefile);

	if (!(vdevice->archive = zip_open(sessionfile, 0, &err))) {
		sr_warn("Failed to open session file '%s': zip error %d\n",
			sessionfile, err);
		return SR_ERR;
	}

	if (zip_stat(vdevice->archive, vdevice->capturefile, 0, &zs) == -1) {
		sr_warn("Failed to check capture file '%s' in session file '%s'.",
			vdevice->capturefile, sessionfile);
		return SR_ERR;
	}

	if (!(vdevice->capfile = zip_fopen(vdevice->archive, vdevice->capturefile, 0))) {
		sr_warn("Failed to open capture file '%s' in session file '%s'.",
			vdevice->capturefile, sessionfile);
		return SR_ERR;
	}

	/* freewheeling source */
	sr_session_source_add(-1, 0, 0, feed_chunk, session_device_id);

	if (!(packet = g_try_malloc(sizeof(struct sr_datafeed_packet)))) {
		sr_err("session: %s: packet malloc failed", __func__);
		return SR_ERR_MALLOC;
	}

	if (!(header = g_try_malloc(sizeof(struct sr_datafeed_header)))) {
		sr_err("session: %s: header malloc failed", __func__);
		return SR_ERR_MALLOC;
	}

	/* Send header packet to the session bus. */
	packet->type = SR_DF_HEADER;
	packet->payload = (unsigned char *)header;
	header->feed_version = 1;
	gettimeofday(&header->starttime, NULL);
	header->samplerate = 0;
	header->num_logic_probes = vdevice->num_probes;
	header->num_analog_probes = 0;
	sr_session_bus(session_device_id, packet);
	g_free(header);
	g_free(packet);

	return SR_OK;
}

struct sr_device_plugin session_driver = {
	"session",
	"Session-emulating driver",
	1,
	hw_init,
	hw_cleanup,
	hw_opendev,
	NULL,
	hw_get_device_info,
	hw_get_status,
	hw_get_capabilities,
	hw_set_configuration,
	hw_start_acquisition,
	NULL,
};
