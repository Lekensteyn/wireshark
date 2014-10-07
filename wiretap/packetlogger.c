/* packetlogger.c
 * Routines for opening Apple's (Bluetooth) PacketLogger file format captures
 * Copyright 2008-2009, Stephen Fisher (see AUTHORS file)
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * Based on commview.c, Linux's BlueZ-Gnome Analyzer program and hexdumps of
 * the output files from Apple's PacketLogger tool.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */

#include "config.h"

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include "wtap.h"
#include "wtap-int.h"
#include <wsutil/buffer.h>
#include "file_wrappers.h"
#include "packetlogger.h"

typedef struct packetlogger_header {
	guint32 len;
	guint64 ts;
} packetlogger_header_t;

static gboolean packetlogger_read(wtap *wth, int *err, gchar **err_info,
				  gint64 *data_offset);
static gboolean packetlogger_seek_read(wtap *wth, gint64 seek_off,
				       struct wtap_pkthdr *phdr,
				       Buffer *buf, int *err, gchar **err_info);
static gboolean packetlogger_read_header(packetlogger_header_t *pl_hdr,
					 FILE_T fh, int *err, gchar **err_info);
static gboolean packetlogger_read_packet(FILE_T fh, struct wtap_pkthdr *phdr,
					 Buffer *buf, int *err,
					 gchar **err_info);

int packetlogger_open(wtap *wth, int *err, gchar **err_info)
{
	packetlogger_header_t pl_hdr;
	guint8 type;

	if(!packetlogger_read_header(&pl_hdr, wth->fh, err, err_info)) {
		if (*err != 0 && *err != WTAP_ERR_SHORT_READ)
			return -1;
		return 0;
	}

	if (!wtap_read_bytes(wth->fh, &type, 1, err, err_info)) {
		if (*err != WTAP_ERR_SHORT_READ)
			return -1;
		return 0;
	}

	/* Verify this file belongs to us */
	if (!((8 <= pl_hdr.len) && (pl_hdr.len < 65536) &&
	      (type < 0x04 || type == 0xFB || type == 0xFC || type == 0xFE || type == 0xFF)))
		return 0;

	/* No file header. Reset the fh to 0 so we can read the first packet */
	if (file_seek(wth->fh, 0, SEEK_SET, err) == -1)
		return -1;

	/* Set up the pointers to the handlers for this file type */
	wth->subtype_read = packetlogger_read;
	wth->subtype_seek_read = packetlogger_seek_read;

	wth->file_type_subtype = WTAP_FILE_TYPE_SUBTYPE_PACKETLOGGER;
	wth->file_encap = WTAP_ENCAP_PACKETLOGGER;
	wth->file_tsprec = WTAP_TSPREC_USEC;

	return 1; /* Our kind of file */
}

static gboolean
packetlogger_read(wtap *wth, int *err, gchar **err_info, gint64 *data_offset)
{
	*data_offset = file_tell(wth->fh);

	return packetlogger_read_packet(wth->fh, &wth->phdr,
	    wth->frame_buffer, err, err_info);
}

static gboolean
packetlogger_seek_read(wtap *wth, gint64 seek_off, struct wtap_pkthdr *phdr,
		       Buffer *buf, int *err, gchar **err_info)
{
	if(file_seek(wth->random_fh, seek_off, SEEK_SET, err) == -1)
		return FALSE;

	if(!packetlogger_read_packet(wth->random_fh, phdr, buf, err, err_info)) {
		if(*err == 0)
			*err = WTAP_ERR_SHORT_READ;

		return FALSE;
	}
	return TRUE;
}

static gboolean
packetlogger_read_header(packetlogger_header_t *pl_hdr, FILE_T fh, int *err,
			 gchar **err_info)
{
	if (!wtap_read_bytes_or_eof(fh, &pl_hdr->len, 4, err, err_info))
		return FALSE;
	if (!wtap_read_bytes(fh, &pl_hdr->ts, 8, err, err_info))
		return FALSE;

	/* Convert multi-byte values from big endian to host endian */
	pl_hdr->len = GUINT32_FROM_BE(pl_hdr->len);
	pl_hdr->ts = GUINT64_FROM_BE(pl_hdr->ts);

	return TRUE;
}

static gboolean
packetlogger_read_packet(FILE_T fh, struct wtap_pkthdr *phdr, Buffer *buf,
			 int *err, gchar **err_info)
{
	packetlogger_header_t pl_hdr;

	if(!packetlogger_read_header(&pl_hdr, fh, err, err_info))
		return FALSE;

	if (pl_hdr.len < 8) {
		*err = WTAP_ERR_BAD_FILE;
		*err_info = g_strdup_printf("packetlogger: record length %u is too small", pl_hdr.len);
		return FALSE;
	}
	if (pl_hdr.len - 8 > WTAP_MAX_PACKET_SIZE) {
		/*
		 * Probably a corrupt capture file; don't blow up trying
		 * to allocate space for an immensely-large packet.
		 */
		*err = WTAP_ERR_BAD_FILE;
		*err_info = g_strdup_printf("packetlogger: File has %u-byte packet, bigger than maximum of %u",
		    pl_hdr.len - 8, WTAP_MAX_PACKET_SIZE);
		return FALSE;
	}

	phdr->rec_type = REC_TYPE_PACKET;
	phdr->presence_flags = WTAP_HAS_TS;

	phdr->len = pl_hdr.len - 8;
	phdr->caplen = pl_hdr.len - 8;

	phdr->ts.secs = (time_t) (pl_hdr.ts >> 32);
	phdr->ts.nsecs = (int)((pl_hdr.ts & 0xFFFFFFFF) * 1000);

	return wtap_read_packet_bytes(fh, buf, phdr->caplen, err, err_info);
}
