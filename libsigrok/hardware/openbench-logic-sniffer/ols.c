/*
 * This file is part of the sigrok project.
 *
 * Copyright (C) 2010 Bert Vermeulen <bert@biot.com>
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

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <string.h>
#include <sys/time.h>
#include <inttypes.h>
#include <glib.h>
#include "sigrok.h"

#define NUM_PROBES				32
#define NUM_TRIGGER_STAGES		4
#define TRIGGER_TYPES			"01"
#define SERIAL_SPEED			B115200
/* TODO: SERIAL_ bits, parity, stop bit */
#define CLOCK_RATE				100000000


/* command opcodes */
#define CMD_RESET					0x00
#define CMD_ID						0x02
#define CMD_SET_FLAGS				0x82
#define CMD_SET_DIVIDER			0x80
#define CMD_RUN					0x01
#define CMD_CAPTURE_SIZE			0x81
#define CMD_SET_TRIGGER_MASK_0		0xc0
#define CMD_SET_TRIGGER_MASK_1		0xc4
#define CMD_SET_TRIGGER_MASK_2		0xc8
#define CMD_SET_TRIGGER_MASK_3		0xcc
#define CMD_SET_TRIGGER_VALUE_0	0xc1
#define CMD_SET_TRIGGER_VALUE_1	0xc5
#define CMD_SET_TRIGGER_VALUE_2	0xc9
#define CMD_SET_TRIGGER_VALUE_3	0xcd
#define CMD_SET_TRIGGER_CONFIG_0	0xc2
#define CMD_SET_TRIGGER_CONFIG_1	0xc6
#define CMD_SET_TRIGGER_CONFIG_2	0xca
#define CMD_SET_TRIGGER_CONFIG_3	0xce

/* bitmasks for CMD_FLAGS */
#define FLAG_DEMUX				0x01
#define FLAG_FILTER			0x02
#define FLAG_CHANNELGROUP_1	0x04
#define FLAG_CHANNELGROUP_2	0x08
#define FLAG_CHANNELGROUP_3	0x10
#define FLAG_CHANNELGROUP_4	0x20
#define FLAG_CLOCK_EXTERNAL	0x40
#define FLAG_CLOCK_INVERTED	0x80
#define FLAG_RLE				0x0100


static int capabilities[] = {
	HWCAP_LOGIC_ANALYZER,
	HWCAP_SAMPLERATE,
	HWCAP_CAPTURE_RATIO,
	HWCAP_LIMIT_SAMPLES,
	0
};

static struct samplerates samplerates = {
	1,
	MHZ(200),
	1,
	0
};

/* list of struct serial_device_instance  */
static GSList *device_instances = NULL;

/* current state of the flag register */
uint32_t flag_reg = 0;

static uint64_t cur_samplerate = 0;
static uint64_t limit_samples = 0;
/* pre/post trigger capture ratio, in percentage. 0 means no pre-trigger data. */
int capture_ratio = 0;
static uint32_t probe_mask = 0, trigger_mask[4] = {0}, trigger_value[4] = {0};



int send_shortcommand(int fd, uint8_t command)
{
	char buf[1];

	g_message("ols: sending cmd 0x%.2x", command);
	buf[0] = command;
	if(write(fd, buf, 1) != 1)
		return SIGROK_NOK;

	return SIGROK_OK;
}


int send_longcommand(int fd, uint8_t command, uint32_t data)
{
	char buf[5];

	g_message("ols: sending cmd 0x%.2x data 0x%.8x", command, data);
	buf[0] = command;
	buf[1] = data & 0xff;
	buf[2] = data & 0xff00 >> 8;
	buf[3] = data & 0xff0000 >> 16;
	buf[4] = data & 0xff000000 >> 24;
	if(write(fd, buf, 5) != 5)
		return SIGROK_NOK;

	return SIGROK_OK;
}

