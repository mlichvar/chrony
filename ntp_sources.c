/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2003
 * Copyright (C) Miroslav Lichvar  2011-2012, 2014
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

  Functions which manage the pool of NTP sources that we are currently
  a client of or peering with.

  */

#include "config.h"

#include "sysincl.h"

#include "array.h"
#include "ntp_sources.h"
#include "ntp_core.h"
#include "util.h"
#include "logging.h"
#include "local.h"
#include "memory.h"
#include "nameserv_async.h"
#include "sched.h"

/* ================================================== */

/* Record type private to this file, used to store information about
   particular sources */
typedef struct {
  NTP_Remote_Address *remote_addr; /* The address of this source, non-NULL
                                      means this slot in table is in use */
  NCR_Instance data;            /* Data for the protocol engine for this source */
} SourceRecord;

/* Hash table of SourceRecord, the size should be a power of two */
static ARR_Instance records;

/* Number of sources in the hash table */
static int n_sources;

/* Flag indicating new sources will be started automatically when added */
static int auto_start_sources = 0;

/* Source with unknown address (which may be resolved later) */
struct UnresolvedSource {
  char *name;
  int port;
  NTP_Source_Type type;
  SourceParameters params;
  struct UnresolvedSource *next;
};

#define RESOLVE_INTERVAL_UNIT 7
#define MIN_RESOLVE_INTERVAL 2
#define MAX_RESOLVE_INTERVAL 9

static struct UnresolvedSource *unresolved_sources = NULL;
static int resolving_interval = 0;
static SCH_TimeoutID resolving_id;
static struct UnresolvedSource *resolving_source = NULL;
static NSR_SourceResolvingEndHandler resolving_end_handler = NULL;

/* ================================================== */
/* Forward prototypes */

static void resolve_sources(void *arg);
static void rehash_records(void);

static void
slew_sources(struct timeval *raw,
             struct timeval *cooked,
             double dfreq,
             double doffset,
             LCL_ChangeType change_type,
             void *anything);

/* ================================================== */

/* Flag indicating whether module is initialised */
static int initialised = 0;

/* ================================================== */

static SourceRecord *
get_record(unsigned index)
{
  return (SourceRecord *)ARR_GetElement(records, index);
}

/* ================================================== */

void
NSR_Initialise(void)
{
  n_sources = 0;
  initialised = 1;

  records = ARR_CreateInstance(sizeof (SourceRecord));
  rehash_records();

  LCL_AddParameterChangeHandler(slew_sources, NULL);
}

/* ================================================== */

void
NSR_Finalise(void)
{
  SourceRecord *record;
  struct UnresolvedSource *us;
  unsigned int i;

  for (i = 0; i < ARR_GetSize(records); i++) {
    record = get_record(i);
    if (!record->remote_addr)
      continue;
    record->remote_addr = NULL;
    NCR_DestroyInstance(record->data);
  }

  ARR_DestroyInstance(records);

  while (unresolved_sources) {
    us = unresolved_sources;
    unresolved_sources = us->next;
    Free(us->name);
    Free(us);
  }

  initialised = 0;
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
  SourceRecord *record;
  uint32_t hash;
  unsigned int i, size;
  unsigned short port;
  uint8_t *ip6;

  size = ARR_GetSize(records);
  
  switch (remote_addr->ip_addr.family) {
    case IPADDR_INET6:
      ip6 = remote_addr->ip_addr.addr.in6;
      hash = (ip6[0] ^ ip6[4] ^ ip6[8] ^ ip6[12]) |
           (ip6[1] ^ ip6[5] ^ ip6[9] ^ ip6[13]) << 8 |
           (ip6[2] ^ ip6[6] ^ ip6[10] ^ ip6[14]) << 16 |
           (ip6[3] ^ ip6[7] ^ ip6[11] ^ ip6[15]) << 24;
      break;
    case IPADDR_INET4:
      hash = remote_addr->ip_addr.addr.in4;
      break;
    default:
      *found = *slot = 0;
      return;
  }

  port = remote_addr->port;

  for (i = 0; i < size / 2; i++) {
    /* Use quadratic probing */
    *slot = (hash + (i + i * i) / 2) % size;
    record = get_record(*slot);

    if (!record->remote_addr)
      break;

    if (!UTI_CompareIPs(&record->remote_addr->ip_addr,
                        &remote_addr->ip_addr, NULL)) {
      *found = record->remote_addr->port == port ? 2 : 1;
      return;
    }
  }

  *found = 0;
}

