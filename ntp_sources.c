/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2003
 * Copyright (C) Miroslav Lichvar  2011-2012, 2014, 2016
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
#include "ntp_io.h"
#include "util.h"
#include "logging.h"
#include "local.h"
#include "memory.h"
#include "nameserv_async.h"
#include "privops.h"
#include "sched.h"

/* ================================================== */

/* Record type private to this file, used to store information about
   particular sources */
typedef struct {
  NTP_Remote_Address *remote_addr; /* The address of this source, non-NULL
                                      means this slot in table is in use
                                      (an IPADDR_ID address means the address
                                      is not resolved yet) */
  NCR_Instance data;            /* Data for the protocol engine for this source */
  char *name;                   /* Name of the source, may be NULL */
  int pool;                     /* Number of the pool from which was this source
                                   added or INVALID_POOL */
  int tentative;                /* Flag indicating there was no valid response
                                   received from the source yet */
} SourceRecord;

/* Hash table of SourceRecord, its size is a power of two and it's never
   more than half full */
static ARR_Instance records;

/* Number of sources in the hash table */
static int n_sources;

/* Flag indicating new sources will be started automatically when added */
static int auto_start_sources = 0;

/* Last assigned address ID */
static uint32_t last_address_id = 0;

/* Source scheduled for name resolving (first resolving or replacement) */
struct UnresolvedSource {
  /* Current address of the source (IDADDR_ID is used for a single source
     with unknown address and IPADDR_UNSPEC for a pool of sources */
  NTP_Remote_Address address;
  /* ID of the pool if not a single source */
  int pool;
  /* Name to be resolved */
  char *name;
  /* Flag indicating addresses should be used in a random order */
  int random_order;
  /* Next unresolved source in the list */
  struct UnresolvedSource *next;
};

#define RESOLVE_INTERVAL_UNIT 7
#define MIN_RESOLVE_INTERVAL 2
#define MAX_RESOLVE_INTERVAL 9
#define MIN_REPLACEMENT_INTERVAL 8

static struct UnresolvedSource *unresolved_sources = NULL;
static int resolving_interval = 0;
static SCH_TimeoutID resolving_id;
static struct UnresolvedSource *resolving_source = NULL;
static NSR_SourceResolvingEndHandler resolving_end_handler = NULL;

#define MAX_POOL_SOURCES 16
#define INVALID_POOL (-1)

/* Pool of sources with the same name */
struct SourcePool {
  /* Number of all sources from the pool */
  int sources;
  /* Number of sources with unresolved address */
  int unresolved_sources;
  /* Number of non-tentative sources */
  int confirmed_sources;
  /* Maximum number of confirmed sources */
  int max_sources;
};

/* Array of SourcePool */
static ARR_Instance pools;

/* ================================================== */
/* Forward prototypes */

static void resolve_sources(void);
static void rehash_records(void);
static void clean_source_record(SourceRecord *record);
static void remove_pool_sources(int pool, int tentative, int unresolved);
static void remove_unresolved_source(struct UnresolvedSource *us);

