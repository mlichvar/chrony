/*
  $Header: /cvs/src/chrony/ntp_sources.c,v 1.17 2003/03/24 23:35:43 richard Exp $

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
 * 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 * 
 **********************************************************************

  =======================================================================

  Functions which manage the pool of NTP sources that we are currently
  a client of or peering with.

  */

#include "sysincl.h"

#include "ntp_sources.h"
#include "ntp_core.h"
#include "util.h"
#include "logging.h"
#include "local.h"

/* ================================================== */

/* Record type private to this file, used to store information about
   particular sources */
typedef struct {
  NTP_Remote_Address remote_addr;       /* The address of this source */
  int in_use;                   /* Whether this slot in the table is in use */
  NCR_Instance data;            /* Data for the protocol engine for this source */
} SourceRecord;

#define N_RECORDS 256

/* Fixed size table, because we use a hard coded hash algorithm.  It
   is rather unlikely we would have anything approaching this number
   of sources. */
static SourceRecord records[N_RECORDS];

static int n_sources;

/* The largest number of sources we want to have stored in the hash table */
#define MAX_SOURCES 64

/* ================================================== */
/* Forward prototypes */
static void
slew_sources(struct timeval *raw,
             struct timeval *cooked,
             double dfreq,
             double afreq,
             double doffset,
             int is_step_change,
             void *anything);

/* ================================================== */

/* Flag indicating whether module is initialised */
static int initialised = 0;

/* ================================================== */

void
NSR_Initialise(void)
{
  int i;
  for (i=0; i<N_RECORDS; i++) {
    records[i].in_use = 0;
  }
  n_sources = 0;
  initialised = 1;

  LCL_AddParameterChangeHandler(slew_sources, NULL);

  return;
}

/* ================================================== */

void
NSR_Finalise(void)
{
  initialised = 0;
  return; /* Nothing to do yet */
}

/* ================================================== */
/* Return slot number and whether the IP address was matched or not.
   found = 0 => Neither IP nor port matched, empty slot returned
   found = 1 => Only IP matched, port doesn't match
   found = 2 => Both IP and port matched.

   It is assumed that there can only ever be one record for a
   particular IP address.  (If a different port comes up, it probably
   means someone is running ntpdate -d or something).  Thus, if we
   match the IP address we stop the search regardless of whether the
   port number matches.

  */

static void
find_slot(NTP_Remote_Address *remote_addr, int *slot, int *found)
{
  unsigned long hash;
  unsigned long ip = remote_addr->ip_addr;
  unsigned short port = remote_addr->port;

  assert(N_RECORDS == 256);
  
  /* Compute hash value just by xor'ing the 4 bytes of the address together */
  hash = ip ^ (ip >> 16);
  hash = (hash ^ (hash >> 8)) & 0xff;

  while ((records[hash].in_use) &&
         (records[hash].remote_addr.ip_addr != ip)) {
    hash++;
    if (hash == 256) hash = 0;
  }

  if (records[hash].in_use) {
    if (records[hash].remote_addr.port == port) {
      *found = 2;
    } else {
      *found = 1;
    }
    *slot = hash;
  } else {
    *found = 0;
    *slot = hash;
  }

  return;
}

/* ================================================== */

/* Procedure to add a new server source (to which this machine will be
   a client) */
NSR_Status
NSR_AddServer(NTP_Remote_Address *remote_addr, SourceParameters *params)
{
  int slot, found;

  assert(initialised);

#if 0
  LOG(LOGS_INFO, LOGF_NtpSources, "IP=%08lx port=%d", (unsigned long)remote_addr->ip_addr, remote_addr->port);
#endif

  /* Find empty bin & check that we don't have the address already */
  find_slot(remote_addr, &slot, &found);
  if (found) {
    return NSR_AlreadyInUse;
  } else {
    if (n_sources == MAX_SOURCES) {
      return NSR_TooManySources;
    } else {
      n_sources++;
      records[slot].remote_addr = *remote_addr;
      records[slot].in_use = 1;
      records[slot].data = NCR_GetServerInstance(remote_addr, params); /* Will need params passing through */
      return NSR_Success;
    }
  }
}

