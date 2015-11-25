/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2003
 * Copyright (C) Miroslav Lichvar  2009
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

  This module keeps a count of the number of successful accesses by
  clients, and the times of the last accesses.

  This can be used for status reporting, and (in the case of a
  server), if it needs to know which clients have made use of its data
  recently.

  */

#include "config.h"

#include "sysincl.h"

#include "array.h"
#include "clientlog.h"
#include "conf.h"
#include "memory.h"
#include "reports.h"
#include "util.h"
#include "logging.h"

typedef struct {
  IPAddr ip_addr;
  uint32_t ntp_hits;
  uint32_t cmd_hits;
  int8_t ntp_rate;
  int8_t cmd_rate;
  time_t last_ntp_hit;
  time_t last_cmd_hit;
} Record;

/* Hash table of records, there is a fixed number of records per slot */
static ARR_Instance records;

#define SLOT_BITS 4

/* Number of records in one slot of the hash table */
#define SLOT_SIZE (1U << SLOT_BITS)

/* Minimum number of slots */
#define MIN_SLOTS 1

/* Maximum number of slots, this is a hard limit */
#define MAX_SLOTS (1U << (24 - SLOT_BITS))

/* Number of slots in the hash table */
static unsigned int slots;

/* Maximum number of slots given memory allocation limit */
static unsigned int max_slots;

/* Request rates are saved in the record as 8-bit scaled log2 values */
#define RATE_SCALE 4
#define MIN_RATE (-14 * RATE_SCALE)
#define INVALID_RATE -128

/* Flag indicating whether facility is turned on or not */
static int active;

/* ================================================== */

static int expand_hashtable(void);

/* ================================================== */

static Record *
get_record(IPAddr *ip)
{
  unsigned int first, i;
  time_t last_hit, oldest_hit = 0;
  Record *record, *oldest_record;

  if (ip->family != IPADDR_INET4 && ip->family != IPADDR_INET6)
    return NULL;

  while (1) {
    /* Get index of the first record in the slot */
    first = UTI_IPToHash(ip) % slots * SLOT_SIZE;

    for (i = 0, oldest_record = NULL; i < SLOT_SIZE; i++) {
      record = ARR_GetElement(records, first + i);

      if (!UTI_CompareIPs(ip, &record->ip_addr, NULL))
        return record;

      if (record->ip_addr.family == IPADDR_UNSPEC)
        break;

      last_hit = MAX(record->last_ntp_hit, record->last_cmd_hit);

      if (!oldest_record ||
          oldest_hit > last_hit ||
          (oldest_hit == last_hit && record->ntp_hits + record->cmd_hits <
           oldest_record->ntp_hits + oldest_record->cmd_hits)) {
        oldest_record = record;
        oldest_hit = last_hit;
      }
    }

    /* If the slot still has an empty record, use it */
    if (record->ip_addr.family == IPADDR_UNSPEC)
      break;

    /* Resize the table if possible and try again as the new slot may
       have some empty records */
    if (expand_hashtable())
      continue;

    /* There is no other option, replace the oldest record */
    record = oldest_record;
    break;
  }

  record->ip_addr = *ip;
  record->ntp_hits = record->cmd_hits = 0;
  record->ntp_rate = record->cmd_rate = INVALID_RATE;
  record->last_ntp_hit = record->last_cmd_hit = 0;

  return record;
}

/* ================================================== */

static int
expand_hashtable(void)
{
  ARR_Instance old_records;
  Record *old_record, *new_record;
  unsigned int i;

  old_records = records;

  if (2 * slots > max_slots)
    return 0;

  records = ARR_CreateInstance(sizeof (Record));

  slots = MAX(MIN_SLOTS, 2 * slots);
  assert(slots <= max_slots);

  ARR_SetSize(records, slots * SLOT_SIZE);

  /* Mark all new records as empty */
  for (i = 0; i < slots * SLOT_SIZE; i++) {
    new_record = ARR_GetElement(records, i);
    new_record->ip_addr.family = IPADDR_UNSPEC;
  }

  if (!old_records)
    return 1;

  /* Copy old records to the new hash table */
  for (i = 0; i < ARR_GetSize(old_records); i++) {
    old_record = ARR_GetElement(old_records, i);
    if (old_record->ip_addr.family == IPADDR_UNSPEC)
      continue;

    new_record = get_record(&old_record->ip_addr);

    assert(new_record);
    *new_record = *old_record;
  }

  ARR_DestroyInstance(old_records);

  return 1;
}