static void
slew_sources(struct timespec *raw,
             struct timespec *cooked,
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

static struct SourcePool *
get_pool(unsigned index)
{
  return (struct SourcePool *)ARR_GetElement(pools, index);
}

/* ================================================== */

void
NSR_Initialise(void)
{
  n_sources = 0;
  initialised = 1;

  records = ARR_CreateInstance(sizeof (SourceRecord));
  rehash_records();

  pools = ARR_CreateInstance(sizeof (struct SourcePool));

  LCL_AddParameterChangeHandler(slew_sources, NULL);
}

/* ================================================== */

void
NSR_Finalise(void)
{
  SourceRecord *record;
  unsigned int i;

  for (i = 0; i < ARR_GetSize(records); i++) {
    record = get_record(i);
    if (record->remote_addr)
      clean_source_record(record);
  }

  ARR_DestroyInstance(records);
  ARR_DestroyInstance(pools);

  while (unresolved_sources)
    remove_unresolved_source(unresolved_sources);

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

  size = ARR_GetSize(records);

  *slot = 0;
  *found = 0;
  
  if (remote_addr->ip_addr.family != IPADDR_INET4 &&
      remote_addr->ip_addr.family != IPADDR_INET6 &&
      remote_addr->ip_addr.family != IPADDR_ID)
    return;

  hash = UTI_IPToHash(&remote_addr->ip_addr);
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
}

/* ================================================== */
/* Check if hash table of given size is sufficient to contain sources */

static int
check_hashtable_size(unsigned int sources, unsigned int size)
{
  return sources * 2 <= size;
}

/* ================================================== */

static void
rehash_records(void)
{
  SourceRecord *temp_records;
  unsigned int i, old_size, new_size;
  int slot, found;

  old_size = ARR_GetSize(records);

  temp_records = MallocArray(SourceRecord, old_size);
  memcpy(temp_records, ARR_GetElements(records), old_size * sizeof (SourceRecord));

  /* The size of the hash table is always a power of two */
  for (new_size = 1; !check_hashtable_size(n_sources, new_size); new_size *= 2)
    ;

  ARR_SetSize(records, new_size);

  for (i = 0; i < new_size; i++)
    get_record(i)->remote_addr = NULL;

  for (i = 0; i < old_size; i++) {
    if (!temp_records[i].remote_addr)
      continue;

    find_slot(temp_records[i].remote_addr, &slot, &found);
    assert(!found);

    *get_record(slot) = temp_records[i];
  }

  Free(temp_records);
}

/* ================================================== */

/* Procedure to add a new source */
static NSR_Status
add_source(NTP_Remote_Address *remote_addr, char *name, NTP_Source_Type type, SourceParameters *params, int pool)
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
        remote_addr->ip_addr.family != IPADDR_INET6 &&
        remote_addr->ip_addr.family != IPADDR_ID) {
      return NSR_InvalidAF;
    } else {
      n_sources++;

      if (!check_hashtable_size(n_sources, ARR_GetSize(records))) {
        rehash_records();
        find_slot(remote_addr, &slot, &found);
      }

      assert(!found);
      record = get_record(slot);
      record->data = NCR_CreateInstance(remote_addr, type, params, name);
      record->remote_addr = NCR_GetRemoteAddress(record->data);
      record->name = name ? Strdup(name) : NULL;
      record->pool = pool;
      record->tentative = 1;

      if (record->pool != INVALID_POOL) {
        get_pool(record->pool)->sources++;
        if (!UTI_IsIPReal(&remote_addr->ip_addr))
          get_pool(record->pool)->unresolved_sources++;
      }

      if (auto_start_sources && UTI_IsIPReal(&remote_addr->ip_addr))
        NCR_StartInstance(record->data);

      return NSR_Success;
    }
  }
}

/* ================================================== */

static NSR_Status
change_source_address(NTP_Remote_Address *old_addr, NTP_Remote_Address *new_addr,
                      int replacement)
{
  int slot1, slot2, found;
  SourceRecord *record;
  LOG_Severity severity;
  char *name;

  find_slot(old_addr, &slot1, &found);
  if (!found)
    return NSR_NoSuchSource;

  /* Make sure there is no other source using the new address (with the same
     or different port), but allow a source to have its port changed */
  find_slot(new_addr, &slot2, &found);
  if (found == 2 || (found != 0 && slot1 != slot2))
    return NSR_AlreadyInUse;

  record = get_record(slot1);
  NCR_ChangeRemoteAddress(record->data, new_addr, !replacement);
  record->remote_addr = NCR_GetRemoteAddress(record->data);
  if (!UTI_IsIPReal(&old_addr->ip_addr) && UTI_IsIPReal(&new_addr->ip_addr)) {
    if (auto_start_sources)
      NCR_StartInstance(record->data);
    if (record->pool != INVALID_POOL)
      get_pool(record->pool)->unresolved_sources--;
  }

  if (!record->tentative) {
    record->tentative = 1;

    if (record->pool != INVALID_POOL)
      get_pool(record->pool)->confirmed_sources--;
  }

  name = record->name;
  severity = UTI_IsIPReal(&old_addr->ip_addr) ? LOGS_INFO : LOGS_DEBUG;

  if (found == 0) {
    /* The hash table must be rebuilt for the changed address */
    rehash_records();

    LOG(severity, "Source %s %s %s (%s)", UTI_IPToString(&old_addr->ip_addr),
        replacement ? "replaced with" : "changed to",
        UTI_IPToString(&new_addr->ip_addr), name ? name : "");
  } else {
    LOG(severity, "Source %s (%s) changed port to %d",
        UTI_IPToString(&new_addr->ip_addr), name ? name : "", new_addr->port);
  }

  return NSR_Success;
}

/* ================================================== */

