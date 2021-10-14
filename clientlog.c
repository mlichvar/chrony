/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2003
 * Copyright (C) Miroslav Lichvar  2009, 2015-2017
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
#include "ntp.h"
#include "reports.h"
#include "util.h"
#include "logging.h"

#define MAX_SERVICES 3

typedef struct {
  IPAddr ip_addr;
  uint32_t last_hit[MAX_SERVICES];
  uint32_t hits[MAX_SERVICES];
  uint16_t drops[MAX_SERVICES];
  uint16_t tokens[MAX_SERVICES];
  int8_t rate[MAX_SERVICES];
  int8_t ntp_timeout_rate;
  uint8_t drop_flags;
  NTP_int64 ntp_rx_ts;
  NTP_int64 ntp_tx_ts;
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

/* Times of last hits are saved as 32-bit fixed point values */
#define TS_FRAC 4
#define INVALID_TS 0

/* Static offset included in conversion to the fixed-point timestamps to
   randomise their alignment */
static uint32_t ts_offset;

/* Request rates are saved in the record as 8-bit scaled log2 values */
#define RATE_SCALE 4
#define MIN_RATE (-14 * RATE_SCALE)
#define INVALID_RATE -128

/* Response rates are controlled by token buckets.  The capacity and
   number of tokens spent on response are determined from configured
   minimum inverval between responses (in log2) and burst length. */

#define MIN_LIMIT_INTERVAL (-15 - TS_FRAC)
#define MAX_LIMIT_INTERVAL 12
#define MIN_LIMIT_BURST 1
#define MAX_LIMIT_BURST 255

static uint16_t max_tokens[MAX_SERVICES];
static uint16_t tokens_per_hit[MAX_SERVICES];

/* Reduction of token rates to avoid overflow of 16-bit counters.  Negative
   shift is used for coarse limiting with intervals shorter than -TS_FRAC. */
static int token_shift[MAX_SERVICES];

/* Rates at which responses are randomly allowed (in log2) when the
   buckets don't have enough tokens.  This is necessary in order to
   prevent an attacker sending requests with spoofed source address
   from blocking responses to the address completely. */

#define MIN_LEAK_RATE 1
#define MAX_LEAK_RATE 4

static int leak_rate[MAX_SERVICES];

/* Limit intervals in log2 */
static int limit_interval[MAX_SERVICES];

/* Flag indicating whether facility is turned on or not */
static int active;

/* Global statistics */
static uint32_t total_hits[MAX_SERVICES];
static uint32_t total_drops[MAX_SERVICES];
static uint32_t total_ntp_auth_hits;
static uint32_t total_record_drops;

#define NSEC_PER_SEC 1000000000U

/* ================================================== */

static int expand_hashtable(void);

/* ================================================== */

static int
compare_ts(uint32_t x, uint32_t y)
{
  if (x == y)
    return 0;
  if (y == INVALID_TS)
    return 1;
  return (int32_t)(x - y) > 0 ? 1 : -1;
}

/* ================================================== */

static int
compare_total_hits(Record *x, Record *y)
{
  uint32_t x_hits, y_hits;
  int i;

  for (i = 0, x_hits = y_hits = 0; i < MAX_SERVICES; i++) {
    x_hits += x->hits[i];
    y_hits += y->hits[i];
  }

  return x_hits > y_hits ? 1 : -1;
}

/* ================================================== */

static Record *
get_record(IPAddr *ip)
{
  uint32_t last_hit = 0, oldest_hit = 0;
  Record *record, *oldest_record;
  unsigned int first, i, j;

  if (!active || (ip->family != IPADDR_INET4 && ip->family != IPADDR_INET6))
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

      for (j = 0; j < MAX_SERVICES; j++) {
        if (j == 0 || compare_ts(last_hit, record->last_hit[j]) < 0)
          last_hit = record->last_hit[j];
      }

      if (!oldest_record || compare_ts(oldest_hit, last_hit) > 0 ||
          (oldest_hit == last_hit && compare_total_hits(oldest_record, record) > 0)) {
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
    total_record_drops++;
    break;
  }

  record->ip_addr = *ip;
  for (i = 0; i < MAX_SERVICES; i++)
    record->last_hit[i] = INVALID_TS;
  for (i = 0; i < MAX_SERVICES; i++)
    record->hits[i] = 0;
  for (i = 0; i < MAX_SERVICES; i++)
    record->drops[i] = 0;
  for (i = 0; i < MAX_SERVICES; i++)
    record->tokens[i] = max_tokens[i];
  for (i = 0; i < MAX_SERVICES; i++)
    record->rate[i] = INVALID_RATE;
  record->ntp_timeout_rate = INVALID_RATE;
  record->drop_flags = 0;
  UTI_ZeroNtp64(&record->ntp_rx_ts);
  UTI_ZeroNtp64(&record->ntp_tx_ts);

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

static void
set_bucket_params(int interval, int burst, uint16_t *max_tokens,
                  uint16_t *tokens_per_packet, int *token_shift)
{
  interval = CLAMP(MIN_LIMIT_INTERVAL, interval, MAX_LIMIT_INTERVAL);
  burst = CLAMP(MIN_LIMIT_BURST, burst, MAX_LIMIT_BURST);

  if (interval >= -TS_FRAC) {
    /* Find the smallest shift with which the maximum number fits in 16 bits */
    for (*token_shift = 0; *token_shift < interval + TS_FRAC; (*token_shift)++) {
      if (burst << (TS_FRAC + interval - *token_shift) < 1U << 16)
        break;
    }
  } else {
    /* Coarse rate limiting */
    *token_shift = interval + TS_FRAC;
    *tokens_per_packet = 1;
    burst = MAX(1U << -*token_shift, burst);
  }

  *tokens_per_packet = 1U << (TS_FRAC + interval - *token_shift);
  *max_tokens = *tokens_per_packet * burst;

  DEBUG_LOG("Tokens max %d packet %d shift %d",
            *max_tokens, *tokens_per_packet, *token_shift);
}

/* ================================================== */

void
CLG_Initialise(void)
{
  int i, interval, burst, lrate, slots2;

  for (i = 0; i < MAX_SERVICES; i++) {
    max_tokens[i] = 0;
    tokens_per_hit[i] = 0;
    token_shift[i] = 0;
    leak_rate[i] = 0;
    limit_interval[i] = MIN_LIMIT_INTERVAL;

    switch (i) {
      case CLG_NTP:
        if (!CNF_GetNTPRateLimit(&interval, &burst, &lrate))
          continue;
        break;
      case CLG_NTSKE:
        if (!CNF_GetNtsRateLimit(&interval, &burst, &lrate))
          continue;
        break;
      case CLG_CMDMON:
        if (!CNF_GetCommandRateLimit(&interval, &burst, &lrate))
          continue;
        break;
      default:
        assert(0);
    }

    set_bucket_params(interval, burst, &max_tokens[i], &tokens_per_hit[i], &token_shift[i]);
    leak_rate[i] = CLAMP(MIN_LEAK_RATE, lrate, MAX_LEAK_RATE);
    limit_interval[i] = CLAMP(MIN_LIMIT_INTERVAL, interval, MAX_LIMIT_INTERVAL);
  }

  active = !CNF_GetNoClientLog();
  if (!active) {
    for (i = 0; i < MAX_SERVICES; i++) {
      if (leak_rate[i] != 0)
        LOG_FATAL("Rate limiting cannot be enabled with noclientlog");
    }
    return;
  }

  /* Calculate the maximum number of slots that can be allocated in the
     configured memory limit.  Take into account expanding of the hash
     table where two copies exist at the same time. */
  max_slots = CNF_GetClientLogLimit() / (sizeof (Record) * SLOT_SIZE * 3 / 2);
  max_slots = CLAMP(MIN_SLOTS, max_slots, MAX_SLOTS);
  for (slots2 = 0; 1U << (slots2 + 1) <= max_slots; slots2++)
    ;

  DEBUG_LOG("Max records %u", 1U << (slots2 + SLOT_BITS));

  slots = 0;
  records = NULL;

  expand_hashtable();

  UTI_GetRandomBytes(&ts_offset, sizeof (ts_offset));
  ts_offset %= NSEC_PER_SEC / (1U << TS_FRAC);
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

static uint32_t
get_ts_from_timespec(struct timespec *ts)
{
  uint32_t sec = ts->tv_sec, nsec = ts->tv_nsec;

  nsec += ts_offset;
  if (nsec >= NSEC_PER_SEC) {
    nsec -= NSEC_PER_SEC;
    sec++;
  }

  /* This is fast and accurate enough */
  return sec << TS_FRAC | (140740U * (nsec >> 15)) >> (32 - TS_FRAC);
}

/* ================================================== */

static void
update_record(CLG_Service service, Record *record, struct timespec *now)
{
  uint32_t interval, now_ts, prev_hit, tokens;
  int interval2, tshift, mtokens;
  int8_t *rate;

  now_ts = get_ts_from_timespec(now);

  prev_hit = record->last_hit[service];
  record->last_hit[service] = now_ts;
  record->hits[service]++;

  interval = now_ts - prev_hit;

  if (prev_hit == INVALID_TS || (int32_t)interval < 0)
    return;

  tshift = token_shift[service];
  mtokens = max_tokens[service];

  if (tshift >= 0)
    tokens = (now_ts >> tshift) - (prev_hit >> tshift);
  else if (now_ts - prev_hit > mtokens)
    tokens = mtokens;
  else
    tokens = (now_ts - prev_hit) << -tshift;
  record->tokens[service] = MIN(record->tokens[service] + tokens, mtokens);

  /* Convert the interval to scaled and rounded log2 */
  if (interval) {
    interval += interval >> 1;
    for (interval2 = -RATE_SCALE * TS_FRAC; interval2 < -MIN_RATE;
         interval2 += RATE_SCALE) {
      if (interval <= 1)
        break;
      interval >>= 1;
    }
  } else {
    interval2 = -RATE_SCALE * (TS_FRAC + 1);
  }

  /* For the NTP service, update one of the two rates depending on whether
     the previous request of the client had a reply or it timed out */
  rate = service == CLG_NTP && record->drop_flags & (1U << service) ?
           &record->ntp_timeout_rate : &record->rate[service];

  /* Update the rate in a rough approximation of exponential moving average */
  if (*rate == INVALID_RATE) {
    *rate = -interval2;
  } else {
    if (*rate < -interval2) {
      (*rate)++;
    } else if (*rate > -interval2) {
      if (*rate > RATE_SCALE * 5 / 2 - interval2)
        *rate = RATE_SCALE * 5 / 2 - interval2;
      else
        *rate = (*rate - interval2 - 1) / 2;
    }
  }
}

/* ================================================== */

static int
get_index(Record *record)
{
  return record - (Record *)ARR_GetElements(records);
}

/* ================================================== */

int
CLG_GetClientIndex(IPAddr *client)
{
  Record *record;

  record = get_record(client);
  if (record == NULL)
    return -1;

  return get_index(record);
}

/* ================================================== */

static void
check_service_number(CLG_Service service)
{
  assert(service >= 0 && service <= MAX_SERVICES);
}

/* ================================================== */

int
CLG_LogServiceAccess(CLG_Service service, IPAddr *client, struct timespec *now)
{
  Record *record;

  check_service_number(service);

  total_hits[service]++;

  record = get_record(client);
  if (record == NULL)
    return -1;

  update_record(service, record, now);

  DEBUG_LOG("service %d hits %"PRIu32" rate %d trate %d tokens %d",
            (int)service, record->hits[service], record->rate[service],
            service == CLG_NTP ? record->ntp_timeout_rate : INVALID_RATE,
            record->tokens[service]);

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
CLG_LimitServiceRate(CLG_Service service, int index)
{
  Record *record;
  int drop;

  check_service_number(service);

  if (tokens_per_hit[service] == 0)
    return 0;

  record = ARR_GetElement(records, index);
  record->drop_flags &= ~(1U << service);

  if (record->tokens[service] >= tokens_per_hit[service]) {
    record->tokens[service] -= tokens_per_hit[service];
    return 0;
  }

  drop = limit_response_random(leak_rate[service]);

  /* Poorly implemented NTP clients can send requests at a higher rate
     when they are not getting replies.  If the request rate seems to be more
     than twice as much as when replies are sent, give up on rate limiting to
     reduce the amount of traffic.  Invert the sense of the leak to respond to
     most of the requests, but still keep the estimated rate updated. */
  if (service == CLG_NTP && record->ntp_timeout_rate != INVALID_RATE &&
      record->ntp_timeout_rate > record->rate[service] + RATE_SCALE)
    drop = !drop;

  if (!drop) {
    record->tokens[service] = 0;
    return 0;
  }

  record->drop_flags |= 1U << service;
  record->drops[service]++;
  total_drops[service]++;

  return 1;
}

/* ================================================== */

void
CLG_LogAuthNtpRequest(void)
{
  total_ntp_auth_hits++;
}

/* ================================================== */

void CLG_GetNtpTimestamps(int index, NTP_int64 **rx_ts, NTP_int64 **tx_ts)
{
  Record *record;

  record = ARR_GetElement(records, index);

  *rx_ts = &record->ntp_rx_ts;
  *tx_ts = &record->ntp_tx_ts;
}

/* ================================================== */

int
CLG_GetNtpMinPoll(void)
{
  return limit_interval[CLG_NTP];
}

/* ================================================== */

int
CLG_GetNumberOfIndices(void)
{
  if (!active)
    return -1;

  return ARR_GetSize(records);
}

/* ================================================== */

static int get_interval(int rate)
{
  if (rate == INVALID_RATE)
    return 127;

  rate += rate > 0 ? RATE_SCALE / 2 : -RATE_SCALE / 2;

  return rate / -RATE_SCALE;
}

/* ================================================== */

static uint32_t get_last_ago(uint32_t x, uint32_t y)
{
  if (y == INVALID_TS || (int32_t)(x - y) < 0)
    return -1;

  return (x - y) >> TS_FRAC;
}

/* ================================================== */

int
CLG_GetClientAccessReportByIndex(int index, int reset, uint32_t min_hits,
                                 RPT_ClientAccessByIndex_Report *report, struct timespec *now)
{
  Record *record;
  uint32_t now_ts;
  int i, r;

  if (!active || index < 0 || index >= ARR_GetSize(records))
    return 0;

  record = ARR_GetElement(records, index);

  if (record->ip_addr.family == IPADDR_UNSPEC)
    return 0;

  if (min_hits == 0) {
    r = 1;
  } else {
    for (i = r = 0; i < MAX_SERVICES; i++) {
      if (record->hits[i] >= min_hits) {
        r = 1;
        break;
      }
    }
  }

  if (r) {
    now_ts = get_ts_from_timespec(now);

    report->ip_addr = record->ip_addr;
    report->ntp_hits = record->hits[CLG_NTP];
    report->nke_hits = record->hits[CLG_NTSKE];
    report->cmd_hits = record->hits[CLG_CMDMON];
    report->ntp_drops = record->drops[CLG_NTP];
    report->nke_drops = record->drops[CLG_NTSKE];
    report->cmd_drops = record->drops[CLG_CMDMON];
    report->ntp_interval = get_interval(record->rate[CLG_NTP]);
    report->nke_interval = get_interval(record->rate[CLG_NTSKE]);
    report->cmd_interval = get_interval(record->rate[CLG_CMDMON]);
    report->ntp_timeout_interval = get_interval(record->ntp_timeout_rate);
    report->last_ntp_hit_ago = get_last_ago(now_ts, record->last_hit[CLG_NTP]);
    report->last_nke_hit_ago = get_last_ago(now_ts, record->last_hit[CLG_NTSKE]);
    report->last_cmd_hit_ago = get_last_ago(now_ts, record->last_hit[CLG_CMDMON]);
  }

  if (reset) {
    for (i = 0; i < MAX_SERVICES; i++) {
      record->hits[i] = 0;
      record->drops[i] = 0;
    }
  }

  return r;
}

/* ================================================== */

void
CLG_GetServerStatsReport(RPT_ServerStatsReport *report)
{
  report->ntp_hits = total_hits[CLG_NTP];
  report->nke_hits = total_hits[CLG_NTSKE];
  report->cmd_hits = total_hits[CLG_CMDMON];
  report->ntp_drops = total_drops[CLG_NTP];
  report->nke_drops = total_drops[CLG_NTSKE];
  report->cmd_drops = total_drops[CLG_CMDMON];
  report->log_drops = total_record_drops;
  report->ntp_auth_hits = total_ntp_auth_hits;
}