/* ================================================== */

static int
update_rate(int rate, time_t now, time_t last_hit)
{
  uint32_t interval;
  int interval2;

  if (!last_hit || now < last_hit)
    return rate;

  interval = now - last_hit;

  /* Convert the interval to scaled and rounded log2 */
  if (interval) {
    interval += interval >> 1;
    for (interval2 = 0; interval2 < -MIN_RATE; interval2 += RATE_SCALE) {
      if (interval <= 1)
        break;
      interval >>= 1;
    }
  } else {
    interval2 = -RATE_SCALE;
  }

  if (rate == INVALID_RATE)
    return -interval2;

  /* Update the rate in a rough approximation of exponential moving average */
  if (rate < -interval2) {
    rate++;
  } else if (rate > -interval2) {
    if (rate > RATE_SCALE * 5 / 2 - interval2)
      rate = RATE_SCALE * 5 / 2 - interval2;
    else
      rate = (rate - interval2 - 1) / 2;
  }

  return rate;
}

/* ================================================== */

void
CLG_Initialise(void)
{
  active = !CNF_GetNoClientLog();
  if (!active)
    return;

  /* Calculate the maximum number of slots that can be allocated in the
     configured memory limit.  Take into account expanding of the hash
     table where two copies exist at the same time. */
  max_slots = CNF_GetClientLogLimit() / (sizeof (Record) * SLOT_SIZE * 3 / 2);
  max_slots = CLAMP(MIN_SLOTS, max_slots, MAX_SLOTS);

  slots = 0;
  records = NULL;

  expand_hashtable();
}

/* ================================================== */

void
CLG_Finalise(void)
{
  if (!active)
    return;

  ARR_DestroyInstance(records);
}

/* ================================================== */

void
CLG_LogNTPAccess(IPAddr *client, time_t now)
{
  Record *record;

  if (!active)
    return;

  record = get_record(client);
  if (record == NULL)
    return;

  record->ntp_hits++;
  record->ntp_rate = update_rate(record->ntp_rate, now, record->last_ntp_hit);
  record->last_ntp_hit = now;

  DEBUG_LOG(LOGF_ClientLog, "NTP hits %"PRIu32" rate %d",
            record->ntp_hits, record->ntp_rate);
}

/* ================================================== */

void
CLG_LogCommandAccess(IPAddr *client, time_t now)
{
  Record *record;

  if (!active)
    return;

  record = get_record(client);
  if (record == NULL)
    return;

  record->cmd_hits++;
  record->cmd_rate = update_rate(record->cmd_rate, now, record->last_cmd_hit);
  record->last_cmd_hit = now;

  DEBUG_LOG(LOGF_ClientLog, "Cmd hits %"PRIu32" rate %d",
            record->cmd_hits, record->cmd_rate);
}

/* ================================================== */

CLG_Status
CLG_GetClientAccessReportByIndex(int index, RPT_ClientAccessByIndex_Report *report,
                                 time_t now, unsigned long *n_indices)
{
  Record *record;

  if (!active)
    return CLG_INACTIVE;

  *n_indices = ARR_GetSize(records);
  if (index < 0 || index >= *n_indices)
    return CLG_INDEXTOOLARGE;
    
  record = ARR_GetElement(records, index);

  report->ip_addr = record->ip_addr;
  report->ntp_hits = record->ntp_hits;
  report->cmd_hits = record->cmd_hits;
  report->last_ntp_hit_ago = now - record->last_ntp_hit;
  report->last_cmd_hit_ago = now - record->last_cmd_hit;

  return CLG_SUCCESS;
}