static int configure_probes(GSList *probes)
{
	struct probe *probe;
	GSList *l;
	int probe_bit, changrp_mask, stage, i;
	char *tc;

	probe_mask = 0;
	for(i = 0; i < NUM_TRIGGER_STAGES; i++)
	{
		trigger_mask[i] = 0;
		trigger_value[i] = 0;
	}

	for(l = probes; l; l = l->next)
	{
		probe = (struct probe *) l->data;
		probe_bit = 1 << (probe->index - 1);
		probe_mask |= probe_bit;
		if(probe->trigger)
		{
			stage = 0;
			for(tc = probe->trigger; *tc; tc++)
			{
				trigger_mask[stage] |= probe_bit;
				if(*tc == '1')
					trigger_value[stage] |= probe_bit;
				stage++;
				if(stage > 3)
				{
					/* only supporting parallel mode, with up to 4 stages */
					return SIGROK_NOK;
				}
			}
		}
	}

	/* enable/disable channel groups in the flag register according
	 * to the probe mask we just made. The register stores them backwards,
	 * hence shift right from 1000.
	 */
	changrp_mask = 0;
	for(i = 0; i < 4; i++)
	{
		if(probe_mask & (0xff << i))
			changrp_mask |= 8 >> i;
	}
	/* but the flag register wants them here */
	flag_reg |= changrp_mask << 2;

	return SIGROK_OK;
}


static int hw_init(char *deviceinfo)
{
	struct sigrok_device_instance *sdi;
	GSList *ports, *l;
	GPollFD *fds;
	struct termios term, *prev_termios;
	int devcnt, final_devcnt, num_ports, fd, i;
	char buf[8], **device_names;

	if(deviceinfo)
		ports = g_slist_append(NULL, g_strdup(deviceinfo));
	else
		/* no specific device given, so scan all serial ports */
		ports = list_serial_ports();

	num_ports = g_slist_length(ports);
	fds = g_malloc0(num_ports * sizeof(GPollFD));
	device_names = g_malloc(num_ports * (sizeof(char *)));
	prev_termios = g_malloc(num_ports * sizeof(struct termios));
	devcnt = 0;
	for(l = ports; l; l = l->next) {
		/* The discovery procedure is like this: first send the Reset command (0x00) 5 times,
		 * since the device could be anywhere in a 5-byte command. Then send the ID command
		 * (0x02). If the device responds with 4 bytes ("OLS1" or "SLA1"), we have a match.
		 * Since it may take the device a while to respond at 115Kb/s, we do all the sending
		 * first, then wait for all of them to respond with g_poll().
		 */
		fd = open(l->data, O_RDWR | O_NONBLOCK);
		if(fd != -1) {
			tcgetattr(fd, &prev_termios[devcnt]);
			tcgetattr(fd, &term);
			cfsetispeed(&term, SERIAL_SPEED);
			tcsetattr(fd, TCSADRAIN, &term);
			memset(buf, CMD_RESET, 5);
			buf[5] = CMD_ID;
			if(write(fd, buf, 6) == 6) {
				fds[devcnt].fd = fd;
				fds[devcnt].events = G_IO_IN;
				device_names[devcnt] = l->data;
				devcnt++;
				g_message("probed device %s", (char *) l->data);
			}
			else {
				/* restore port settings. of course, we've crapped all over the port. */
				tcsetattr(fd, TCSADRAIN, &prev_termios[devcnt]);
				g_free(l->data);
			}
		}
	}

	/* 2ms should do it, that's enough time for 28 bytes to go over the bus */
	usleep(2000);

	final_devcnt = 0;
	g_poll(fds, devcnt, 1);
	for(i = 0; i < devcnt; i++) {
		if(fds[i].revents == G_IO_IN) {
			if(read(fds[i].fd, buf, 4) == 4) {
				if(!strncmp(buf, "1SLO", 4) || !strncmp(buf, "1ALS", 4)) {
					if(!strncmp(buf, "1SLO", 4))
						sdi = sigrok_device_instance_new(final_devcnt, ST_INACTIVE,
								"Openbench", "Logic Sniffer", "v1.0");
					else
						sdi = sigrok_device_instance_new(final_devcnt, ST_INACTIVE,
								"Sump", "Logic Analyzer", "v1.0");
					sdi->serial = serial_device_instance_new(device_names[i], -1);
					device_instances = g_slist_append(device_instances, sdi);
					final_devcnt++;
					fds[i].fd = 0;
				}
			}
		}

		if(fds[i].fd != 0)
			tcsetattr(fds[i].fd, TCSADRAIN, &prev_termios[i]);
		close(fds[i].fd);
	}

	g_free(fds);
	g_free(device_names);
	g_free(prev_termios);
	g_slist_free(ports);

	return final_devcnt;
}