/* ================================================== */

/* Check if hash table of given size is sufficient to contain sources */
static int
check_hashtable_size(unsigned int sources, unsigned int size)
{
  return sources * 2 + 1 < size;
}

/* ================================================== */

static void
rehash_records(void)
{
  SourceRecord *record, *temp_records;
  unsigned int i, old_size, new_size;
  int slot, found;

  old_size = ARR_GetSize(records);

  temp_records = MallocArray(SourceRecord, old_size);
  memcpy(temp_records, ARR_GetElements(records), old_size * sizeof (SourceRecord));

  /* The size of the hash table is always a power of two */
  for (new_size = 4; !check_hashtable_size(n_sources, new_size); new_size *= 2)
    ;

  ARR_SetSize(records, new_size);

  for (i = 0; i < new_size; i++)
    get_record(i)->remote_addr = NULL;

  for (i = 0; i < old_size; i++) {
    if (!temp_records[i].remote_addr)
      continue;

    find_slot(temp_records[i].remote_addr, &slot, &found);
    assert(!found);

    record = get_record(slot);
    record->remote_addr = temp_records[i].remote_addr;
    record->data = temp_records[i].data;
  }

  Free(temp_records);
}

/* ================================================== */

/* Procedure to add a new source */
NSR_Status
NSR_AddSource(NTP_Remote_Address *remote_addr, NTP_Source_Type type, SourceParameters *params)
{
  SourceRecord *record;
  int slot, found;

  assert(initialised);

  /* Find empty bin & check that we don't have the address already */
  find_slot(remote_addr, &slot, &found);
  if (found) {
    return NSR_AlreadyInUse;
  } else {
    if (remote_addr->ip_addr.family != IPADDR_INET4 &&
               remote_addr->ip_addr.family != IPADDR_INET6) {
      return NSR_InvalidAF;
    } else {
      n_sources++;

      if (!check_hashtable_size(n_sources, ARR_GetSize(records))) {
        rehash_records();
        find_slot(remote_addr, &slot, &found);
      }

      assert(!found);
      record = get_record(slot);
      record->data = NCR_GetInstance(remote_addr, type, params);
      record->remote_addr = NCR_GetRemoteAddress(record->data);

      if (auto_start_sources)
        NCR_StartInstance(record->data);

      return NSR_Success;
    }
  }
}

/* ================================================== */