/* ================================================== */

/* Procedure to add a new peer. */
NSR_Status
NSR_AddPeer(NTP_Remote_Address *remote_addr, SourceParameters *params)
{
  int slot, found;

  assert(initialised);

#if 0
  LOG(LOGS_INFO, LOGF_NtpSources, "IP=%08lx port=%d", (unsigned long) remote_addr->ip_addr, remote_addr->port);
#endif

  /* Find empty bin & check that we don't have the address already */
  find_slot(remote_addr, &slot, &found);
  if (found) {
    return NSR_AlreadyInUse;
  } else {
    if (n_sources == MAX_SOURCES) {
      return NSR_TooManySources;
    } else {
      n_sources++;
      records[slot].remote_addr = *remote_addr;
      records[slot].in_use = 1;
      records[slot].data = NCR_GetPeerInstance(remote_addr, params); /* Will need params passing through */
      return NSR_Success;
    }
  }
}

/* ================================================== */

/* Procedure to remove a source.  We don't bother whether the port
   address is matched - we're only interested in removing a record for
   the right IP address.  Thus the caller can specify the port number
   as zero if it wishes. */
NSR_Status
NSR_RemoveSource(NTP_Remote_Address *remote_addr)
{
  int slot, found;

  assert(initialised);

  find_slot(remote_addr, &slot, &found);
  if (!found) {
    return NSR_NoSuchSource;
  } else {
    n_sources--;
    records[slot].in_use = 0;
    NCR_DestroyInstance(records[slot].data);
    return NSR_Success;
  }
}

/* ================================================== */

/* This routine is called by ntp_io when a new packet arrives off the network.*/
void
NSR_ProcessReceive(NTP_Packet *message, struct timeval *now, NTP_Remote_Address *remote_addr)
{
  int slot, found;

  assert(initialised);

#if 0
  LOG(LOGS_INFO, LOGF_NtpSources, "from (%s,%d) at %s",
      UTI_IPToDottedQuad(remote_addr->ip_addr),
      remote_addr->port, UTI_TimevalToString(now));
#endif
  
  find_slot(remote_addr, &slot, &found);
  if (found == 2) { /* Must match IP address AND port number */
    NCR_ProcessNoauthKnown(message, now, records[slot].data);
  } else {
    NCR_ProcessNoauthUnknown(message, now, remote_addr);
  }
}

/* ================================================== */

/* This routine is called by ntp_io when a new packet with an authentication tail arrives off the network */
void
NSR_ProcessAuthenticatedReceive(NTP_Packet *message, struct timeval *now, NTP_Remote_Address *remote_addr)
{
  int slot, found;

  assert(initialised);

  find_slot(remote_addr, &slot, &found);
  if (found == 2) {
    NCR_ProcessAuthKnown(message, now, records[slot].data);
  } else {
    NCR_ProcessAuthUnknown(message, now, remote_addr);
  }
}

/* ================================================== */

static void
slew_sources(struct timeval *raw,
             struct timeval *cooked,
             double dfreq,
             double afreq,
             double doffset,
             int is_step_change,
             void *anything)
{
  int i;

  for (i=0; i<N_RECORDS; i++) {
    if (records[i].in_use) {
#if 0
      LOG(LOGS_INFO, LOGF_Sources, "IP=%s dfreq=%f doff=%f",
          UTI_IPToDottedQuad(records[i].remote_addr.ip_addr), dfreq, doffset);
#endif

      NCR_SlewTimes(records[i].data, cooked, dfreq, doffset);
    }
  }

}

/* ================================================== */

int
NSR_TakeSourcesOnline(unsigned long mask, unsigned long address)
{
  int i;
  int any;
  unsigned long ip;

  any = 0;
  for (i=0; i<N_RECORDS; i++) {
    if (records[i].in_use) {
      ip = records[i].remote_addr.ip_addr;
      if ((ip & mask) == address) {
        any = 1;
        NCR_TakeSourceOnline(records[i].data);
      }
    }
  }

  return any;
}

