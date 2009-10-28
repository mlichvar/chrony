/*
  $Header: /cvs/src/chrony/broadcast.c,v 1.3 2002/02/28 23:27:08 richard Exp $

  =======================================================================

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

  Deal with broadcast server functions.
  */

#include "sysincl.h"
#include "memory.h"

#include "addressing.h"
#include "broadcast.h"
#include "sched.h"
#include "ntp.h"
#include "local.h"
#include "reference.h"
#include "util.h"
#include "ntp_io.h"

typedef struct {
  NTP_Remote_Address addr;
  int interval;
} Destination;
static Destination *destinations = 0;
static int n_destinations = 0;
static int max_destinations = 0;

void
BRD_Initialise(void)
{
  return; /* Nothing to do */
}

/* ================================================== */

void
BRD_Finalise(void)
{
  return; /* Nothing to do */
}

/* ================================================== */
/* This is a cut-down version of what transmit_packet in ntp_core.c does */

static void
timeout_handler(void *arbitrary)
{
  Destination *d = (Destination *) arbitrary;
  NTP_Packet message;
  /* Parameters read from reference module */
  int version;
  int leap;
  int are_we_synchronised, our_stratum;
  NTP_Leap leap_status;
  unsigned long our_ref_id;
  struct timeval our_ref_time;
  double our_root_delay, our_root_dispersion;
  double local_time_err;
  struct timeval local_transmit;

  version = 3;

  LCL_ReadCookedTime(&local_transmit, &local_time_err);
  REF_GetReferenceParams(&local_transmit,
                         &are_we_synchronised, &leap_status,
                         &our_stratum,
                         &our_ref_id, &our_ref_time,
                         &our_root_delay, &our_root_dispersion);


  if (are_we_synchronised) {
    leap = (int) leap_status;
  } else {
    leap = 3;
  }

  message.lvm = ((leap << 6) &0xc0) | ((version << 3) & 0x38) | (MODE_BROADCAST & 0x07);
  message.stratum = our_stratum;
  message.poll = 6; /* FIXME: what should this be? */
  message.precision = LCL_GetSysPrecisionAsLog();

  /* If we're sending a client mode packet and we aren't synchronized yet, 
     we might have to set up artificial values for some of these parameters */
  message.root_delay = double_to_int32(our_root_delay);
  message.root_dispersion = double_to_int32(our_root_dispersion);

  message.reference_id = htonl((NTP_int32) our_ref_id);

  /* Now fill in timestamps */
  UTI_TimevalToInt64(&our_ref_time, &message.reference_ts);
  message.originate_ts.hi = 0UL;
  message.originate_ts.lo = 0UL;
  message.receive_ts.hi = 0UL;
  message.receive_ts.lo = 0UL;

  LCL_ReadCookedTime(&local_transmit, &local_time_err);
  UTI_TimevalToInt64(&local_transmit, &message.transmit_ts);
  NIO_SendNormalPacket(&message, &d->addr);

  /* Requeue timeout.  Don't care if interval drifts gradually, so just do it
   * at the end. */
  SCH_AddTimeoutInClass((double) d->interval, 1.0,
                        SCH_NtpBroadcastClass,
                        timeout_handler, (void *) d);


}

/* ================================================== */

void 
BRD_AddDestination(IPAddr *addr, unsigned short port, int interval)
{
  if (max_destinations == n_destinations) {
    /* Expand array */
    max_destinations += 8;
    if (destinations) {
      destinations = ReallocArray(Destination, max_destinations, destinations);
    } else {
      destinations = MallocArray(Destination, max_destinations);
    }
  }

  destinations[n_destinations].addr.ip_addr = *addr;
  destinations[n_destinations].addr.local_ip_addr.family = IPADDR_UNSPEC;
  destinations[n_destinations].addr.port = port;
  destinations[n_destinations].interval = interval;

  SCH_AddTimeoutInClass((double) interval, 1.0,
                        SCH_NtpBroadcastClass,
                        timeout_handler, (void *)(destinations + n_destinations));
  
  ++n_destinations;

}


