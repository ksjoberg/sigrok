/*
 * This file is part of the sigrok project.
 *
 * Copyright (C) 2010 Uwe Hermann <uwe@hermann-uwe.de>
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

#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <sigrok.h>
#include <sigrok-internal.h>
#include "config.h"

static int data(struct sr_output *o, const char *data_in, uint64_t length_in,
		char **data_out, uint64_t *length_out)
{
	char *outbuf;

	/* Prevent compiler warnings. */
	o = o;

	if (!data_in) {
		sr_warn("binary output: %s: data_in was NULL", __func__);
		return SR_ERR;
	}

	if (!length_out) {
		sr_warn("binary output: %s: length_out was NULL", __func__);
		return SR_ERR;
	}

	if (length_in == 0) {
		sr_warn("binary output: %s: length_in was 0", __func__);
		return SR_ERR;
	}

	if (!(outbuf = calloc(1, length_in))) {
		sr_warn("binary output: %s: outbuf calloc failed", __func__);
		return SR_ERR_MALLOC;
	}

	memcpy(outbuf, data_in, length_in);
	*data_out = outbuf;
	*length_out = length_in;

	return SR_OK;
}

struct sr_output_format output_binary = {
	.id = "binary",
	.description = "Raw binary",
	.df_type = SR_DF_LOGIC,
	.init = NULL,
	.data = data,
	.event = NULL,
};
