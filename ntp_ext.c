/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Miroslav Lichvar  2019
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 * 
 **********************************************************************

  =======================================================================

  Functions for working with NTP extension fields
  */

#include "config.h"

#include "sysincl.h"

#include "ntp_ext.h"

struct ExtFieldHeader {
  uint16_t type;
  uint16_t length;
};

/* ================================================== */

int
NEF_ParseSingleField(unsigned char *buffer, int buffer_length, int start,
                     int *length, int *type, void **body, int *body_length)
{
  struct ExtFieldHeader *header;
  int ef_length;

  if (buffer_length < 0 || start < 0 || buffer_length <= start ||
      buffer_length - start < sizeof (*header))
    return 0;

  header = (struct ExtFieldHeader *)(buffer + start);

  assert(sizeof (*header) == 4);

  ef_length = ntohs(header->length);

  if (ef_length < (int)(sizeof (*header)) || start + ef_length > buffer_length ||
      ef_length % 4 != 0)
    return 0;

  if (length)
    *length = ef_length;
  if (type)
    *type = ntohs(header->type);
  if (body)
    *body = header + 1;
  if (body_length)
    *body_length = ef_length - sizeof (*header);

  return 1;
}

/* ================================================== */

int
NEF_ParseField(NTP_Packet *packet, int packet_length, int start,
               int *length, int *type, void **body, int *body_length)
{
  int ef_length;

  if (packet_length <= NTP_HEADER_LENGTH || packet_length > sizeof (*packet) ||
      packet_length <= start || packet_length % 4 != 0 ||
      start < NTP_HEADER_LENGTH || start % 4 != 0)
    return 0;

  /* Only NTPv4 packets have extension fields */
  if (NTP_LVM_TO_VERSION(packet->lvm) != 4)
    return 0;

  /* Check if the remaining data is a MAC.  RFC 7822 specifies the maximum
     length of a MAC in NTPv4 packets in order to enable deterministic
     parsing. */
  if (packet_length - start <= NTP_MAX_V4_MAC_LENGTH)
    return 0;

  if (!NEF_ParseSingleField((unsigned char *)packet, packet_length, start,
                            &ef_length, type, body, body_length))
    return 0;

  if (ef_length < NTP_MIN_EF_LENGTH)
    return 0;

  if (length)
    *length = ef_length;

  return 1;
}