static int
replace_source_connectable(NTP_Remote_Address *old_addr, NTP_Remote_Address *new_addr)
{
  if (!NIO_IsServerConnectable(new_addr)) {
    DEBUG_LOG("%s not connectable", UTI_IPToString(&new_addr->ip_addr));
    return 0;
  }

  if (change_source_address(old_addr, new_addr, 1) == NSR_AlreadyInUse)
    return 0;

  return 1;
}

/* ================================================== */

static void
process_resolved_name(struct UnresolvedSource *us, IPAddr *ip_addrs, int n_addrs)
{
  NTP_Remote_Address old_addr, new_addr;
  SourceRecord *record;
  unsigned short first = 0;
  int i, j;

  if (us->random_order)
    UTI_GetRandomBytes(&first, sizeof (first));

  for (i = 0; i < n_addrs; i++) {
    new_addr.ip_addr = ip_addrs[((unsigned int)i + first) % n_addrs];

    DEBUG_LOG("(%d) %s", i + 1, UTI_IPToString(&new_addr.ip_addr));

    if (us->pool != INVALID_POOL) {
      /* In the pool resolving mode, try to replace all sources from
         the pool which don't have a real address yet */
      for (j = 0; j < ARR_GetSize(records); j++) {
        record = get_record(j);
        if (!record->remote_addr || record->pool != us->pool ||
            UTI_IsIPReal(&record->remote_addr->ip_addr))
          continue;
        old_addr = *record->remote_addr;
        new_addr.port = old_addr.port;
        if (replace_source_connectable(&old_addr, &new_addr))
          break;
      }
    } else {
      new_addr.port = us->address.port;
      if (replace_source_connectable(&us->address, &new_addr))
        break;
    }
  }
}

/* ================================================== */

static int
is_resolved(struct UnresolvedSource *us)
{
  int slot, found;

  if (us->pool != INVALID_POOL) {
    return get_pool(us->pool)->unresolved_sources <= 0;
  } else {
    /* If the address is no longer present, it was removed or replaced
       (i.e. resolved) */
    find_slot(&us->address, &slot, &found);
    return !found;
  }
}

/* ================================================== */

static void
resolve_sources_timeout(void *arg)
{
  resolving_id = 0;
  resolve_sources();
}

/* ================================================== */