static void
name_resolve_handler(DNS_Status status, IPAddr *ip_addr, void *anything)
{
  struct UnresolvedSource *us, **i, *next;
  NTP_Remote_Address address;

  us = (struct UnresolvedSource *)anything;

  assert(us == resolving_source);

  switch (status) {
    case DNS_TryAgain:
      break;
    case DNS_Success:
      DEBUG_LOG(LOGF_NtpSources, "%s resolved to %s", us->name, UTI_IPToString(ip_addr));
      address.ip_addr = *ip_addr;
      address.port = us->port;
      NSR_AddSource(&address, us->type, &us->params);
      break;
    case DNS_Failure:
      LOG(LOGS_WARN, LOGF_NtpSources, "Invalid host %s", us->name);
      break;
    default:
      assert(0);
  }

  next = us->next;

  if (status != DNS_TryAgain) {
    /* Remove the source from the list */
    for (i = &unresolved_sources; *i; i = &(*i)->next) {
      if (*i == us) {
        *i = us->next;
        Free(us->name);
        Free(us);
        break;
      }
    }
  }

  resolving_source = next;

  if (next) {
    /* Continue with the next source in the list */
    DEBUG_LOG(LOGF_NtpSources, "resolving %s", next->name);
    DNS_Name2IPAddressAsync(next->name, name_resolve_handler, next);
  } else {
    /* This was the last source in the list. If some sources couldn't
       be resolved, try again in exponentially increasing interval. */
    if (unresolved_sources) {
      if (resolving_interval < MIN_RESOLVE_INTERVAL)
        resolving_interval = MIN_RESOLVE_INTERVAL;
      else if (resolving_interval < MAX_RESOLVE_INTERVAL)
        resolving_interval++;
      resolving_id = SCH_AddTimeoutByDelay(RESOLVE_INTERVAL_UNIT *
          (1 << resolving_interval), resolve_sources, NULL);
    } else {
      resolving_interval = 0;
    }

    /* This round of resolving is done */
    if (resolving_end_handler)
      (resolving_end_handler)();
  }
}

/* ================================================== */

static void
resolve_sources(void *arg)
{
  struct UnresolvedSource *us;

  assert(!resolving_source);

  DNS_Reload();

  /* Start with the first source in the list, name_resolve_handler
     will iterate over the rest */
  us = unresolved_sources;

  resolving_source = us;
  DEBUG_LOG(LOGF_NtpSources, "resolving %s", us->name);
  DNS_Name2IPAddressAsync(us->name, name_resolve_handler, us);
}

/* ================================================== */

/* Procedure to add a new server or peer source, but instead of an IP address
   only a name is provided */
void
NSR_AddUnresolvedSource(char *name, int port, NTP_Source_Type type, SourceParameters *params)
{
  struct UnresolvedSource *us, **i;

  us = MallocNew(struct UnresolvedSource);

  us->name = name;
  us->port = port;
  us->type = type;
  us->params = *params;
  us->next = NULL;

  for (i = &unresolved_sources; *i; i = &(*i)->next)
    ;
  *i = us;
}

/* ================================================== */

void
NSR_SetSourceResolvingEndHandler(NSR_SourceResolvingEndHandler handler)
{
  resolving_end_handler = handler;
}

/* ================================================== */

void
NSR_ResolveSources(void)
{
  /* Try to resolve unresolved sources now */
  if (unresolved_sources) {
    /* Make sure no resolving is currently running */
    if (!resolving_source) {
      if (resolving_interval) {
        SCH_RemoveTimeout(resolving_id);
        resolving_interval--;
      }
      resolve_sources(NULL);
    }
  } else {
    /* No unresolved sources, we are done */
    if (resolving_end_handler)
      (resolving_end_handler)();
  }
}

/* ================================================== */

void NSR_StartSources(void)
{
  unsigned int i;

  for (i = 0; i < ARR_GetSize(records); i++) {
    if (!get_record(i)->remote_addr)
      continue;
    NCR_StartInstance(get_record(i)->data);
  }
}

/* ================================================== */

void NSR_AutoStartSources(void)
{
  auto_start_sources = 1;
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
  }

  n_sources--;
  get_record(slot)->remote_addr = NULL;
  NCR_DestroyInstance(get_record(slot)->data);

  /* Rehash the table to make sure there are no broken probe sequences.
     This is costly, but it's not expected to happen frequently. */

  rehash_records();

  return NSR_Success;
}

/* ================================================== */

void
NSR_RemoveAllSources(void)
{
  SourceRecord *record;
  unsigned int i;

  for (i = 0; i < ARR_GetSize(records); i++) {
    record = get_record(i);
    if (!record->remote_addr)
      continue;
    NCR_DestroyInstance(record->data);
    record->remote_addr = NULL;
  }
}

/* ================================================== */

/* This routine is called by ntp_io when a new packet arrives off the network,
   possibly with an authentication tail */