static int hw_opendev(int device_index)
{
	struct sigrok_device_instance *sdi;

	if(!(sdi = get_sigrok_device_instance(device_instances, device_index)))
		return SIGROK_NOK;

	sdi->serial->fd = open(sdi->serial->port, O_RDWR);
	if(sdi->serial->fd == -1)
		return SIGROK_NOK;

	sdi->status = ST_ACTIVE;

	return SIGROK_OK;
}


static void hw_closedev(int device_index)
{
	struct sigrok_device_instance *sdi;

	if(!(sdi = get_sigrok_device_instance(device_instances, device_index)))
		return;

	if(sdi->serial->fd != -1) {
		close(sdi->serial->fd);
		sdi->serial->fd = -1;
		sdi->status = ST_INACTIVE;
	}

}


static void hw_cleanup(void)
{
	GSList *l;
	struct sigrok_device_instance *sdi;

	/* properly close all devices */
	for(l = device_instances; l; l = l->next) {
		sdi = l->data;
		if(sdi->serial->fd != -1)
			close(sdi->serial->fd);
		sigrok_device_instance_free(sdi);
	}
	g_slist_free(device_instances);
	device_instances = NULL;

}


static void *hw_get_device_info(int device_index, int device_info_id)
{
	struct sigrok_device_instance *sdi;
	void *info;

	if( !(sdi = get_sigrok_device_instance(device_instances, device_index)) )
		return NULL;

	info = NULL;
	switch(device_info_id)
	{
	case DI_INSTANCE:
		info = sdi;
		break;
	case DI_NUM_PROBES:
		info = GINT_TO_POINTER(NUM_PROBES);
		break;
	case DI_SAMPLERATES:
		info = &samplerates;
		break;
	case DI_TRIGGER_TYPES:
		info = (char *) TRIGGER_TYPES;
		break;
	case DI_CUR_SAMPLERATE:
		info = &cur_samplerate;
		break;
	}

	return info;
}


static int hw_get_status(int device_index)
{
	struct sigrok_device_instance *sdi;

	if(!(sdi = get_sigrok_device_instance(device_instances, device_index)))
		return ST_NOT_FOUND;

	return sdi->status;
}


static int *hw_get_capabilities(void)
{

	return capabilities;
}


static int set_configuration_samplerate(struct sigrok_device_instance *sdi, uint64_t samplerate)
{
	uint32_t divider;

	if(samplerate < samplerates.low || samplerate > samplerates.high)
		return SIGROK_ERR_BADVALUE;

	if(samplerate  > CLOCK_RATE) {
		flag_reg |= FLAG_DEMUX;
		divider = (uint8_t) (CLOCK_RATE * 2 / samplerate) - 1;
	}
	else {
		flag_reg &= FLAG_DEMUX;
		divider = (uint8_t) (CLOCK_RATE / samplerate) - 1;
	}
	divider <<= 8;

	g_message("setting samplerate to %"PRIu64" Hz (divider %u, demux %s)", samplerate, divider,
			flag_reg & FLAG_DEMUX ? "on" : "off");
	if(send_longcommand(sdi->serial->fd, CMD_SET_DIVIDER, divider) != SIGROK_OK)
		return SIGROK_NOK;
	cur_samplerate = samplerate;

	return SIGROK_OK;
}


