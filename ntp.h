/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2003
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

  Header file containing common NTP bits and pieces
  */

#ifndef GOT_NTP_H
#define GOT_NTP_H

#ifdef HAS_STDINT_H
#include <stdint.h>
#elif defined(HAS_INTTYPES_H)
#include <inttypes.h>
#endif

typedef struct {
  uint32_t hi;
  uint32_t lo;
} NTP_int64;

typedef uint32_t NTP_int32;

#define AUTH_DATA_LEN 16 

/* Type definition for leap bits */
typedef enum {
  LEAP_Normal = 0,
  LEAP_InsertSecond = 1,
  LEAP_DeleteSecond = 2,
  LEAP_Unsynchronised = 3
} NTP_Leap;

typedef enum {
  MODE_UNDEFINED = 0,
  MODE_ACTIVE = 1,
  MODE_PASSIVE = 2,
  MODE_CLIENT = 3,
  MODE_SERVER = 4,
  MODE_BROADCAST = 5
} NTP_Mode;

typedef struct {
  uint8_t lvm;
  uint8_t stratum;
  int8_t poll;
  int8_t precision;
  NTP_int32 root_delay;
  NTP_int32 root_dispersion;
  NTP_int32 reference_id;
  NTP_int64 reference_ts;
  NTP_int64 originate_ts;
  NTP_int64 receive_ts;
  NTP_int64 transmit_ts;
  NTP_int32 auth_keyid;
  uint8_t auth_data[AUTH_DATA_LEN];
} NTP_Packet;

/* We have to declare a buffer type to hold a datagram read from the
   network.  Even though we won't be using them (yet?!), this must be
   large enough to hold NTP control messages. */

/* Define the maximum number of bytes that can be read in a single
   message.  (This is cribbed from ntp.h in the xntpd source code). */

#define MAX_NTP_MESSAGE_SIZE (468+12+16+4)

typedef union {
  NTP_Packet ntp_pkt;
  uint8_t arbitrary[MAX_NTP_MESSAGE_SIZE];
} ReceiveBuffer;

#define NTP_NORMAL_PACKET_SIZE (sizeof(NTP_Packet) - (sizeof(NTP_int32) + AUTH_DATA_LEN))

/* ================================================== */

inline static double
int32_to_double(NTP_int32 x)
{
  return (double) ntohl(x) / 65536.0;
}

/* ================================================== */

inline static NTP_int32
double_to_int32(double x)
{
  return htonl((NTP_int32)(0.5 + 65536.0 * x));
}

/* ================================================== */

#endif /* GOT_NTP_H */