void
NSR_ProcessReceive(NTP_Packet *message, struct timeval *now, double now_err, NTP_Remote_Address *remote_addr, NTP_Local_Address *local_addr, int length)
{
  int slot, found;

  assert(initialised);

  find_slot(remote_addr, &slot, &found);
  if (found == 2) { /* Must match IP address AND port number */
    NCR_ProcessKnown(message, now, now_err, get_record(slot)->data,
                     local_addr->sock_fd, length);
  } else {
    NCR_ProcessUnknown(message, now, now_err, remote_addr, local_addr, length);
  }
}

/* ================================================== */

static void
slew_sources(struct timeval *raw,
             struct timeval *cooked,
             double dfreq,
             double doffset,
             LCL_ChangeType change_type,
             void *anything)
{
  SourceRecord *record;
  unsigned int i;

  for (i = 0; i < ARR_GetSize(records); i++) {
    record = get_record(i);
    if (record->remote_addr) {
      if (change_type == LCL_ChangeUnknownStep) {
        NCR_ResetInstance(record->data);
      } else {
        NCR_SlewTimes(record->data, cooked, dfreq, doffset);
      }
    }
  }
}

/* ================================================== */

int
NSR_TakeSourcesOnline(IPAddr *mask, IPAddr *address)
{
  SourceRecord *record;
  unsigned int i;
  int any;

  NSR_ResolveSources();

  any = 0;
  for (i = 0; i < ARR_GetSize(records); i++) {
    record = get_record(i);
    if (record->remote_addr) {
      if (address->family == IPADDR_UNSPEC ||
          !UTI_CompareIPs(&record->remote_addr->ip_addr, address, mask)) {
        any = 1;
        NCR_TakeSourceOnline(record->data);
      }
    }
  }

  if (address->family == IPADDR_UNSPEC) {
    struct UnresolvedSource *us;

    for (us = unresolved_sources; us; us = us->next) {
      any = 1;
      us->params.online = 1;
    }
  }

  return any;
}

/* ================================================== */

int
NSR_TakeSourcesOffline(IPAddr *mask, IPAddr *address)
{
  SourceRecord *record, *syncpeer;
  unsigned int i, any;

  any = 0;
  syncpeer = NULL;
  for (i = 0; i < ARR_GetSize(records); i++) {
    record = get_record(i);
    if (record->remote_addr) {
      if (address->family == IPADDR_UNSPEC ||
          !UTI_CompareIPs(&record->remote_addr->ip_addr, address, mask)) {
        any = 1;
        if (NCR_IsSyncPeer(record->data)) {
          syncpeer = record;
          continue;
        }
        NCR_TakeSourceOffline(record->data);
      }
    }
  }

  /* Take sync peer offline as last to avoid reference switching */
  if (syncpeer) {
    NCR_TakeSourceOffline(syncpeer->data);
  }

  if (address->family == IPADDR_UNSPEC) {
    struct UnresolvedSource *us;

    for (us = unresolved_sources; us; us = us->next) {
      any = 1;
      us->params.online = 0;
    }
  }

  return any;
}

/* ================================================== */

int
NSR_ModifyMinpoll(IPAddr *address, int new_minpoll)
{
  int slot, found;
  NTP_Remote_Address addr;
  addr.ip_addr = *address;
  addr.port = 0;

  find_slot(&addr, &slot, &found);
  if (found == 0) {
    return 0;
  } else {
    NCR_ModifyMinpoll(get_record(slot)->data, new_minpoll);
    return 1;
  }
}

/* ================================================== */

int
NSR_ModifyMaxpoll(IPAddr *address, int new_maxpoll)
{
  int slot, found;
  NTP_Remote_Address addr;
  addr.ip_addr = *address;
  addr.port = 0;

  find_slot(&addr, &slot, &found);
  if (found == 0) {
    return 0;
  } else {
    NCR_ModifyMaxpoll(get_record(slot)->data, new_maxpoll);
    return 1;
  }
}