static int hw_set_configuration(int device_index, int capability, void *value)
{
	struct sigrok_device_instance *sdi;
	int ret;
	uint64_t *tmp_u64;

	if(!(sdi = get_sigrok_device_instance(device_instances, device_index)))
		return SIGROK_NOK;

	if(sdi->status != ST_ACTIVE)
		return SIGROK_NOK;

	if(capability == HWCAP_SAMPLERATE) {
		tmp_u64 = value;
		ret = set_configuration_samplerate(sdi, *tmp_u64);
	}
	else if(capability == HWCAP_PROBECONFIG)
		ret = configure_probes( (GSList *) value);
	else if(capability == HWCAP_LIMIT_SAMPLES) {
		limit_samples = strtoull(value, NULL, 10);
		ret = SIGROK_OK;
	}
	else if(capability == HWCAP_CAPTURE_RATIO) {
		capture_ratio = strtol(value, NULL, 10);
		if(capture_ratio < 0 || capture_ratio > 100) {
			capture_ratio = 0;
			ret = SIGROK_NOK;
		}
		else
			ret = SIGROK_OK;
	}
	else
		ret = SIGROK_NOK;

	return ret;
}


static int receive_data(int fd, int revents, void *user_data)
{
	static int num_transfers = 0;
	static int num_bytes = 0;
	static char last_sample[4] = {0xff};
	static unsigned char sample[4];
	int count, buflen, i;
	struct datafeed_packet packet;
	unsigned char byte, *buffer;

	if(num_transfers++ == 0) {
		/* first time round, means the device started sending data, and will not
		 * stop until done. if it stops sending for longer than it takes to send
		 * a byte, that means it's finished. we'll double that to 30ms to be sure...
		 */
		source_remove(fd);
		source_add(fd, G_IO_IN, 30, receive_data, user_data);
	}

	/* TODO: / 4 depends on # of channels used */
	if(revents == G_IO_IN && num_transfers / 4 <= limit_samples){
		if(read(fd, &byte, 1) != 1)
			return FALSE;

		sample[num_bytes++] = byte;
		if(num_bytes == 4) {
			g_message("got sample 0x%.2x%.2x%.2x%.2x", sample[3], sample[2], sample[1], sample[0]);
			/* got a full sample */
			if(flag_reg & FLAG_RLE) {
				/* in RLE mode -1 should never come in as a sample, because
				 * bit 31 is the "count" flag */
				/* TODO: endianness may be wrong here, could be sample[3] */
				if(sample[0] & 0x80 && !(last_sample[0] & 0x80)) {
					count = (int) (*sample) & 0x7fffffff;
					buffer = g_malloc(count);
					buflen = 0;
					for(i = 0; i < count; i++)
					{
						memcpy(buffer + buflen , last_sample, 4);
						buflen += 4;
					}
				}
				else {
					/* just a single sample, next sample will probably be a count
					 * referring to this -- but this one is still a part of the stream
					 */
					buffer = sample;
					buflen = 4;
				}
			}
			else {
				/* no compression */
				buffer = sample;
				buflen = 4;
			}

			/* send it all to the session bus */
			packet.type = DF_LOGIC32;
			packet.length = buflen;
			packet.payload = buffer;
			session_bus(user_data, &packet);
			if(buffer == sample)
				memcpy(last_sample, buffer, 4);
			else
				g_free(buffer);

			num_bytes = 0;
		}
	}
	else {
		/* this is the main loop telling us a timeout was reached -- we're done */
		tcflush(fd, TCIOFLUSH);
		close(fd);
		packet.type = DF_END;
		packet.length = 0;
		session_bus(user_data, &packet);
	}

	return TRUE;
}


