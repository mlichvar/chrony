/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2002
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

  This is the header file for the NTP socket I/O bits.

  */

#ifndef GOT_NTP_IO_H
#define GOT_NTP_IO_H

#include "ntp.h"
#include "addressing.h"

/* Function to initialise the module. */
extern void NIO_Initialise(void);

/* Function to finalise the module */
extern void NIO_Finalise(void);

/* Function to transmit a packet */
extern void NIO_SendNormalPacket(NTP_Packet *packet, NTP_Remote_Address *remote_addr);

/* Function to transmit an authenticated packet */
extern void NIO_SendAuthenticatedPacket(NTP_Packet *packet, NTP_Remote_Address *remote_addr);

/* Function to send a datagram to a remote machine's UDP echo port. */
extern void NIO_SendEcho(NTP_Remote_Address *remote_addr);

#endif /* GOT_NTP_IO_H */