/* ================================================== */

int
NSR_ModifyMaxdelay(IPAddr *address, double new_max_delay)
{
  int slot, found;
  NTP_Remote_Address addr;
  addr.ip_addr = *address;
  addr.port = 0;

  find_slot(&addr, &slot, &found);
  if (found == 0) {
    return 0;
  } else {
    NCR_ModifyMaxdelay(get_record(slot)->data, new_max_delay);
    return 1;
  }
}

/* ================================================== */

int
NSR_ModifyMaxdelayratio(IPAddr *address, double new_max_delay_ratio)
{
  int slot, found;
  NTP_Remote_Address addr;
  addr.ip_addr = *address;
  addr.port = 0;

  find_slot(&addr, &slot, &found);
  if (found == 0) {
    return 0;
  } else {
    NCR_ModifyMaxdelayratio(get_record(slot)->data, new_max_delay_ratio);
    return 1;
  }
}

/* ================================================== */

int
NSR_ModifyMaxdelaydevratio(IPAddr *address, double new_max_delay_dev_ratio)
{
  int slot, found;
  NTP_Remote_Address addr;
  addr.ip_addr = *address;
  addr.port = 0;

  find_slot(&addr, &slot, &found);
  if (found == 0) {
    return 0;
  } else {
    NCR_ModifyMaxdelaydevratio(get_record(slot)->data, new_max_delay_dev_ratio);
    return 1;
  }
}

/* ================================================== */

int
NSR_ModifyMinstratum(IPAddr *address, int new_min_stratum)
{
  int slot, found;
  NTP_Remote_Address addr;
  addr.ip_addr = *address;
  addr.port = 0;

  find_slot(&addr, &slot, &found);
  if (found == 0) {
    return 0;
  } else {
    NCR_ModifyMinstratum(get_record(slot)->data, new_min_stratum);
    return 1;
  }
}

/* ================================================== */

int
NSR_ModifyPolltarget(IPAddr *address, int new_poll_target)
{
  int slot, found;
  NTP_Remote_Address addr;
  addr.ip_addr = *address;
  addr.port = 0;

  find_slot(&addr, &slot, &found);
  if (found == 0) {
    return 0;
  } else {
    NCR_ModifyPolltarget(get_record(slot)->data, new_poll_target);
    return 1;
  }
}

/* ================================================== */

int
NSR_InitiateSampleBurst(int n_good_samples, int n_total_samples,
                        IPAddr *mask, IPAddr *address)
{
  SourceRecord *record;
  unsigned int i;
  int any;

  any = 0;
  for (i = 0; i < ARR_GetSize(records); i++) {
    record = get_record(i);
    if (record->remote_addr) {
      if (address->family == IPADDR_UNSPEC ||
          !UTI_CompareIPs(&record->remote_addr->ip_addr, address, mask)) {
        any = 1;
        NCR_InitiateSampleBurst(record->data, n_good_samples, n_total_samples);
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
    NCR_ReportSource(get_record(slot)->data, report, now);
  } else {
    report->poll = 0;
    report->latest_meas_ago = 0;
  }
}

/* ================================================== */

void
NSR_GetActivityReport(RPT_ActivityReport *report)
{
  SourceRecord *record;
  unsigned int i;
  struct UnresolvedSource *us;

  report->online = 0;
  report->offline = 0;
  report->burst_online = 0;
  report->burst_offline = 0;

  for (i = 0; i < ARR_GetSize(records); i++) {
    record = get_record(i);
    if (record->remote_addr) {
      NCR_IncrementActivityCounters(record->data, &report->online, &report->offline,
                                    &report->burst_online, &report->burst_offline);
    }
  }

  report->unresolved = 0;

  for (us = unresolved_sources; us; us = us->next) {
    report->unresolved++;
  }
}


/* ================================================== */