static int hw_start_acquisition(int device_index, gpointer session_device_id)
{
	struct datafeed_packet *packet;
	struct datafeed_header *header;
	struct sigrok_device_instance *sdi;
	uint32_t data;

	if(!(sdi = get_sigrok_device_instance(device_instances, device_index)))
		return SIGROK_NOK;

	if(sdi->status != ST_ACTIVE)
		return SIGROK_NOK;

	/* reset again */
	if(send_longcommand(sdi->serial->fd, CMD_RESET, 0) != SIGROK_OK)
		return SIGROK_NOK;

	/* send flag register */
	data = flag_reg << 24;
	if(send_longcommand(sdi->serial->fd, CMD_SET_FLAGS, data) != SIGROK_OK)
		return SIGROK_NOK;

	/* send sample limit and pre/post-trigger capture ratio */
	data = limit_samples / 4 << 16;
	if(capture_ratio)
		data |= (limit_samples - (limit_samples / 100 * capture_ratio)) / 4;
	data = 0x00190019;
	if(send_longcommand(sdi->serial->fd, CMD_CAPTURE_SIZE, data) != SIGROK_OK)
		return SIGROK_NOK;

	/* trigger masks */
	if(send_longcommand(sdi->serial->fd, CMD_SET_TRIGGER_MASK_0, trigger_mask[0]) != SIGROK_OK)
		return SIGROK_NOK;
//	if(send_longcommand(sdi->serial->fd, CMD_SET_TRIGGER_MASK_1, trigger_mask[1]) != SIGROK_OK)
//		return SIGROK_NOK;
//	if(send_longcommand(sdi->serial->fd, CMD_SET_TRIGGER_MASK_2, trigger_mask[2]) != SIGROK_OK)
//		return SIGROK_NOK;
//	if(send_longcommand(sdi->serial->fd, CMD_SET_TRIGGER_MASK_3, trigger_mask[3]) != SIGROK_OK)
//		return SIGROK_NOK;

	if(send_longcommand(sdi->serial->fd, CMD_SET_TRIGGER_VALUE_0, trigger_value[0]) != SIGROK_OK)
		return SIGROK_NOK;
//	if(send_longcommand(sdi->serial->fd, CMD_SET_TRIGGER_VALUE_1, trigger_value[1]) != SIGROK_OK)
//		return SIGROK_NOK;
//	if(send_longcommand(sdi->serial->fd, CMD_SET_TRIGGER_VALUE_2, trigger_value[2]) != SIGROK_OK)
//		return SIGROK_NOK;
//	if(send_longcommand(sdi->serial->fd, CMD_SET_TRIGGER_VALUE_3, trigger_value[3]) != SIGROK_OK)
//		return SIGROK_NOK;

	/* trigger configuration */
	/* TODO: the start flag should only be on the last used stage I think... */
	if(send_longcommand(sdi->serial->fd, CMD_SET_TRIGGER_CONFIG_0, 0x00000008) != SIGROK_OK)
		return SIGROK_NOK;
//	if(send_longcommand(sdi->serial->fd, CMD_SET_TRIGGER_CONFIG_1, 0x00000000) != SIGROK_OK)
//		return SIGROK_NOK;
//	if(send_longcommand(sdi->serial->fd, CMD_SET_TRIGGER_CONFIG_2, 0x00000000) != SIGROK_OK)
//		return SIGROK_NOK;
//	if(send_longcommand(sdi->serial->fd, CMD_SET_TRIGGER_CONFIG_3, 0x00000000) != SIGROK_OK)
//		return SIGROK_NOK;

	set_configuration_samplerate(sdi, cur_samplerate);

	/* start acquisition on the device */
	if(send_shortcommand(sdi->serial->fd, CMD_RUN) != SIGROK_OK)
		return SIGROK_NOK;

	source_add(sdi->serial->fd, G_IO_IN, -1, receive_data, session_device_id);

	/* send header packet to the session bus */
	packet = g_malloc(sizeof(struct datafeed_packet));
	header = g_malloc(sizeof(struct datafeed_header));
	if(!packet || !header)
		return SIGROK_NOK;
	packet->type = DF_HEADER;
	packet->length = sizeof(struct datafeed_header);
	packet->payload = (unsigned char *) header;
	header->feed_version = 1;
	gettimeofday(&header->starttime, NULL);
	header->samplerate = cur_samplerate;
	header->protocol_id = PROTO_RAW;
	header->num_probes = NUM_PROBES;
	session_bus(session_device_id, packet);
	g_free(header);
	g_free(packet);

	return SIGROK_OK;
}


static void hw_stop_acquisition(int device_index, gpointer session_device_id)
{
	struct datafeed_packet packet;

	packet.type = DF_END;
	packet.length = 0;
	session_bus(session_device_id, &packet);

}



struct device_plugin ols_plugin_info = {
	"sump",
	1,
	hw_init,
	hw_cleanup,

	hw_opendev,
	hw_closedev,
	hw_get_device_info,
	hw_get_status,
	hw_get_capabilities,
	hw_set_configuration,
	hw_start_acquisition,
	hw_stop_acquisition
};