/* ================================================== */

int
NSR_TakeSourcesOffline(unsigned long mask, unsigned long address)
{
  int i;
  int any;
  unsigned long ip;

  any = 0;
  for (i=0; i<N_RECORDS; i++) {
    if (records[i].in_use) {
      ip = records[i].remote_addr.ip_addr;
      if ((ip & mask) == address) {
        any = 1;
        NCR_TakeSourceOffline(records[i].data);
      }
    }
  }

  return any;
}

/* ================================================== */

int
NSR_ModifyMinpoll(unsigned long address, int new_minpoll)
{
  int slot, found;
  NTP_Remote_Address addr;
  addr.ip_addr = address;
  addr.port = 0;

  find_slot(&addr, &slot, &found);
  if (found == 0) {
    return 0;
  } else {
    NCR_ModifyMinpoll(records[slot].data, new_minpoll);
    return 1;
  }
}

/* ================================================== */

int
NSR_ModifyMaxpoll(unsigned long address, int new_maxpoll)
{
  int slot, found;
  NTP_Remote_Address addr;
  addr.ip_addr = address;
  addr.port = 0;

  find_slot(&addr, &slot, &found);
  if (found == 0) {
    return 0;
  } else {
    NCR_ModifyMaxpoll(records[slot].data, new_maxpoll);
    return 1;
  }
}

/* ================================================== */

int
NSR_ModifyMaxdelay(unsigned long address, double new_max_delay)
{
  int slot, found;
  NTP_Remote_Address addr;
  addr.ip_addr = address;
  addr.port = 0;

  find_slot(&addr, &slot, &found);
  if (found == 0) {
    return 0;
  } else {
    NCR_ModifyMaxdelay(records[slot].data, new_max_delay);
    return 1;
  }
}

/* ================================================== */

int
NSR_ModifyMaxdelayratio(unsigned long address, double new_max_delay_ratio)
{
  int slot, found;
  NTP_Remote_Address addr;
  addr.ip_addr = address;
  addr.port = 0;

  find_slot(&addr, &slot, &found);
  if (found == 0) {
    return 0;
  } else {
    NCR_ModifyMaxdelayratio(records[slot].data, new_max_delay_ratio);
    return 1;
  }
}

/* ================================================== */

int
NSR_InitiateSampleBurst(int n_good_samples, int n_total_samples,
                        unsigned long mask, unsigned long address)
{
  int i;
  int any;
  unsigned long ip;

  any = 0;
  for (i=0; i<N_RECORDS; i++) {
    if (records[i].in_use) {
      ip = records[i].remote_addr.ip_addr;
      if ((ip & mask) == address) {
        any = 1;
        NCR_InitiateSampleBurst(records[i].data, n_good_samples, n_total_samples);
      }
    }
  }

  return any;

}

/* ================================================== */
/* The ip address is assumed to be completed on input, that is how we
   identify the source record. */

void
NSR_ReportSource(RPT_SourceReport *report, struct timeval *now)
{
  NTP_Remote_Address rem_addr;
  int slot, found;

  rem_addr.ip_addr = report->ip_addr;
  rem_addr.port = 0;
  find_slot(&rem_addr, &slot, &found);
  if (found) {
    NCR_ReportSource(records[slot].data, report, now);
  } else {
    report->poll = 0;
    report->latest_meas_ago = 0;
  }
}

/* ================================================== */

void
NSR_GetActivityReport(RPT_ActivityReport *report)
{
  int i;

  report->online = 0;
  report->offline = 0;
  report->burst_online = 0;
  report->burst_offline = 0;

  for (i=0; i<N_RECORDS; i++) {
    if (records[i].in_use) {
      NCR_IncrementActivityCounters(records[i].data, &report->online, &report->offline,
                                    &report->burst_online, &report->burst_offline);
    }
  }
  return;
}


/* ================================================== */

