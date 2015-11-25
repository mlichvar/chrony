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
  uint16_t ntp_drops;
  uint16_t cmd_drops;
  int8_t ntp_rate;
  int8_t cmd_rate;
  int8_t ntp_timeout_rate;
  uint8_t ntp_burst;
  uint8_t cmd_burst;
  uint8_t flags;
  uint16_t _pad;
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

/* Thresholds for request rate to activate response rate limiting */

#define MIN_THRESHOLD (-10 * RATE_SCALE)
#define MAX_THRESHOLD (0 * RATE_SCALE)

static int ntp_threshold;
static int cmd_threshold;

/* Numbers of responses after the rate exceeded the threshold before
   actually dropping requests */

#define MIN_LEAK_BURST 0
#define MAX_LEAK_BURST 255

static int ntp_leak_burst;
static int cmd_leak_burst;

/* Rates at which responses are randomly allowed (in log2). This is
   necessary to prevent an attacker sending requests with spoofed
   source address from blocking responses to the client completely. */

#define MIN_LEAK_RATE 1
#define MAX_LEAK_RATE 4

static int ntp_leak_rate;
static int cmd_leak_rate;

/* Flag indicating whether the last response was dropped */
#define FLAG_NTP_DROPPED 0x1

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
  record->ntp_drops = record->cmd_drops = 0;
  record->ntp_rate = record->cmd_rate = INVALID_RATE;
  record->ntp_timeout_rate = INVALID_RATE;
  record->ntp_burst = record->cmd_burst = 0;
  record->flags = 0;
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
  int threshold, burst, leak_rate;

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

  if (CNF_GetNTPRateLimit(&threshold, &burst, &leak_rate))
    ntp_threshold = CLAMP(MIN_THRESHOLD, threshold * -RATE_SCALE, MAX_THRESHOLD);
  else
    ntp_threshold = INVALID_RATE;
  ntp_leak_burst = CLAMP(MIN_LEAK_BURST, burst, MAX_LEAK_BURST);
  ntp_leak_rate = CLAMP(MIN_LEAK_RATE, leak_rate, MAX_LEAK_RATE);

  if (CNF_GetCommandRateLimit(&threshold, &burst, &leak_rate))
    cmd_threshold = CLAMP(MIN_THRESHOLD, threshold * -RATE_SCALE, MAX_THRESHOLD);
  else
    cmd_threshold = INVALID_RATE;
  cmd_leak_burst = CLAMP(MIN_LEAK_BURST, burst, MAX_LEAK_BURST);
  cmd_leak_rate = CLAMP(MIN_LEAK_RATE, leak_rate, MAX_LEAK_RATE);
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

static int
get_index(Record *record)
{
  return record - (Record *)ARR_GetElements(records);
}

/* ================================================== */

int
CLG_LogNTPAccess(IPAddr *client, time_t now)
{
  Record *record;

  if (!active)
    return -1;

  record = get_record(client);
  if (record == NULL)
    return -1;

  record->ntp_hits++;

  /* Update one of the two rates depending on whether the previous request
     of the client had a reply or it timed out */
  if (record->flags & FLAG_NTP_DROPPED)
    record->ntp_timeout_rate = update_rate(record->ntp_timeout_rate,
                                           now, record->last_ntp_hit);
  else
    record->ntp_rate = update_rate(record->ntp_rate, now, record->last_ntp_hit);

  record->last_ntp_hit = now;

  DEBUG_LOG(LOGF_ClientLog, "NTP hits %"PRIu32" rate %d trate %d burst %d",
            record->ntp_hits, record->ntp_rate, record->ntp_timeout_rate,
            record->ntp_burst);

  return get_index(record);
}

/* ================================================== */

int
CLG_LogCommandAccess(IPAddr *client, time_t now)
{
  Record *record;

  if (!active)
    return -1;

  record = get_record(client);
  if (record == NULL)
    return -1;

  record->cmd_hits++;
  record->cmd_rate = update_rate(record->cmd_rate, now, record->last_cmd_hit);
  record->last_cmd_hit = now;

  DEBUG_LOG(LOGF_ClientLog, "Cmd hits %"PRIu32" rate %d burst %d",
            record->cmd_hits, record->cmd_rate, record->cmd_burst);

  return get_index(record);
}

/* ================================================== */

static int
limit_response_random(int leak_rate)
{
  static uint32_t rnd;
  static int bits_left = 0;
  int r;

  if (bits_left < leak_rate) {
    UTI_GetRandomBytes(&rnd, sizeof (rnd));
    bits_left = 8 * sizeof (rnd);
  }

  /* Return zero on average once per 2^leak_rate */
  r = rnd % (1U << leak_rate) ? 1 : 0;
  rnd >>= leak_rate;
  bits_left -= leak_rate;

  return r;
}

/* ================================================== */

int
CLG_LimitNTPResponseRate(int index)
{
  Record *record;
  int drop;

  record = ARR_GetElement(records, index);
  record->flags &= ~FLAG_NTP_DROPPED;

  /* Respond to all requests if the rate doesn't exceed the threshold */
  if (ntp_threshold == INVALID_RATE ||
      record->ntp_rate == INVALID_RATE ||
      record->ntp_rate <= ntp_threshold) {
    record->ntp_burst = 0;
    return 0;
  }

  /* Allow the client to send a burst of requests */
  if (record->ntp_burst < ntp_leak_burst) {
    record->ntp_burst++;
    return 0;
  }

  drop = limit_response_random(ntp_leak_rate);

  /* Poorly implemented clients may send new requests at even a higher rate
     when they are not getting replies.  If the request rate seems to be more
     than twice as much as when replies are sent, give up on rate limiting to
     reduce the amount of traffic.  Invert the sense of the leak to respond to
     most of the requests, but still keep the estimated rate updated. */
  if (record->ntp_timeout_rate != INVALID_RATE &&
      record->ntp_timeout_rate > record->ntp_rate + RATE_SCALE)
    drop = !drop;

  if (!drop)
    return 0;

  record->flags |= FLAG_NTP_DROPPED;
  record->ntp_drops++;

  return 1;
}

/* ================================================== */

int
CLG_LimitCommandResponseRate(int index)
{
  Record *record;

  record = ARR_GetElement(records, index);

  if (cmd_threshold == INVALID_RATE ||
      record->cmd_rate == INVALID_RATE ||
      record->cmd_rate <= cmd_threshold) {
    record->cmd_burst = 0;
    return 0;
  }

  if (record->cmd_burst < cmd_leak_burst) {
    record->cmd_burst++;
    return 0;
  }

  if (!limit_response_random(cmd_leak_rate))
    return 0;

  return 1;
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