static void
name_resolve_handler(DNS_Status status, int n_addrs, IPAddr *ip_addrs, void *anything)
{
  struct UnresolvedSource *us, *next;

  us = (struct UnresolvedSource *)anything;

  assert(us == resolving_source);
  assert(resolving_id == 0);

  DEBUG_LOG("%s resolved to %d addrs", us->name, n_addrs);

  switch (status) {
    case DNS_TryAgain:
      break;
    case DNS_Success:
      process_resolved_name(us, ip_addrs, n_addrs);
      break;
    case DNS_Failure:
      LOG(LOGS_WARN, "Invalid host %s", us->name);
      break;
    default:
      assert(0);
  }

  next = us->next;

  /* Don't repeat the resolving if it (permanently) failed, it was a
     replacement of a real address, or all addresses are already resolved */
  if (status == DNS_Failure || UTI_IsIPReal(&us->address.ip_addr) || is_resolved(us))
    remove_unresolved_source(us);

  resolving_source = next;

  if (next) {
    /* Continue with the next source in the list */
    DEBUG_LOG("resolving %s", next->name);
    DNS_Name2IPAddressAsync(next->name, name_resolve_handler, next);
  } else {
    /* This was the last source in the list. If some sources couldn't
       be resolved, try again in exponentially increasing interval. */
    if (unresolved_sources) {
      resolving_interval = CLAMP(MIN_RESOLVE_INTERVAL, resolving_interval + 1,
                                 MAX_RESOLVE_INTERVAL);
      resolving_id = SCH_AddTimeoutByDelay(RESOLVE_INTERVAL_UNIT * (1 << resolving_interval),
                                           resolve_sources_timeout, NULL);
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
resolve_sources(void)
{
  struct UnresolvedSource *us, *next, *i;

  assert(!resolving_source);

  /* Remove sources that don't need to be resolved anymore */
  for (i = unresolved_sources; i; i = next) {
    next = i->next;
    if (is_resolved(i))
      remove_unresolved_source(i);
  }

  if (!unresolved_sources)
    return;

  PRV_ReloadDNS();

  /* Start with the first source in the list, name_resolve_handler
     will iterate over the rest */
  us = unresolved_sources;

  resolving_source = us;
  DEBUG_LOG("resolving %s", us->name);
  DNS_Name2IPAddressAsync(us->name, name_resolve_handler, us);
}

/* ================================================== */

static void
append_unresolved_source(struct UnresolvedSource *us)
{
  struct UnresolvedSource **i;

  for (i = &unresolved_sources; *i; i = &(*i)->next)
    ;
  *i = us;
  us->next = NULL;
}

/* ================================================== */

static void
remove_unresolved_source(struct UnresolvedSource *us)
{
  struct UnresolvedSource **i;

  for (i = &unresolved_sources; *i; i = &(*i)->next) {
    if (*i == us) {
      *i = us->next;
      Free(us->name);
      Free(us);
      break;
    }
  }
}

/* ================================================== */

NSR_Status
NSR_AddSource(NTP_Remote_Address *remote_addr, NTP_Source_Type type, SourceParameters *params)
{
  return add_source(remote_addr, NULL, type, params, INVALID_POOL);
}

/* ================================================== */

NSR_Status
NSR_AddSourceByName(char *name, int port, int pool, NTP_Source_Type type, SourceParameters *params)
{
  struct UnresolvedSource *us;
  struct SourcePool *sp;
  NTP_Remote_Address remote_addr;
  int i, new_sources;

  /* If the name is an IP address, don't bother with full resolving now
     or later when trying to replace the source */
  if (UTI_StringToIP(name, &remote_addr.ip_addr)) {
    remote_addr.port = port;
    return NSR_AddSource(&remote_addr, type, params);
  }

  /* Make sure the name is at least printable and has no spaces */
  for (i = 0; name[i] != '\0'; i++) {
    if (!isgraph(name[i]))
      return NSR_InvalidName;
  }

  us = MallocNew(struct UnresolvedSource);
  us->name = Strdup(name);
  us->random_order = 0;

  remote_addr.ip_addr.family = IPADDR_ID;
  remote_addr.ip_addr.addr.id = ++last_address_id;
  remote_addr.port = port;

  if (!pool) {
    us->pool = INVALID_POOL;
    us->address = remote_addr;
    new_sources = 1;
  } else {
    sp = (struct SourcePool *)ARR_GetNewElement(pools);
    sp->sources = 0;
    sp->unresolved_sources = 0;
    sp->confirmed_sources = 0;
    sp->max_sources = CLAMP(1, params->max_sources, MAX_POOL_SOURCES);
    us->pool = ARR_GetSize(pools) - 1;
    us->address.ip_addr.family = IPADDR_UNSPEC;
    new_sources = MIN(2 * sp->max_sources, MAX_POOL_SOURCES);
  }

  append_unresolved_source(us);

  for (i = 0; i < new_sources; i++) {
    if (i > 0)
      remote_addr.ip_addr.addr.id = ++last_address_id;
    if (add_source(&remote_addr, name, type, params, us->pool) != NSR_Success)
      return NSR_TooManySources;
  }

  return NSR_UnresolvedName;
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
      if (resolving_id != 0) {
        SCH_RemoveTimeout(resolving_id);
        resolving_id = 0;
        resolving_interval--;
      }
      resolve_sources();
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
  NTP_Remote_Address *addr;
  unsigned int i;

  for (i = 0; i < ARR_GetSize(records); i++) {
    addr = get_record(i)->remote_addr;
    if (!addr || !UTI_IsIPReal(&addr->ip_addr))
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

static void
clean_source_record(SourceRecord *record)
{
  assert(record->remote_addr);

  if (record->pool != INVALID_POOL) {
    struct SourcePool *pool = get_pool(record->pool);

    pool->sources--;
    if (!UTI_IsIPReal(&record->remote_addr->ip_addr))
      pool->unresolved_sources--;
    if (!record->tentative)
      pool->confirmed_sources--;
    if (pool->max_sources > pool->sources)
      pool->max_sources = pool->sources;
  }

  record->remote_addr = NULL;
  NCR_DestroyInstance(record->data);
  if (record->name)
    Free(record->name);

  n_sources--;
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

  clean_source_record(get_record(slot));

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
    clean_source_record(record);
  }

  rehash_records();
}

/* ================================================== */

static void
resolve_source_replacement(SourceRecord *record)
{
  struct UnresolvedSource *us;

  DEBUG_LOG("trying to replace %s", UTI_IPToString(&record->remote_addr->ip_addr));

  us = MallocNew(struct UnresolvedSource);
  us->name = Strdup(record->name);
  /* If there never was a valid reply from this source (e.g. it was a bad
     replacement), ignore the order of addresses from the resolver to not get
     stuck to a pair of addresses if the order doesn't change, or a group of
     IPv4/IPv6 addresses if the resolver prefers inaccessible IP family */
  us->random_order = record->tentative;
  us->pool = INVALID_POOL;
  us->address = *record->remote_addr;

  append_unresolved_source(us);
  NSR_ResolveSources();
}

/* ================================================== */

void
NSR_HandleBadSource(IPAddr *address)
{
  static struct timespec last_replacement;
  struct timespec now;
  NTP_Remote_Address remote_addr;
  SourceRecord *record;
  int slot, found;
  double diff;

  remote_addr.ip_addr = *address;
  remote_addr.port = 0;

  find_slot(&remote_addr, &slot, &found);
  if (!found)
    return;

  record = get_record(slot);

  /* Only sources with a name can be replaced */
  if (!record->name)
    return;

  /* Don't resolve names too frequently */
  SCH_GetLastEventTime(NULL, NULL, &now);
  diff = UTI_DiffTimespecsToDouble(&now, &last_replacement);
  if (fabs(diff) < RESOLVE_INTERVAL_UNIT * (1 << MIN_REPLACEMENT_INTERVAL)) {
    DEBUG_LOG("replacement postponed");
    return;
  }
  last_replacement = now;

  resolve_source_replacement(record);
}

/* ================================================== */

void
NSR_RefreshAddresses(void)
{
  SourceRecord *record;
  unsigned int i;

  for (i = 0; i < ARR_GetSize(records); i++) {
    record = get_record(i);
    if (!record->remote_addr || !record->name)
      continue;

    resolve_source_replacement(record);
  }
}

/* ================================================== */

NSR_Status
NSR_UpdateSourceNtpAddress(NTP_Remote_Address *old_addr, NTP_Remote_Address *new_addr)
{
  if (new_addr->ip_addr.family == IPADDR_UNSPEC)
    return NSR_InvalidAF;

  return change_source_address(old_addr, new_addr, 0);
}

/* ================================================== */

static void remove_pool_sources(int pool, int tentative, int unresolved)
{
  SourceRecord *record;
  unsigned int i, removed;

  for (i = removed = 0; i < ARR_GetSize(records); i++) {
    record = get_record(i);

    if (!record->remote_addr || record->pool != pool)
      continue;

    if ((tentative && !record->tentative) ||
        (unresolved && UTI_IsIPReal(&record->remote_addr->ip_addr)))
      continue;

    DEBUG_LOG("removing %ssource %s", tentative ? "tentative " : "",
              UTI_IPToString(&record->remote_addr->ip_addr));

    clean_source_record(record);
    removed++;
  }

  if (removed)
    rehash_records();
}

/* ================================================== */

uint32_t
NSR_GetLocalRefid(IPAddr *address)
{
  NTP_Remote_Address remote_addr;
  int slot, found;

  remote_addr.ip_addr = *address;
  remote_addr.port = 0;

  find_slot(&remote_addr, &slot, &found);
  if (!found)
    return 0;

  return NCR_GetLocalRefid(get_record(slot)->data);
}

/* ================================================== */

char *
NSR_GetName(IPAddr *address)
{
  NTP_Remote_Address remote_addr;
  int slot, found;
  SourceRecord *record;

  remote_addr.ip_addr = *address;
  remote_addr.port = 0;

  find_slot(&remote_addr, &slot, &found);
  if (!found)
    return NULL;

  record = get_record(slot);
  if (record->name)
    return record->name;

  return UTI_IPToString(&record->remote_addr->ip_addr); 
}

/* ================================================== */

/* This routine is called by ntp_io when a new packet arrives off the network,
   possibly with an authentication tail */
void
NSR_ProcessRx(NTP_Remote_Address *remote_addr, NTP_Local_Address *local_addr,
              NTP_Local_Timestamp *rx_ts, NTP_Packet *message, int length)
{
  SourceRecord *record;
  struct SourcePool *pool;
  int slot, found;

  assert(initialised);

  find_slot(remote_addr, &slot, &found);
  if (found == 2) { /* Must match IP address AND port number */
    record = get_record(slot);

    if (!NCR_ProcessRxKnown(record->data, local_addr, rx_ts, message, length))
      return;

    if (record->tentative) {
      /* This was the first good reply from the source */
      record->tentative = 0;

      if (record->pool != INVALID_POOL) {
        pool = get_pool(record->pool);
        pool->confirmed_sources++;

        DEBUG_LOG("pool %s has %d confirmed sources", record->name, pool->confirmed_sources);

        /* If the number of sources from the pool reached the configured
           maximum, remove the remaining tentative sources */
        if (pool->confirmed_sources >= pool->max_sources)
          remove_pool_sources(record->pool, 1, 0);
      }
    }
  } else {
    NCR_ProcessRxUnknown(remote_addr, local_addr, rx_ts, message, length);
  }
}

/* ================================================== */

void
NSR_ProcessTx(NTP_Remote_Address *remote_addr, NTP_Local_Address *local_addr,
              NTP_Local_Timestamp *tx_ts, NTP_Packet *message, int length)
{
  SourceRecord *record;
  int slot, found;

  find_slot(remote_addr, &slot, &found);

  if (found == 2) { /* Must match IP address AND port number */
    record = get_record(slot);
    NCR_ProcessTxKnown(record->data, local_addr, tx_ts, message, length);
  } else {
    NCR_ProcessTxUnknown(remote_addr, local_addr, tx_ts, message, length);
  }
}

/* ================================================== */

static void
slew_sources(struct timespec *raw,
             struct timespec *cooked,
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
        NCR_ResetPoll(record->data);
      } else {
        NCR_SlewTimes(record->data, cooked, dfreq, doffset);
      }
    }
  }
}

/* ================================================== */

int
NSR_SetConnectivity(IPAddr *mask, IPAddr *address, SRC_Connectivity connectivity)
{
  SourceRecord *record, *syncpeer;
  unsigned int i, any;

  if (connectivity != SRC_OFFLINE)
    NSR_ResolveSources();

  any = 0;
  syncpeer = NULL;
  for (i = 0; i < ARR_GetSize(records); i++) {
    record = get_record(i);
    if (record->remote_addr) {
      /* Ignore SRC_MAYBE_ONLINE connectivity change for unspecified unresolved
         sources as they would always end up in the offline state */
      if ((address->family == IPADDR_UNSPEC &&
           (connectivity != SRC_MAYBE_ONLINE || UTI_IsIPReal(&record->remote_addr->ip_addr))) ||
          !UTI_CompareIPs(&record->remote_addr->ip_addr, address, mask)) {
        any = 1;
        if (NCR_IsSyncPeer(record->data)) {
          syncpeer = record;
          continue;
        }
        NCR_SetConnectivity(record->data, connectivity);
      }
    }
  }

  /* Set the sync peer last to avoid unnecessary reference switching */
  if (syncpeer)
    NCR_SetConnectivity(syncpeer->data, connectivity);

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
NSR_ReportSource(RPT_SourceReport *report, struct timespec *now)
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

int
NSR_GetAuthReport(IPAddr *address, RPT_AuthReport *report)
{
  NTP_Remote_Address rem_addr;
  int slot, found;

  rem_addr.ip_addr = *address;
  rem_addr.port = 0;
  find_slot(&rem_addr, &slot, &found);
  if (!found)
    return 0;

  NCR_GetAuthReport(get_record(slot)->data, report);
  return 1;
}

/* ================================================== */
/* The ip address is assumed to be completed on input, that is how we
   identify the source record. */

int
NSR_GetNTPReport(RPT_NTPReport *report)
{
  NTP_Remote_Address rem_addr;
  int slot, found;

  rem_addr.ip_addr = report->remote_addr;
  rem_addr.port = 0;
  find_slot(&rem_addr, &slot, &found);
  if (!found)
    return 0;

  NCR_GetNTPReport(get_record(slot)->data, report);
  return 1;
}

/* ================================================== */

void
NSR_GetActivityReport(RPT_ActivityReport *report)
{
  SourceRecord *record;
  unsigned int i;

  report->online = 0;
  report->offline = 0;
  report->burst_online = 0;
  report->burst_offline = 0;
  report->unresolved = 0;

  for (i = 0; i < ARR_GetSize(records); i++) {
    record = get_record(i);
    if (!record->remote_addr)
      continue;

    if (!UTI_IsIPReal(&record->remote_addr->ip_addr)) {
      report->unresolved++;
    } else {
      NCR_IncrementActivityCounters(record->data, &report->online, &report->offline,
                                    &report->burst_online, &report->burst_offline);
    }
  }
}

/* ================================================== */

void
NSR_DumpAuthData(void)
{
  SourceRecord *record;
  int i;

  for (i = 0; i < ARR_GetSize(records); i++) {
    record = get_record(i);
    if (!record->remote_addr)
      continue;
    NCR_DumpAuthData(record->data);
  }
}
