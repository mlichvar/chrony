/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2003
 * Copyright (C) Miroslav Lichvar  2009-2016
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

  Core NTP protocol engine
  */

#include "config.h"

#include "sysincl.h"

#include "array.h"
#include "ntp_core.h"
#include "ntp_io.h"
#include "ntp_signd.h"
#include "memory.h"
#include "sched.h"
#include "reference.h"
#include "local.h"
#include "smooth.h"
#include "sources.h"
#include "util.h"
#include "conf.h"
#include "logging.h"
#include "keys.h"
#include "addrfilt.h"
#include "clientlog.h"

/* ================================================== */

static LOG_FileID logfileid;

/* ================================================== */
/* Enumeration used for remembering the operating mode of one of the
   sources */

typedef enum {
  MD_OFFLINE,                   /* No sampling at all */
  MD_ONLINE,                    /* Normal sampling based on sampling interval */
  MD_BURST_WAS_OFFLINE,         /* Burst sampling, return to offline afterwards */
  MD_BURST_WAS_ONLINE,          /* Burst sampling, return to online afterwards */
} OperatingMode;

/* ================================================== */
/* Enumeration for authentication modes of NTP packets */

typedef enum {
  AUTH_NONE = 0,                /* No authentication */
  AUTH_SYMMETRIC,               /* MAC using symmetric key (RFC 1305, RFC 5905) */
  AUTH_MSSNTP,                  /* MS-SNTP authenticator field */
  AUTH_MSSNTP_EXT,              /* MS-SNTP extended authenticator field */
} AuthenticationMode;

/* ================================================== */
/* Structure used for holding a single peer/server's
   protocol machine */

struct NCR_Instance_Record {
  NTP_Remote_Address remote_addr; /* Needed for routing transmit packets */
  NTP_Local_Address local_addr; /* Local address/socket used to send packets */
  NTP_Mode mode;                /* The source's NTP mode
                                   (client/server or symmetric active peer) */
  int interleaved;              /* Boolean enabling interleaved NTP mode */
  OperatingMode opmode;         /* Whether we are sampling this source
                                   or not and in what way */
  SCH_TimeoutID rx_timeout_id;  /* Timeout ID for latest received response */
  SCH_TimeoutID tx_timeout_id;  /* Timeout ID for next transmission */
  int tx_suspended;             /* Boolean indicating we can't transmit yet */

  int auto_offline;             /* If 1, automatically go offline if server/peer
                                   isn't responding */

  int local_poll;               /* Log2 of polling interval at our end */
  int remote_poll;              /* Log2 of server/peer's polling interval (recovered
                                   from received packets) */
  int remote_stratum;           /* Stratum of the server/peer (recovered from
                                   received packets) */

  int presend_minpoll;           /* If the current polling interval is
                                    at least this, an extra client packet
                                    will be send some time before normal
                                    transmit.  This ensures that both
                                    us and the server/peer have an ARP
                                    entry for each other ready, which
                                    means our measurement is not
                                    botched by an ARP round-trip on one
                                    side or the other. */

  int presend_done;             /* The presend packet has been sent */

  int minpoll;                  /* Log2 of minimum defined polling interval */
  int maxpoll;                  /* Log2 of maximum defined polling interval */

  int min_stratum;              /* Increase stratum in received packets to the
                                   minimum */

  int poll_target;              /* Target number of sourcestats samples */

  int version;                  /* Version set in packets for server/peer */

  double poll_score;            /* Score of current local poll */

  double max_delay;             /* Maximum round-trip delay to the
                                   peer that we can tolerate and still
                                   use the sample for generating
                                   statistics from */

  double max_delay_ratio;       /* Largest ratio of delay /
                                   min_delay_in_register that we can
                                   tolerate.  */

  double max_delay_dev_ratio;   /* Maximum ratio of increase in delay / stddev */

  double offset_correction;     /* Correction applied to measured offset
                                   (e.g. for asymmetry in network delay) */

  AuthenticationMode auth_mode; /* Authentication mode of our requests */
  uint32_t auth_key_id;          /* The ID of the authentication key to
                                   use. */

  /* Count of how many packets we have transmitted since last successful
     receive from this peer */
  int tx_count;

  /* Flag indicating a valid response was received since last request */
  int valid_rx;

  /* Flag indicating the timestamps below are from a valid packet and may
     be used for synchronisation */
  int valid_timestamps;

  /* Flag indicating the timestamps below were updated since last request */
  int updated_timestamps;

  /* Receive and transmit timestamps from the last received packet */
  NTP_int64 remote_ntp_rx;
  NTP_int64 remote_ntp_tx;

  /* Local timestamp when the last packet was received from the
     source.  We have to be prepared to tinker with this if the local
     clock has its frequency adjusted before we repond.  The value we
     store here is what our own local time was when the same arrived.
     Before replying, we have to correct this to fit with the
     parameters for the current reference.  (It must be stored
     relative to local time to permit frequency and offset adjustments
     to be made when we trim the local clock). */
  NTP_int64 local_ntp_rx;
  NTP_Local_Timestamp local_rx;

  /* Local timestamp when we last transmitted a packet to the source.
     We store two versions.  The first is in NTP format, and is used
     to validate the next received packet from the source.
     Additionally, this is corrected to bring it into line with the
     current reference.  The second is in timespec format, and is kept
     relative to the local clock.  We modify this in accordance with
     local clock frequency/offset changes, and use this for computing
     statistics about the source when a return packet arrives. */
  NTP_int64 local_ntp_tx;
  NTP_Local_Timestamp local_tx;

  /* The instance record in the main source management module.  This
     performs the statistical analysis on the samples we generate */

  SRC_Instance source;

  int burst_good_samples_to_go;
  int burst_total_samples_to_go;

};

typedef struct {
  NTP_Remote_Address addr;
  NTP_Local_Address local_addr;
  int interval;
} BroadcastDestination;

/* Array of BroadcastDestination */
static ARR_Instance broadcasts;

/* ================================================== */
/* Initial delay period before first packet is transmitted (in seconds) */
#define INITIAL_DELAY 0.2

/* Spacing required between samples for any two servers/peers (to
   minimise risk of network collisions) (in seconds) */
#define SAMPLING_SEPARATION 0.2

/* Randomness added to spacing between samples for one server/peer */
#define SAMPLING_RANDOMNESS 0.02

/* Adjustment of the peer polling interval */
#define PEER_SAMPLING_ADJ 1.1

/* Spacing between samples in burst mode for one server/peer */
#define BURST_INTERVAL 2.0

/* Time to wait before retransmitting in burst mode, if we did not get
   a reply to the previous probe */
#define BURST_TIMEOUT 2.0

/* Number of samples in initial burst */
#define IBURST_GOOD_SAMPLES 4
#define IBURST_TOTAL_SAMPLES SOURCE_REACH_BITS

/* Time to wait after sending packet to 'warm up' link */
#define WARM_UP_DELAY 4.0

/* Compatible NTP protocol versions */
#define NTP_MAX_COMPAT_VERSION NTP_VERSION
#define NTP_MIN_COMPAT_VERSION 1

/* Maximum allowed dispersion - as defined in RFC 5905 (16 seconds) */
#define NTP_MAX_DISPERSION 16.0

/* Invalid stratum number */
#define NTP_INVALID_STRATUM 0

/* Maximum allowed time for server to process client packet */
#define MAX_SERVER_INTERVAL 4.0

/* Maximum acceptable delay in transmission for timestamp correction */
#define MAX_TX_DELAY 1.0

/* Minimum and maximum allowed poll interval */
#define MIN_POLL 0
#define MAX_POLL 24

/* Kiss-o'-Death codes */
#define KOD_RATE 0x52415445UL /* RATE */

/* Maximum poll interval set by KoD RATE */
#define MAX_KOD_RATE_POLL SRC_DEFAULT_MAXPOLL

/* Invalid socket, different from the one in ntp_io.c */
#define INVALID_SOCK_FD -2

/* ================================================== */

/* Server IPv4/IPv6 sockets */
static int server_sock_fd4;
static int server_sock_fd6;

static ADF_AuthTable access_auth_table;

/* Characters for printing synchronisation status and timestamping source */
static const char leap_chars[4] = {'N', '+', '-', '?'};
static const char tss_chars[3] = {'D', 'K', 'H'};

/* ================================================== */
/* Forward prototypes */

static void transmit_timeout(void *arg);
static double get_transmit_delay(NCR_Instance inst, int on_tx, double last_tx);

/* ================================================== */

static void
do_size_checks(void)
{
  /* Assertions to check the sizes of certain data types
     and the positions of certain record fields */

  /* Check that certain invariants are true */
  assert(sizeof(NTP_int32) == 4);
  assert(sizeof(NTP_int64) == 8);

  /* Check offsets of all fields in the NTP packet format */
  assert(offsetof(NTP_Packet, lvm)             ==  0);
  assert(offsetof(NTP_Packet, stratum)         ==  1);
  assert(offsetof(NTP_Packet, poll)            ==  2);
  assert(offsetof(NTP_Packet, precision)       ==  3);
  assert(offsetof(NTP_Packet, root_delay)      ==  4);
  assert(offsetof(NTP_Packet, root_dispersion) ==  8);
  assert(offsetof(NTP_Packet, reference_id)    == 12);
  assert(offsetof(NTP_Packet, reference_ts)    == 16);
  assert(offsetof(NTP_Packet, originate_ts)    == 24);
  assert(offsetof(NTP_Packet, receive_ts)      == 32);
  assert(offsetof(NTP_Packet, transmit_ts)     == 40);
}

/* ================================================== */

static void
do_time_checks(void)
{
  struct timespec now;
  time_t warning_advance = 3600 * 24 * 365 * 10; /* 10 years */

#ifdef HAVE_LONG_TIME_T
  /* Check that time before NTP_ERA_SPLIT underflows correctly */

  struct timespec ts1 = {NTP_ERA_SPLIT, 1}, ts2 = {NTP_ERA_SPLIT - 1, 1};
  NTP_int64 nts1, nts2;
  int r;

  UTI_TimespecToNtp64(&ts1, &nts1, NULL);
  UTI_TimespecToNtp64(&ts2, &nts2, NULL);
  UTI_Ntp64ToTimespec(&nts1, &ts1);
  UTI_Ntp64ToTimespec(&nts2, &ts2);

  r = ts1.tv_sec == NTP_ERA_SPLIT &&
      ts1.tv_sec + (1ULL << 32) - 1 == ts2.tv_sec;

  assert(r);

  LCL_ReadRawTime(&now);
  if (ts2.tv_sec - now.tv_sec < warning_advance)
    LOG(LOGS_WARN, LOGF_NtpCore, "Assumed NTP time ends at %s!",
        UTI_TimeToLogForm(ts2.tv_sec));
#else
  LCL_ReadRawTime(&now);
  if (now.tv_sec > 0x7fffffff - warning_advance)
    LOG(LOGS_WARN, LOGF_NtpCore, "System time ends at %s!",
        UTI_TimeToLogForm(0x7fffffff));
#endif
}

/* ================================================== */

void
NCR_Initialise(void)
{
  do_size_checks();
  do_time_checks();

  logfileid = CNF_GetLogMeasurements() ? LOG_FileOpen("measurements",
      "   Date (UTC) Time     IP Address   L St 123 567 ABCD  LP RP Score    Offset  Peer del. Peer disp.  Root del. Root disp. Refid     MTxRx")
    : -1;

  access_auth_table = ADF_CreateTable();
  broadcasts = ARR_CreateInstance(sizeof (BroadcastDestination));

  /* Server socket will be opened when access is allowed */
  server_sock_fd4 = INVALID_SOCK_FD;
  server_sock_fd6 = INVALID_SOCK_FD;
}

/* ================================================== */

void
NCR_Finalise(void)
{
  unsigned int i;

  if (server_sock_fd4 != INVALID_SOCK_FD)
    NIO_CloseServerSocket(server_sock_fd4);
  if (server_sock_fd6 != INVALID_SOCK_FD)
    NIO_CloseServerSocket(server_sock_fd6);

  for (i = 0; i < ARR_GetSize(broadcasts); i++)
    NIO_CloseServerSocket(((BroadcastDestination *)ARR_GetElement(broadcasts, i))->local_addr.sock_fd);

  ARR_DestroyInstance(broadcasts);
  ADF_DestroyTable(access_auth_table);
}

/* ================================================== */

static void
restart_timeout(NCR_Instance inst, double delay)
{
  /* Check if we can transmit */
  if (inst->tx_suspended) {
    assert(!inst->tx_timeout_id);
    return;
  }

  /* Stop both rx and tx timers if running */
  SCH_RemoveTimeout(inst->rx_timeout_id);
  inst->rx_timeout_id = 0;
  SCH_RemoveTimeout(inst->tx_timeout_id);

  /* Start new timer for transmission */
  inst->tx_timeout_id = SCH_AddTimeoutInClass(delay, SAMPLING_SEPARATION,
                                              SAMPLING_RANDOMNESS,
                                              SCH_NtpSamplingClass,
                                              transmit_timeout, (void *)inst);
}

/* ================================================== */

static void
start_initial_timeout(NCR_Instance inst)
{
  double delay, last_tx;
  struct timespec now;

  if (!inst->tx_timeout_id) {
    /* This will be the first transmission after mode change */

    /* Mark source active */
    SRC_SetActive(inst->source);
  }

  /* In case the offline period was too short, adjust the delay to keep
     the interval between packets at least as long as the current polling
     interval */
  SCH_GetLastEventTime(&now, NULL, NULL);
  last_tx = UTI_DiffTimespecsToDouble(&now, &inst->local_tx.ts);
  if (last_tx < 0.0)
    last_tx = 0.0;
  delay = get_transmit_delay(inst, 0, 0.0) - last_tx;
  if (delay < INITIAL_DELAY)
    delay = INITIAL_DELAY;

  restart_timeout(inst, delay);
}

/* ================================================== */

static void
close_client_socket(NCR_Instance inst)
{
  if (inst->mode == MODE_CLIENT && inst->local_addr.sock_fd != INVALID_SOCK_FD) {
    NIO_CloseClientSocket(inst->local_addr.sock_fd);
    inst->local_addr.sock_fd = INVALID_SOCK_FD;
  }

  SCH_RemoveTimeout(inst->rx_timeout_id);
  inst->rx_timeout_id = 0;
}

/* ================================================== */

static void
take_offline(NCR_Instance inst)
{
  inst->opmode = MD_OFFLINE;

  SCH_RemoveTimeout(inst->tx_timeout_id);
  inst->tx_timeout_id = 0;

  /* Mark source unreachable */
  SRC_ResetReachability(inst->source);

  /* And inactive */
  SRC_UnsetActive(inst->source);

  close_client_socket(inst);

  NCR_ResetInstance(inst);
}

/* ================================================== */

NCR_Instance
NCR_GetInstance(NTP_Remote_Address *remote_addr, NTP_Source_Type type, SourceParameters *params)
{
  NCR_Instance result;

  result = MallocNew(struct NCR_Instance_Record);

  result->remote_addr = *remote_addr;
  result->local_addr.ip_addr.family = IPADDR_UNSPEC;

  switch (type) {
    case NTP_SERVER:
      /* Client socket will be obtained when sending request */
      result->local_addr.sock_fd = INVALID_SOCK_FD;
      result->mode = MODE_CLIENT;
      break;
    case NTP_PEER:
      result->local_addr.sock_fd = NIO_OpenServerSocket(remote_addr);
      result->mode = MODE_ACTIVE;
      break;
    default:
      assert(0);
  }

  result->interleaved = params->interleaved;

  result->minpoll = params->minpoll;
  if (result->minpoll < MIN_POLL)
    result->minpoll = SRC_DEFAULT_MINPOLL;
  else if (result->minpoll > MAX_POLL)
    result->minpoll = MAX_POLL;
  result->maxpoll = params->maxpoll;
  if (result->maxpoll < MIN_POLL)
    result->maxpoll = SRC_DEFAULT_MAXPOLL;
  else if (result->maxpoll > MAX_POLL)
    result->maxpoll = MAX_POLL;
  if (result->maxpoll < result->minpoll)
    result->maxpoll = result->minpoll;

  result->min_stratum = params->min_stratum;
  if (result->min_stratum >= NTP_MAX_STRATUM)
    result->min_stratum = NTP_MAX_STRATUM - 1;

  /* Presend doesn't work in symmetric mode */
  result->presend_minpoll = params->presend_minpoll;
  if (result->presend_minpoll && result->mode != MODE_CLIENT)
    result->presend_minpoll = 0;

  result->max_delay = params->max_delay;
  result->max_delay_ratio = params->max_delay_ratio;
  result->max_delay_dev_ratio = params->max_delay_dev_ratio;
  result->offset_correction = params->offset;
  result->auto_offline = params->auto_offline;
  result->poll_target = params->poll_target;

  result->version = params->version;
  if (result->version < NTP_MIN_COMPAT_VERSION)
    result->version = NTP_MIN_COMPAT_VERSION;
  else if (result->version > NTP_VERSION)
    result->version = NTP_VERSION;

  if (params->authkey == INACTIVE_AUTHKEY) {
    result->auth_mode = AUTH_NONE;
    result->auth_key_id = 0;
  } else {
    result->auth_mode = AUTH_SYMMETRIC;
    result->auth_key_id = params->authkey;
    if (!KEY_KeyKnown(result->auth_key_id)) {
      LOG(LOGS_WARN, LOGF_NtpCore, "Key %"PRIu32" used by source %s is %s",
          result->auth_key_id, UTI_IPToString(&result->remote_addr.ip_addr),
          "missing");
    } else if (!KEY_CheckKeyLength(result->auth_key_id)) {
      LOG(LOGS_WARN, LOGF_NtpCore, "Key %"PRIu32" used by source %s is %s",
          result->auth_key_id, UTI_IPToString(&result->remote_addr.ip_addr),
          "too short");
    }
  }

  /* Create a source instance for this NTP source */
  result->source = SRC_CreateNewInstance(UTI_IPToRefid(&remote_addr->ip_addr),
                                         SRC_NTP, params->sel_options,
                                         &result->remote_addr.ip_addr,
                                         params->min_samples, params->max_samples);

  result->rx_timeout_id = 0;
  result->tx_timeout_id = 0;
  result->tx_suspended = 1;
  result->opmode = params->online ? MD_ONLINE : MD_OFFLINE;
  result->local_poll = result->minpoll;
  result->poll_score = 0.0;
  UTI_ZeroTimespec(&result->local_tx.ts);
  result->local_tx.err = 0.0;
  result->local_tx.source = NTP_TS_DAEMON;
  
  NCR_ResetInstance(result);

  if (params->iburst) {
    NCR_InitiateSampleBurst(result, IBURST_GOOD_SAMPLES, IBURST_TOTAL_SAMPLES);
  }

  return result;
}

/* ================================================== */

/* Destroy an instance */
void
NCR_DestroyInstance(NCR_Instance instance)
{
  if (instance->opmode != MD_OFFLINE)
    take_offline(instance);

  if (instance->mode == MODE_ACTIVE)
    NIO_CloseServerSocket(instance->local_addr.sock_fd);

  /* This will destroy the source instance inside the
     structure, which will cause reselection if this was the
     synchronising source etc. */
  SRC_DestroyInstance(instance->source);

  /* Free the data structure */
  Free(instance);
}

/* ================================================== */

void
NCR_StartInstance(NCR_Instance instance)
{
  instance->tx_suspended = 0;
  if (instance->opmode != MD_OFFLINE)
    start_initial_timeout(instance);
}

/* ================================================== */

void
NCR_ResetInstance(NCR_Instance instance)
{
  instance->tx_count = 0;
  instance->presend_done = 0;

  instance->remote_poll = 0;
  instance->remote_stratum = 0;

  instance->valid_rx = 0;
  instance->valid_timestamps = 0;
  instance->updated_timestamps = 0;
  UTI_ZeroNtp64(&instance->remote_ntp_rx);
  UTI_ZeroNtp64(&instance->remote_ntp_tx);
  UTI_ZeroNtp64(&instance->local_ntp_rx);
  UTI_ZeroNtp64(&instance->local_ntp_tx);
  UTI_ZeroTimespec(&instance->local_rx.ts);
  instance->local_rx.err = 0.0;
  instance->local_rx.source = NTP_TS_DAEMON;
}

/* ================================================== */

void
NCR_ResetPoll(NCR_Instance instance)
{
  if (instance->local_poll != instance->minpoll) {
    instance->local_poll = instance->minpoll;

    /* The timer was set with a longer poll interval, restart it */
    if (instance->tx_timeout_id)
      restart_timeout(instance, get_transmit_delay(instance, 0, 0.0));
  }
}

/* ================================================== */

void
NCR_ChangeRemoteAddress(NCR_Instance inst, NTP_Remote_Address *remote_addr)
{
  NCR_ResetInstance(inst);
  inst->remote_addr = *remote_addr;

  if (inst->mode == MODE_CLIENT)
    close_client_socket(inst);
  else {
    NIO_CloseServerSocket(inst->local_addr.sock_fd);
    inst->local_addr.ip_addr.family = IPADDR_UNSPEC;
    inst->local_addr.sock_fd = NIO_OpenServerSocket(remote_addr);
  }

  /* Update the reference ID and reset the source/sourcestats instances */
  SRC_SetRefid(inst->source, UTI_IPToRefid(&remote_addr->ip_addr),
               &inst->remote_addr.ip_addr);
  SRC_ResetInstance(inst->source);
}

/* ================================================== */

static void
adjust_poll(NCR_Instance inst, double adj)
{
  inst->poll_score += adj;

  if (inst->poll_score >= 1.0) {
    inst->local_poll += (int)inst->poll_score;
    inst->poll_score -= (int)inst->poll_score;
  }

  if (inst->poll_score < 0.0) {
    inst->local_poll += (int)(inst->poll_score - 1.0);
    inst->poll_score -= (int)(inst->poll_score - 1.0);
  }
  
  /* Clamp polling interval to defined range */
  if (inst->local_poll < inst->minpoll) {
    inst->local_poll = inst->minpoll;
    inst->poll_score = 0;
  } else if (inst->local_poll > inst->maxpoll) {
    inst->local_poll = inst->maxpoll;
    inst->poll_score = 1.0;
  }
}

/* ================================================== */

static double
get_poll_adj(NCR_Instance inst, double error_in_estimate, double peer_distance)
{
  double poll_adj;

  if (error_in_estimate > peer_distance) {
    int shift = 0;
    unsigned long temp = (int)(error_in_estimate / peer_distance);
    do {
      shift++;
      temp>>=1;
    } while (temp);

    poll_adj = -shift - inst->poll_score + 0.5;

  } else {
    int samples = SRC_Samples(inst->source);

    /* Adjust polling interval so that the number of sourcestats samples
       remains close to the target value */
    poll_adj = ((double)samples / inst->poll_target - 1.0) / inst->poll_target;

    /* Make interval shortening quicker */
    if (samples < inst->poll_target) {
      poll_adj *= 2.0;
    }
  }

  return poll_adj;
}

/* ================================================== */

static double
get_transmit_delay(NCR_Instance inst, int on_tx, double last_tx)
{
  int poll_to_use, stratum_diff;
  double delay_time;

  /* If we're in burst mode, queue for immediate dispatch.

     If we're operating in client/server mode, queue the timeout for
     the poll interval hence.  The fact that a timeout has been queued
     in the transmit handler is immaterial - that is only done so that
     we at least send something, if no reply is heard.

     If we're in symmetric mode, we have to take account of the peer's
     wishes, otherwise his sampling regime will fall to pieces.  If
     we're in client/server mode, we don't care what poll interval the
     server responded with last time. */

  switch (inst->opmode) {
    case MD_OFFLINE:
      assert(0);
      break;
    case MD_ONLINE:
      /* Normal processing, depending on whether we're in
         client/server or symmetric mode */

      switch(inst->mode) {
        case MODE_CLIENT:
          /* Client/server association - aim at some randomised time
             approx the poll interval away */
          poll_to_use = inst->local_poll;

          delay_time = (double) (1UL<<poll_to_use);
          if (inst->presend_done)
            delay_time = WARM_UP_DELAY;

          break;

        case MODE_ACTIVE:
          /* Symmetric active association - aim at some randomised time approx
             the poll interval away since the last transmit */

          /* Use shorter of the local and remote poll interval, but not shorter
             than the allowed minimum */
          poll_to_use = inst->local_poll;
          if (poll_to_use > inst->remote_poll)
            poll_to_use = inst->remote_poll;
          if (poll_to_use < inst->minpoll)
            poll_to_use = inst->minpoll;

          delay_time = (double) (1UL<<poll_to_use);

          /* If the remote stratum is higher than ours, try to lock on the
             peer's polling to minimize our response time by slightly extending
             our delay or waiting for the peer to catch up with us as the
             random part in the actual interval is reduced. If the remote
             stratum is equal to ours, try to interleave evenly with the peer. */
          stratum_diff = inst->remote_stratum - REF_GetOurStratum();
          if ((stratum_diff > 0 && last_tx * PEER_SAMPLING_ADJ < delay_time) ||
              (!on_tx && !stratum_diff &&
               last_tx / delay_time > PEER_SAMPLING_ADJ - 0.5))
            delay_time *= PEER_SAMPLING_ADJ;

          /* Substract the already spend time */
          if (last_tx > 0.0)
            delay_time -= last_tx;
          if (delay_time < 0.0)
            delay_time = 0.0;

          break;
        default:
          assert(0);
          break;
      }
      break;

    case MD_BURST_WAS_ONLINE:
    case MD_BURST_WAS_OFFLINE:
      /* Burst modes */
      delay_time = on_tx ? BURST_TIMEOUT : BURST_INTERVAL;
      break;
    default:
      assert(0);
      break;
  }

  return delay_time;
}

/* ================================================== */
/* Timeout handler for closing the client socket when no acceptable
   reply can be received from the server */

static void
receive_timeout(void *arg)
{
  NCR_Instance inst = (NCR_Instance)arg;

  DEBUG_LOG(LOGF_NtpCore, "Receive timeout for [%s:%d]",
            UTI_IPToString(&inst->remote_addr.ip_addr), inst->remote_addr.port);

  inst->rx_timeout_id = 0;
  close_client_socket(inst);
}

/* ================================================== */

static int
transmit_packet(NTP_Mode my_mode, /* The mode this machine wants to be */
                int interleaved, /* Flag enabling interleaved mode */
                int my_poll, /* The log2 of the local poll interval */
                int version, /* The NTP version to be set in the packet */
                int auth_mode, /* The authentication mode */
                uint32_t key_id, /* The authentication key ID */
                NTP_int64 *remote_ntp_rx, /* The receive timestamp from received packet */
                NTP_int64 *remote_ntp_tx, /* The transmit timestamp from received packet */
                NTP_Local_Timestamp *local_rx, /* The RX time of the received packet */
                NTP_Local_Timestamp *local_tx, /* The TX time of the previous packet
                                                  RESULT : TX time of this packet */
                NTP_int64 *local_ntp_rx, /* RESULT : receive timestamp from this packet */
                NTP_int64 *local_ntp_tx, /* RESULT : transmit timestamp from this packet */
                NTP_Remote_Address *where_to, /* Where to address the reponse to */
                NTP_Local_Address *from /* From what address to send it */
                )
{
  NTP_Packet message;
  int auth_len, length, ret, precision;
  struct timespec local_receive, local_transmit;
  NTP_int64 ts_fuzz;

  /* Parameters read from reference module */
  int are_we_synchronised, our_stratum, smooth_time;
  NTP_Leap leap_status;
  uint32_t our_ref_id;
  struct timespec our_ref_time;
  double our_root_delay, our_root_dispersion, smooth_offset;

  /* Don't reply with version higher than ours */
  if (version > NTP_VERSION) {
    version = NTP_VERSION;
  }

  /* Allow interleaved mode only if there was a prior transmission */
  if (interleaved && (!local_tx || UTI_IsZeroTimespec(&local_tx->ts)))
    interleaved = 0;

  smooth_time = 0;
  smooth_offset = 0.0;

  if (my_mode == MODE_CLIENT) {
    /* Don't reveal local time or state of the clock in client packets */
    precision = 32;
    leap_status = our_stratum = our_ref_id = 0;
    our_root_delay = our_root_dispersion = 0.0;
    UTI_ZeroTimespec(&our_ref_time);
  } else {
    /* This is accurate enough and cheaper than calling LCL_ReadCookedTime.
       A more accurate timestamp will be taken later in this function. */
    SCH_GetLastEventTime(&local_transmit, NULL, NULL);

    REF_GetReferenceParams(&local_transmit,
                           &are_we_synchronised, &leap_status,
                           &our_stratum,
                           &our_ref_id, &our_ref_time,
                           &our_root_delay, &our_root_dispersion);

    /* Get current smoothing offset when sending packet to a client */
    if (SMT_IsEnabled() && (my_mode == MODE_SERVER || my_mode == MODE_BROADCAST)) {
      smooth_offset = SMT_GetOffset(&local_transmit);
      smooth_time = fabs(smooth_offset) > LCL_GetSysPrecisionAsQuantum();

      /* Suppress leap second when smoothing and slew mode are enabled */
      if (REF_GetLeapMode() == REF_LeapModeSlew &&
          (leap_status == LEAP_InsertSecond || leap_status == LEAP_DeleteSecond))
        leap_status = LEAP_Normal;
    }

    precision = LCL_GetSysPrecisionAsLog();
  }

  if (smooth_time && !UTI_IsZeroTimespec(&local_rx->ts)) {
    our_ref_id = NTP_REFID_SMOOTH;
    UTI_AddDoubleToTimespec(&our_ref_time, smooth_offset, &our_ref_time);
    UTI_AddDoubleToTimespec(&local_rx->ts, smooth_offset, &local_receive);
  } else {
    local_receive = local_rx->ts;
  }

  /* Generate transmit packet */
  message.lvm = NTP_LVM(leap_status, version, my_mode);
  /* Stratum 16 and larger are invalid */
  if (our_stratum < NTP_MAX_STRATUM) {
    message.stratum = our_stratum;
  } else {
    message.stratum = NTP_INVALID_STRATUM;
  }
 
  message.poll = my_poll;
  message.precision = precision;

  /* If we're sending a client mode packet and we aren't synchronized yet, 
     we might have to set up artificial values for some of these parameters */
  message.root_delay = UTI_DoubleToNtp32(our_root_delay);
  message.root_dispersion = UTI_DoubleToNtp32(our_root_dispersion);

  message.reference_id = htonl(our_ref_id);

  /* Now fill in timestamps */

  UTI_TimespecToNtp64(&our_ref_time, &message.reference_ts, NULL);

  /* Originate - this comes from the last packet the source sent us */
  message.originate_ts = interleaved ? *remote_ntp_rx : *remote_ntp_tx;

  /* Prepare random bits which will be added to the receive timestamp */
  UTI_GetNtp64Fuzz(&ts_fuzz, precision);

  /* Receive - this is when we received the last packet from the source.
     This timestamp will have been adjusted so that it will now look to
     the source like we have been running on our latest estimate of
     frequency all along */
  UTI_TimespecToNtp64(&local_receive, &message.receive_ts, &ts_fuzz);

  /* Prepare random bits which will be added to the transmit timestamp. */
  UTI_GetNtp64Fuzz(&ts_fuzz, precision);

  /* Transmit - this our local time right now!  Also, we might need to
     store this for our own use later, next time we receive a message
     from the source we're sending to now. */
  LCL_ReadCookedTime(&local_transmit, NULL);

  if (smooth_time)
    UTI_AddDoubleToTimespec(&local_transmit, smooth_offset, &local_transmit);

  length = NTP_NORMAL_PACKET_LENGTH;

  /* Authenticate the packet if needed */

  if (auth_mode == AUTH_SYMMETRIC || auth_mode == AUTH_MSSNTP) {
    /* Pre-compensate the transmit time by approx. how long it will
       take to generate the authentication data. */
    local_transmit.tv_nsec += auth_mode == AUTH_SYMMETRIC ?
                              KEY_GetAuthDelay(key_id) : NSD_GetAuthDelay(key_id);
    UTI_NormaliseTimespec(&local_transmit);
    UTI_TimespecToNtp64(interleaved ? &local_tx->ts : &local_transmit,
                        &message.transmit_ts, &ts_fuzz);

    if (auth_mode == AUTH_SYMMETRIC) {
      auth_len = KEY_GenerateAuth(key_id, (unsigned char *) &message,
                                  offsetof(NTP_Packet, auth_keyid),
                                  (unsigned char *)&message.auth_data,
                                  sizeof (message.auth_data));
      if (!auth_len) {
        DEBUG_LOG(LOGF_NtpCore, "Could not generate auth data with key %"PRIu32, key_id);
        return 0;
      }
      message.auth_keyid = htonl(key_id);
      length += sizeof (message.auth_keyid) + auth_len;
    } else if (auth_mode == AUTH_MSSNTP) {
      /* MS-SNTP packets are signed (asynchronously) by ntp_signd */
      return NSD_SignAndSendPacket(key_id, &message, where_to, from, length);
    }
  } else {
    UTI_TimespecToNtp64(interleaved ? &local_tx->ts : &local_transmit,
                        &message.transmit_ts, &ts_fuzz);
  }

  ret = NIO_SendPacket(&message, where_to, from, length, local_tx != NULL);

  if (local_tx) {
    local_tx->ts = local_transmit;
    local_tx->err = 0.0;
    local_tx->source = NTP_TS_DAEMON;
  }

  if (local_ntp_rx)
    *local_ntp_rx = message.receive_ts;
  if (local_ntp_tx)
    *local_ntp_tx = message.transmit_ts;

  return ret;
}

/* ================================================== */
/* Timeout handler for transmitting to a source. */

static void
transmit_timeout(void *arg)
{
  NCR_Instance inst = (NCR_Instance) arg;
  NTP_Local_Address local_addr;
  int sent;

  inst->tx_timeout_id = 0;

  switch (inst->opmode) {
    case MD_BURST_WAS_ONLINE:
      /* With online burst switch to online before last packet */
      if (inst->burst_total_samples_to_go <= 1)
        inst->opmode = MD_ONLINE;
    case MD_BURST_WAS_OFFLINE:
      if (inst->burst_total_samples_to_go <= 0)
        take_offline(inst);
      break;
    default:
      break;
  }

  /* With auto_offline take the source offline on 2nd missed reply */
  if (inst->auto_offline && inst->tx_count >= 2)
    NCR_TakeSourceOffline(inst);

  if (inst->opmode == MD_OFFLINE) {
    return;
  }

  DEBUG_LOG(LOGF_NtpCore, "Transmit timeout for [%s:%d]",
      UTI_IPToString(&inst->remote_addr.ip_addr), inst->remote_addr.port);

  /* Open new client socket */
  if (inst->mode == MODE_CLIENT) {
    close_client_socket(inst);
    assert(inst->local_addr.sock_fd == INVALID_SOCK_FD);
    inst->local_addr.sock_fd = NIO_OpenClientSocket(&inst->remote_addr);
  }

  /* Don't require the packet to be sent from the same address as before */
  local_addr.ip_addr.family = IPADDR_UNSPEC;
  local_addr.sock_fd = inst->local_addr.sock_fd;

  /* Check whether we need to 'warm up' the link to the other end by
     sending an NTP exchange to ensure both ends' ARP caches are
     primed.  On loaded systems this might also help ensure that bits
     of the program are paged in properly before we start. */

  if ((inst->presend_minpoll > 0) &&
      (inst->presend_minpoll <= inst->local_poll) &&
      !inst->presend_done) {
    inst->presend_done = 1;
  } else {
    /* Reset for next time */
    inst->presend_done = 0;
  }

  sent = transmit_packet(inst->mode, inst->interleaved, inst->local_poll,
                         inst->version,
                         inst->auth_mode, inst->auth_key_id,
                         &inst->remote_ntp_rx, &inst->remote_ntp_tx,
                         &inst->local_rx, &inst->local_tx,
                         &inst->local_ntp_rx, &inst->local_ntp_tx,
                         &inst->remote_addr,
                         &local_addr);

  ++inst->tx_count;
  inst->valid_rx = 0;
  inst->updated_timestamps = 0;

  /* If the source loses connectivity and our packets are still being sent,
     back off the sampling rate to reduce the network traffic.  If it's the
     source to which we are currently locked, back off slowly. */

  if (inst->tx_count >= 2) {
    /* Implies we have missed at least one transmission */

    if (sent) {
      adjust_poll(inst, SRC_IsSyncPeer(inst->source) ? 0.1 : 0.25);
    }

    SRC_UpdateReachability(inst->source, 0);
  }

  switch (inst->opmode) {
    case MD_BURST_WAS_ONLINE:
      /* When not reachable, don't stop online burst until sending succeeds */
      if (!sent && !SRC_IsReachable(inst->source))
        break;
      /* Fall through */
    case MD_BURST_WAS_OFFLINE:
      --inst->burst_total_samples_to_go;
      break;
    default:
      break;
  }

  /* Restart timer for this message */
  restart_timeout(inst, get_transmit_delay(inst, 1, 0.0));

  /* If a client packet was just sent, schedule a timeout to close the socket
     at the time when all server replies would fail the delay test, so the
     socket is not open for longer than necessary */
  if (inst->mode == MODE_CLIENT)
    inst->rx_timeout_id = SCH_AddTimeoutByDelay(inst->max_delay + MAX_SERVER_INTERVAL,
                                                receive_timeout, (void *)inst);
}

/* ================================================== */

static int
check_packet_format(NTP_Packet *message, int length)
{
  int version;

  /* Check version and length */

  version = NTP_LVM_TO_VERSION(message->lvm);
  if (version < NTP_MIN_COMPAT_VERSION || version > NTP_MAX_COMPAT_VERSION) {
    DEBUG_LOG(LOGF_NtpCore, "NTP packet has invalid version %d", version);
    return 0;
  } 

  if (length < NTP_NORMAL_PACKET_LENGTH || (unsigned int)length % 4) {
    DEBUG_LOG(LOGF_NtpCore, "NTP packet has invalid length %d", length);
    return 0;
  }

  /* We can't reliably check the packet for invalid extension fields as we
     support MACs longer than the shortest valid extension field */

  return 1;
}

/* ================================================== */

static int
is_zero_data(unsigned char *data, int length)
{
  int i;

  for (i = 0; i < length; i++)
    if (data[i])
      return 0;
  return 1;
}

/* ================================================== */

static int
check_packet_auth(NTP_Packet *pkt, int length,
                  AuthenticationMode *auth_mode, uint32_t *key_id)
{
  int i, version, remainder, ext_length;
  unsigned char *data;
  uint32_t id;

  /* Go through extension fields and see if there is a valid MAC */

  version = NTP_LVM_TO_VERSION(pkt->lvm);
  i = NTP_NORMAL_PACKET_LENGTH;
  data = (void *)pkt;

  while (1) {
    remainder = length - i;

    /* Check if the remaining data is a valid MAC.  This needs to be done
       before trying to parse it as an extension field, because we support
       MACs longer than the shortest valid extension field. */
    if (remainder >= NTP_MIN_MAC_LENGTH && remainder <= NTP_MAX_MAC_LENGTH) {
      id = ntohl(*(uint32_t *)(data + i));
      if (KEY_CheckAuth(id, (void *)pkt, i, (void *)(data + i + 4),
                        remainder - 4)) {
        *auth_mode = AUTH_SYMMETRIC;
        *key_id = id;
        return 1;
      }
    }

    /* Check if this is a valid NTPv4 extension field and skip it.  It should
       have a 16-bit type, 16-bit length, and data padded to 32 bits. */
    if (version == 4 && remainder >= NTP_MIN_EXTENSION_LENGTH) {
      ext_length = ntohs(*(uint16_t *)(data + i + 2));
      if (ext_length >= NTP_MIN_EXTENSION_LENGTH &&
          ext_length <= remainder && ext_length % 4 == 0) {
        i += ext_length;
        continue;
      }
    }

    /* Invalid or missing MAC, or format error */
    break;
  }

  /* This is not 100% reliable as a MAC could fail to authenticate and could
     pass as an extension field, leaving reminder smaller than the minimum MAC
     length */
  if (remainder >= NTP_MIN_MAC_LENGTH) {
    *auth_mode = AUTH_SYMMETRIC;
    *key_id = ntohl(*(uint32_t *)(data + i));

    /* Check if it is an MS-SNTP authenticator field or extended authenticator
       field with zeroes as digest */
    if (version == 3 && *key_id) {
      if (remainder == 20 && is_zero_data(data + i + 4, remainder - 4))
        *auth_mode = AUTH_MSSNTP;
      else if (remainder == 72 && is_zero_data(data + i + 8, remainder - 8))
        *auth_mode = AUTH_MSSNTP_EXT;
    }
  } else {
    *auth_mode = AUTH_NONE;
    *key_id = 0;
  }

  return 0;
}

/* ================================================== */

static int
receive_packet(NCR_Instance inst, NTP_Local_Address *local_addr,
               NTP_Local_Timestamp *rx_ts, NTP_Packet *message, int length)
{
  int pkt_leap;
  uint32_t pkt_refid, pkt_key_id;
  double pkt_root_delay;
  double pkt_root_dispersion;
  AuthenticationMode pkt_auth_mode;

  /* The local time to which the (offset, delay, dispersion) triple will
     be taken to relate.  For client/server operation this is practically
     the same as either the transmit or receive time.  The difference comes
     in symmetric active mode, when the receive may come minutes after the
     transmit, and this time will be midway between the two */
  struct timespec sample_time;

  /* The estimated offset in seconds, a positive value indicates that the local
     clock is SLOW of the remote source and a negative value indicates that the
     local clock is FAST of the remote source */
  double offset;

  /* The estimated peer delay, dispersion and distance */
  double delay, dispersion, distance;

  /* The total root delay and dispersion */
  double root_delay, root_dispersion;

  /* The skew and estimated frequency offset relative to the remote source */
  double skew, source_freq_lo, source_freq_hi;

  /* RFC 5905 packet tests */
  int test1, test2n, test2i, test2, test3, test5, test6, test7;
  int interleaved_packet, valid_packet, synced_packet;

  /* Additional tests */
  int testA, testB, testC, testD;
  int good_packet;

  /* Kiss-o'-Death codes */
  int kod_rate;

  /* The estimated offset predicted from previous samples.  The
     convention here is that positive means local clock FAST of
     reference, i.e. backwards to the way that 'offset' is defined. */
  double estimated_offset;

  /* The absolute difference between the offset estimate and
     measurement in seconds */
  double error_in_estimate;

  double delay_time, precision;

  /* ==================== */

  pkt_leap = NTP_LVM_TO_LEAP(message->lvm);
  pkt_refid = ntohl(message->reference_id);
  pkt_root_delay = UTI_Ntp32ToDouble(message->root_delay);
  pkt_root_dispersion = UTI_Ntp32ToDouble(message->root_dispersion);

  /* Check if the packet is valid per RFC 5905, section 8.
     The test values are 1 when passed and 0 when failed. */
  
  /* Test 1 checks for duplicate packet */
  test1 = !!UTI_CompareNtp64(&message->transmit_ts, &inst->remote_ntp_tx);

  /* Test 2 checks for bogus packet in the basic and interleaved modes.  This
     ensures the source is responding to the latest packet we sent to it. */
  test2n = !UTI_CompareNtp64(&message->originate_ts, &inst->local_ntp_tx);
  test2i = inst->interleaved &&
           !UTI_CompareNtp64(&message->originate_ts, &inst->local_ntp_rx);
  test2 = test2n || test2i;
  interleaved_packet = !test2n && test2i;
  
  /* Test 3 checks for invalid timestamps.  This can happen when the
     association if not properly 'up'. */
  test3 = !UTI_IsZeroNtp64(&message->originate_ts) &&
          !UTI_IsZeroNtp64(&message->receive_ts) &&
          !UTI_IsZeroNtp64(&message->transmit_ts);

  /* Test 4 would check for denied access.  It would always pass as this
     function is called only for known sources. */

  /* Test 5 checks for authentication failure.  If we expect authenticated info
     from this peer/server and the packet doesn't have it, the authentication
     is bad, or it's authenticated with a different key than expected, it's got
     to fail.  If we don't expect the packet to be authenticated, just ignore
     the test. */
  test5 = inst->auth_mode == AUTH_NONE ||
          (check_packet_auth(message, length, &pkt_auth_mode, &pkt_key_id) &&
           pkt_auth_mode == inst->auth_mode && pkt_key_id == inst->auth_key_id);

  /* Test 6 checks for unsynchronised server */
  test6 = pkt_leap != LEAP_Unsynchronised &&
          message->stratum < NTP_MAX_STRATUM &&
          message->stratum != NTP_INVALID_STRATUM; 

  /* Test 7 checks for bad data.  The root distance must be smaller than a
     defined maximum. */
  test7 = pkt_root_delay / 2.0 + pkt_root_dispersion < NTP_MAX_DISPERSION;

  /* The packet is considered valid if the tests 1-5 passed.  The timestamps
     can be used for synchronisation if the tests 6 and 7 passed too. */
  valid_packet = test1 && test2 && test3 && test5;
  synced_packet = valid_packet && test6 && test7;

  /* Check for Kiss-o'-Death codes */
  kod_rate = 0;
  if (test1 && test2 && test5 && pkt_leap == LEAP_Unsynchronised &&
      message->stratum == NTP_INVALID_STRATUM) {
    if (pkt_refid == KOD_RATE)
      kod_rate = 1;
  }

  if (synced_packet && (!interleaved_packet || inst->valid_timestamps)) {
    /* These are the timespec equivalents of the remote and local epochs */
    struct timespec remote_receive, remote_transmit, prev_remote_receive;
    struct timespec local_average, remote_average;
    double remote_interval, local_interval, server_interval;

    precision = LCL_GetSysPrecisionAsQuantum() +
                UTI_Log2ToDouble(message->precision);

    SRC_GetFrequencyRange(inst->source, &source_freq_lo, &source_freq_hi);
    
    UTI_Ntp64ToTimespec(&message->receive_ts, &remote_receive);
    UTI_Ntp64ToTimespec(&message->transmit_ts, &remote_transmit);

    /* Calculate intervals between remote and local timestamps */
    if (interleaved_packet) {
      UTI_Ntp64ToTimespec(&inst->remote_ntp_rx, &prev_remote_receive);
      UTI_AverageDiffTimespecs(&remote_transmit, &remote_receive,
                               &remote_average, &remote_interval);
      UTI_AverageDiffTimespecs(&inst->local_rx.ts, &inst->local_tx.ts,
                               &local_average, &local_interval);
      server_interval = UTI_DiffTimespecsToDouble(&remote_transmit,
                                                  &prev_remote_receive);
    } else {
      UTI_AverageDiffTimespecs(&remote_receive, &remote_transmit,
                               &remote_average, &remote_interval);
      UTI_AverageDiffTimespecs(&inst->local_tx.ts, &rx_ts->ts,
                               &local_average, &local_interval);
      server_interval = remote_interval;
    }

    /* In our case, we work out 'delay' as the worst case delay,
       assuming worst case frequency error between us and the other
       source */
    delay = local_interval - remote_interval * (1.0 + source_freq_lo);

    /* Clamp delay to avoid misleading results later */
    delay = fabs(delay);
    if (delay < precision)
      delay = precision;
    
    /* Calculate offset.  Following the NTP definition, this is negative
       if we are fast of the remote source. */
    offset = UTI_DiffTimespecsToDouble(&remote_average, &local_average);

    /* Apply configured correction */
    offset += inst->offset_correction;

    /* We treat the time of the sample as being midway through the local
       measurement period.  An analysis assuming constant relative
       frequency and zero network delay shows this is the only possible
       choice to estimate the frequency difference correctly for every
       sample pair. */
    sample_time = local_average;
    
    /* Calculate skew */
    skew = (source_freq_hi - source_freq_lo) / 2.0;
    
    /* and then calculate peer dispersion */
    dispersion = precision + rx_ts->err + skew * fabs(local_interval);
    
    /* Additional tests required to pass before accumulating the sample */

    /* Test A requires that the peer delay is not larger than the configured
       maximum, in client mode that the server processing time is sane, and in
       interleaved symmetric mode that the delay is not longer than half of the
       remote polling interval to detect missed packets */
    testA = delay <= inst->max_delay &&
            !(inst->mode == MODE_CLIENT && server_interval > MAX_SERVER_INTERVAL) &&
            !(inst->mode == MODE_ACTIVE && interleaved_packet &&
              delay > UTI_Log2ToDouble(message->poll - 1));

    /* Test B requires that the ratio of the round trip delay to the
       minimum one currently in the stats data register is less than an
       administrator-defined value */
    testB = inst->max_delay_ratio <= 1.0 ||
            delay / SRC_MinRoundTripDelay(inst->source) <= inst->max_delay_ratio;

    /* Test C requires that the ratio of the increase in delay from the minimum
       one in the stats data register to the standard deviation of the offsets
       in the register is less than an administrator-defined value or the
       difference between measured offset and predicted offset is larger than
       the increase in delay */
    testC = SRC_IsGoodSample(inst->source, -offset, delay,
                             inst->max_delay_dev_ratio, LCL_GetMaxClockError(),
                             &sample_time);

    /* Test D requires that the remote peer is not synchronised to us to
       prevent a synchronisation loop */
    testD = message->stratum <= 1 || REF_GetMode() != REF_ModeNormal ||
            pkt_refid != UTI_IPToRefid(&local_addr->ip_addr);
  } else {
    offset = delay = dispersion = 0.0;
    sample_time = rx_ts->ts;
    testA = testB = testC = testD = 0;
  }
  
  /* The packet is considered good for synchronisation if
     the additional tests passed */
  good_packet = testA && testB && testC && testD;

  root_delay = pkt_root_delay + delay;
  root_dispersion = pkt_root_dispersion + dispersion;
  distance = dispersion + 0.5 * delay;

  /* Update the NTP timestamps.  If it's a valid packet from a synchronised
     source, the timestamps may be used later when processing a packet in the
     interleaved mode.  Protect the timestamps against replay attacks in client
     mode, and also in symmetric mode as long as the peers use the same polling
     interval and never start with clocks in future or very distant past.
     The authentication test (test5) is required to prevent DoS attacks using
     unauthenticated packets on authenticated symmetric associations. */
  if ((inst->mode == MODE_CLIENT && valid_packet && !inst->valid_rx) ||
      (inst->mode == MODE_ACTIVE && (valid_packet || !inst->valid_rx) &&
       test5 && !UTI_IsZeroNtp64(&message->transmit_ts) &&
       (!inst->updated_timestamps || (valid_packet && !inst->valid_rx) ||
        UTI_CompareNtp64(&inst->remote_ntp_tx, &message->transmit_ts) < 0))) {
    inst->remote_ntp_rx = message->receive_ts;
    inst->remote_ntp_tx = message->transmit_ts;
    inst->local_rx = *rx_ts;
    inst->valid_timestamps = synced_packet;
    inst->updated_timestamps = 1;
  }

  /* Accept at most one response per request.  The NTP specification recommends
     resetting local_ntp_tx to make the following packets fail test2 or test3,
     but that would not allow the code above to make multiple updates of the
     timestamps in symmetric mode.  Also, ignore presend responses. */
  if (inst->valid_rx) {
    test2 = test3 = 0;
    valid_packet = synced_packet = good_packet = 0;
  } else if (valid_packet) {
    if (inst->presend_done) {
      testA = 0;
      good_packet = 0;
    }
    inst->valid_rx = 1;
  }

  DEBUG_LOG(LOGF_NtpCore, "NTP packet lvm=%o stratum=%d poll=%d prec=%d root_delay=%f root_disp=%f refid=%"PRIx32" [%s]",
            message->lvm, message->stratum, message->poll, message->precision,
            pkt_root_delay, pkt_root_dispersion, pkt_refid,
            message->stratum == NTP_INVALID_STRATUM ? UTI_RefidToString(pkt_refid) : "");
  DEBUG_LOG(LOGF_NtpCore, "reference=%s origin=%s receive=%s transmit=%s",
            UTI_Ntp64ToString(&message->reference_ts),
            UTI_Ntp64ToString(&message->originate_ts),
            UTI_Ntp64ToString(&message->receive_ts),
            UTI_Ntp64ToString(&message->transmit_ts));
  DEBUG_LOG(LOGF_NtpCore, "offset=%.9f delay=%.9f dispersion=%f root_delay=%f root_dispersion=%f",
            offset, delay, dispersion, root_delay, root_dispersion);
  DEBUG_LOG(LOGF_NtpCore, "test123=%d%d%d test567=%d%d%d testABCD=%d%d%d%d kod_rate=%d interleaved=%d presend=%d valid=%d good=%d updated=%d",
            test1, test2, test3, test5, test6, test7, testA, testB, testC, testD,
            kod_rate, interleaved_packet, inst->presend_done, valid_packet, good_packet,
            !UTI_CompareTimespecs(&inst->local_rx.ts, &rx_ts->ts));

  if (valid_packet) {
    if (synced_packet) {
      inst->remote_poll = message->poll;
      inst->remote_stratum = message->stratum;
      inst->tx_count = 0;
      SRC_UpdateReachability(inst->source, 1);
    }

    if (good_packet) {
      /* Do this before we accumulate a new sample into the stats registers, obviously */
      estimated_offset = SRC_PredictOffset(inst->source, &sample_time);

      SRC_AccumulateSample(inst->source,
                           &sample_time,
                           offset, delay, dispersion,
                           root_delay, root_dispersion,
                           MAX(message->stratum, inst->min_stratum),
                           (NTP_Leap) pkt_leap);

      SRC_SelectSource(inst->source);

      /* Now examine the registers.  First though, if the prediction is
         not even within +/- the peer distance of the peer, we are clearly
         not tracking the peer at all well, so we back off the sampling
         rate depending on just how bad the situation is. */
      error_in_estimate = fabs(-offset - estimated_offset);

      /* Now update the polling interval */
      adjust_poll(inst, get_poll_adj(inst, error_in_estimate, distance));

      /* If we're in burst mode, check whether the burst is completed and
         revert to the previous mode */
      switch (inst->opmode) {
        case MD_BURST_WAS_ONLINE:
        case MD_BURST_WAS_OFFLINE:
          --inst->burst_good_samples_to_go;
          if (inst->burst_good_samples_to_go <= 0) {
            if (inst->opmode == MD_BURST_WAS_ONLINE)
              inst->opmode = MD_ONLINE;
            else
              take_offline(inst);
          }
          break;
        default:
          break;
      }
    } else if (synced_packet) {
      /* Slowly increase the polling interval if we can't get good packet */
      adjust_poll(inst, 0.1);
    }

    /* If in client mode, no more packets are expected to be coming from the
       server and the socket can be closed */
    close_client_socket(inst);

    /* Update the local address */
    inst->local_addr.ip_addr = local_addr->ip_addr;

    /* And now, requeue the timer */
    if (inst->opmode != MD_OFFLINE) {
      delay_time = get_transmit_delay(inst, 0,
                     UTI_DiffTimespecsToDouble(&inst->local_rx.ts, &inst->local_tx.ts));

      if (kod_rate) {
        /* Back off for a while and stop ongoing burst */
        delay_time += 4 * (1UL << inst->minpoll);

        if (inst->opmode == MD_BURST_WAS_OFFLINE || inst->opmode == MD_BURST_WAS_ONLINE) {
          inst->burst_good_samples_to_go = 0;
          LOG(LOGS_WARN, LOGF_NtpCore, "Received KoD RATE from %s, burst sampling stopped",
              UTI_IPToString(&inst->remote_addr.ip_addr));
        }
      }

      /* Get rid of old timeout and start a new one */
      assert(inst->tx_timeout_id);
      restart_timeout(inst, delay_time);
    }
  }

  /* Do measurement logging */
  if (logfileid != -1) {
    LOG_FileWrite(logfileid, "%s %-15s %1c %2d %1d%1d%1d %1d%1d%1d %1d%1d%1d%d  %2d %2d %4.2f %10.3e %10.3e %10.3e %10.3e %10.3e %08X %1d%1c %1c %1c",
            UTI_TimeToLogForm(sample_time.tv_sec),
            UTI_IPToString(&inst->remote_addr.ip_addr),
            leap_chars[pkt_leap],
            message->stratum,
            test1, test2, test3, test5, test6, test7, testA, testB, testC, testD,
            inst->local_poll, message->poll,
            inst->poll_score,
            offset, delay, dispersion,
            pkt_root_delay, pkt_root_dispersion, pkt_refid,
            NTP_LVM_TO_MODE(message->lvm), interleaved_packet ? 'I' : 'B',
            tss_chars[CLAMP(0, inst->local_tx.source, sizeof (tss_chars))],
            tss_chars[CLAMP(0, rx_ts->source, sizeof (tss_chars))]);
  }            

  return good_packet;
}

/* ================================================== */
/* From RFC 5905, the standard handling of received packets, depending
   on the mode of the packet and of the source, is :

   +------------------+---------------------------------------+
   |                  |              Packet Mode              |
   +------------------+-------+-------+-------+-------+-------+
   | Association Mode |   1   |   2   |   3   |   4   |   5   |
   +------------------+-------+-------+-------+-------+-------+
   | No Association 0 | NEWPS | DSCRD | FXMIT | MANY  | NEWBC |
   | Symm. Active   1 | PROC  | PROC  | DSCRD | DSCRD | DSCRD |
   | Symm. Passive  2 | PROC  | ERR   | DSCRD | DSCRD | DSCRD |
   | Client         3 | DSCRD | DSCRD | DSCRD | PROC  | DSCRD |
   | Server         4 | DSCRD | DSCRD | DSCRD | DSCRD | DSCRD |
   | Broadcast      5 | DSCRD | DSCRD | DSCRD | DSCRD | DSCRD |
   | Bcast Client   6 | DSCRD | DSCRD | DSCRD | DSCRD | PROC  |
   +------------------+-------+-------+-------+-------+-------+

   Association mode 0 is implemented in NCR_ProcessRxUnknown(), other modes
   in NCR_ProcessRxKnown().

   Broadcast, manycast and ephemeral symmetric passive associations are not
   supported yet.
 */

/* ================================================== */
/* This routine is called when a new packet arrives off the network,
   and it relates to a source we have an ongoing protocol exchange with */

int
NCR_ProcessRxKnown(NCR_Instance inst, NTP_Local_Address *local_addr,
                   NTP_Local_Timestamp *rx_ts, NTP_Packet *message, int length)
{
  int pkt_mode, proc_packet, proc_as_unknown;

  if (!check_packet_format(message, length))
    return 0;

  pkt_mode = NTP_LVM_TO_MODE(message->lvm);
  proc_packet = 0;
  proc_as_unknown = 0;

  /* Now, depending on the mode we decide what to do */
  switch (pkt_mode) {
    case MODE_ACTIVE:
      switch (inst->mode) {
        case MODE_ACTIVE:
          /* Ordinary symmetric peering */
          proc_packet = 1;
          break;
        case MODE_PASSIVE:
          /* In this software this case should not arise, we don't
             support unconfigured peers */
          break;
        case MODE_CLIENT:
          /* This is where we have the remote configured as a server and he has
             us configured as a peer, process as from an unknown source */
          proc_as_unknown = 1;
          break;
        default:
          /* Discard */
          break;
      }
      break;

    case MODE_PASSIVE:
      switch (inst->mode) {
        case MODE_ACTIVE:
          /* This would arise if we have the remote configured as a peer and
             he does not have us configured */
          proc_packet = 1;
          break;
        case MODE_PASSIVE:
          /* Error condition in RFC 5905 */
          break;
        default:
          /* Discard */
          break;
      }
      break;

    case MODE_CLIENT:
      /* If message is client mode, we just respond with a server mode
         packet, regardless of what we think the remote machine is
         supposed to be.  However, even though this is a configured
         peer or server, we still implement access restrictions on
         client mode operation.

         This copes with the case for an isolated network where one
         machine is set by eye and is used as the master, with the
         other machines pointed at it.  If the master goes down, we
         want to be able to reset its time at startup by relying on
         one of the secondaries to flywheel it. The behaviour coded here
         is required in the secondaries to make this possible. */

      proc_as_unknown = 1;
      break;

    case MODE_SERVER:
      switch (inst->mode) {
        case MODE_CLIENT:
          /* Standard case where he's a server and we're the client */
          proc_packet = 1;
          break;
        default:
          /* Discard */
          break;
      }
      break;

    case MODE_BROADCAST:
      /* Just ignore these */
      break;

    default:
      /* Obviously ignore */
      break;
  }

  if (proc_packet) {
    /* Check if the reply was received by the socket that sent the request */
    if (local_addr->sock_fd != inst->local_addr.sock_fd) {
      DEBUG_LOG(LOGF_NtpCore,
                "Packet received by wrong socket %d (expected %d)",
                local_addr->sock_fd, inst->local_addr.sock_fd);
      return 0;
    }

    /* Ignore packets from offline sources */
    if (inst->opmode == MD_OFFLINE || inst->tx_suspended) {
      DEBUG_LOG(LOGF_NtpCore, "Packet from offline source");
      return 0;
    }

    return receive_packet(inst, local_addr, rx_ts, message, length);
  } else if (proc_as_unknown) {
    NCR_ProcessRxUnknown(&inst->remote_addr, local_addr, rx_ts, message, length);
    /* It's not a reply to our request, don't return success */
    return 0;
  } else {
    DEBUG_LOG(LOGF_NtpCore, "NTP packet discarded pkt_mode=%d our_mode=%d",
              pkt_mode, inst->mode);
    return 0;
  }
}

/* ================================================== */
/* This routine is called when a new packet arrives off the network,
   and it relates to a source we don't know (not our server or peer) */

void
NCR_ProcessRxUnknown(NTP_Remote_Address *remote_addr, NTP_Local_Address *local_addr,
                     NTP_Local_Timestamp *rx_ts, NTP_Packet *message, int length)
{
  NTP_Mode pkt_mode, my_mode;
  NTP_int64 *local_ntp_rx, *local_ntp_tx;
  NTP_Local_Timestamp local_tx, *tx_ts;
  int valid_auth, log_index, interleaved;
  AuthenticationMode auth_mode;
  uint32_t key_id;

  /* Ignore the packet if it wasn't received by server socket */
  if (!NIO_IsServerSocket(local_addr->sock_fd)) {
    DEBUG_LOG(LOGF_NtpCore, "NTP request packet received by client socket %d",
              local_addr->sock_fd);
    return;
  }

  if (!check_packet_format(message, length))
    return;

  if (!ADF_IsAllowed(access_auth_table, &remote_addr->ip_addr)) {
    DEBUG_LOG(LOGF_NtpCore, "NTP packet received from unauthorised host %s port %d",
              UTI_IPToString(&remote_addr->ip_addr),
              remote_addr->port);
    return;
  }

  pkt_mode = NTP_LVM_TO_MODE(message->lvm);

  switch (pkt_mode) {
    case MODE_ACTIVE:
      /* We are symmetric passive, even though we don't ever lock to him */
      my_mode = MODE_PASSIVE;
      break;
    case MODE_CLIENT:
      /* Reply with server packet */
      my_mode = MODE_SERVER;
      break;
    default:
      /* Discard */
      DEBUG_LOG(LOGF_NtpCore, "NTP packet discarded pkt_mode=%d", pkt_mode);
      return;
  }

  log_index = CLG_LogNTPAccess(&remote_addr->ip_addr, &rx_ts->ts);

  /* Don't reply to all requests if the rate is excessive */
  if (log_index >= 0 && CLG_LimitNTPResponseRate(log_index)) {
      DEBUG_LOG(LOGF_NtpCore, "NTP packet discarded to limit response rate");
      return;
  }

  /* Check if the packet includes MAC that authenticates properly */
  valid_auth = check_packet_auth(message, length, &auth_mode, &key_id);

  /* If authentication failed, select whether and how we should respond */
  if (!valid_auth) {
    switch (auth_mode) {
      case AUTH_NONE:
        /* Reply with no MAC */
        break;
      case AUTH_MSSNTP:
        /* Ignore the failure (MS-SNTP servers don't check client MAC) */
        break;
      default:
        /* Discard packets in other modes */
        DEBUG_LOG(LOGF_NtpCore, "NTP packet discarded auth_mode=%d", auth_mode);
        return;
    }
  }

  local_ntp_rx = local_ntp_tx = NULL;
  tx_ts = NULL;
  interleaved = 0;

  /* Check if the client is using the interleaved mode.  If it is, save the
     new transmit timestamp and if the old transmit timestamp is valid, respond
     in the interleaved mode.  This means the third reply to a new client is
     the earliest one that can be interleaved.  We don't want to waste time
     on clients that are not using the interleaved mode. */
  if (log_index >= 0) {
    CLG_GetNtpTimestamps(log_index, &local_ntp_rx, &local_ntp_tx);
    interleaved = !UTI_IsZeroNtp64(local_ntp_rx) &&
                  !UTI_CompareNtp64(&message->originate_ts, local_ntp_rx);

    if (interleaved) {
      if (!UTI_IsZeroNtp64(local_ntp_tx))
        UTI_Ntp64ToTimespec(local_ntp_tx, &local_tx.ts);
      else
        interleaved = 0;
      tx_ts = &local_tx;
    } else {
      UTI_ZeroNtp64(local_ntp_tx);
      local_ntp_tx = NULL;
    }
  }

  /* Send a reply */
  transmit_packet(my_mode, interleaved, message->poll, NTP_LVM_TO_VERSION(message->lvm),
                  auth_mode, key_id, &message->receive_ts, &message->transmit_ts,
                  rx_ts, tx_ts, local_ntp_rx, NULL, remote_addr, local_addr);

  /* Save the transmit timestamp */
  if (tx_ts)
    UTI_TimespecToNtp64(&tx_ts->ts, local_ntp_tx, NULL);
}

/* ================================================== */

static void
update_tx_timestamp(NTP_Local_Timestamp *tx_ts, NTP_Local_Timestamp *new_tx_ts,
                    NTP_int64 *local_ntp_rx, NTP_int64 *local_ntp_tx, NTP_Packet *message)
{
  double delay;

  if (UTI_IsZeroTimespec(&tx_ts->ts)) {
    DEBUG_LOG(LOGF_NtpCore, "Unexpected TX update");
    return;
  }

  /* Check if this is the last packet that was sent */
  if ((local_ntp_rx && UTI_CompareNtp64(&message->receive_ts, local_ntp_rx)) ||
      (local_ntp_tx && UTI_CompareNtp64(&message->transmit_ts, local_ntp_tx))) {
    DEBUG_LOG(LOGF_NtpCore, "RX/TX timestamp mismatch");
    return;
  }

  delay = UTI_DiffTimespecsToDouble(&new_tx_ts->ts, &tx_ts->ts);

  if (delay < 0.0 || delay > MAX_TX_DELAY) {
    DEBUG_LOG(LOGF_NtpCore, "Unacceptable TX delay %.9f", delay);
    return;
  }

  *tx_ts = *new_tx_ts;

  DEBUG_LOG(LOGF_NtpCore, "Updated TX timestamp delay=%.9f", delay);
}

/* ================================================== */

void
NCR_ProcessTxKnown(NCR_Instance inst, NTP_Local_Address *local_addr,
                   NTP_Local_Timestamp *tx_ts, NTP_Packet *message, int length)
{
  NTP_Mode pkt_mode;

  if (!check_packet_format(message, length))
    return;

  pkt_mode = NTP_LVM_TO_MODE(message->lvm);

  /* Server and passive mode packets are responses to unknown sources */
  if (pkt_mode != MODE_CLIENT && pkt_mode != MODE_ACTIVE) {
    NCR_ProcessTxUnknown(&inst->remote_addr, local_addr, tx_ts, message, length);
    return;
  }

  update_tx_timestamp(&inst->local_tx, tx_ts, &inst->local_ntp_rx, &inst->local_ntp_tx,
                      message);
}

/* ================================================== */

void
NCR_ProcessTxUnknown(NTP_Remote_Address *remote_addr, NTP_Local_Address *local_addr,
                     NTP_Local_Timestamp *tx_ts, NTP_Packet *message, int length)
{
  NTP_int64 *local_ntp_rx, *local_ntp_tx;
  NTP_Local_Timestamp local_tx;
  int log_index;

  if (!check_packet_format(message, length))
    return;

  log_index = CLG_GetClientIndex(&remote_addr->ip_addr);
  if (log_index < 0)
    return;

  CLG_GetNtpTimestamps(log_index, &local_ntp_rx, &local_ntp_tx);

  if (UTI_IsZeroNtp64(local_ntp_tx))
    return;

  UTI_Ntp64ToTimespec(local_ntp_tx, &local_tx.ts);
  update_tx_timestamp(&local_tx, tx_ts, local_ntp_rx, NULL, message);
  UTI_TimespecToNtp64(&local_tx.ts, local_ntp_tx, NULL);
}

/* ================================================== */

void
NCR_SlewTimes(NCR_Instance inst, struct timespec *when, double dfreq, double doffset)
{
  double delta;

  if (!UTI_IsZeroTimespec(&inst->local_rx.ts))
    UTI_AdjustTimespec(&inst->local_rx.ts, when, &inst->local_rx.ts, &delta, dfreq, doffset);
  if (!UTI_IsZeroTimespec(&inst->local_tx.ts))
    UTI_AdjustTimespec(&inst->local_tx.ts, when, &inst->local_tx.ts, &delta, dfreq, doffset);
}

/* ================================================== */

void
NCR_TakeSourceOnline(NCR_Instance inst)
{
  switch (inst->opmode) {
    case MD_ONLINE:
      /* Nothing to do */
      break;
    case MD_OFFLINE:
      LOG(LOGS_INFO, LOGF_NtpCore, "Source %s online", UTI_IPToString(&inst->remote_addr.ip_addr));
      inst->opmode = MD_ONLINE;
      NCR_ResetInstance(inst);
      start_initial_timeout(inst);
      break;
    case MD_BURST_WAS_ONLINE:
      /* Will revert */
      break;
    case MD_BURST_WAS_OFFLINE:
      inst->opmode = MD_BURST_WAS_ONLINE;
      LOG(LOGS_INFO, LOGF_NtpCore, "Source %s online", UTI_IPToString(&inst->remote_addr.ip_addr));
      break;
  }
}

/* ================================================== */

void
NCR_TakeSourceOffline(NCR_Instance inst)
{
  switch (inst->opmode) {
    case MD_ONLINE:
      LOG(LOGS_INFO, LOGF_NtpCore, "Source %s offline", UTI_IPToString(&inst->remote_addr.ip_addr));
      take_offline(inst);
      break;
    case MD_OFFLINE:
      break;
    case MD_BURST_WAS_ONLINE:
      inst->opmode = MD_BURST_WAS_OFFLINE;
      LOG(LOGS_INFO, LOGF_NtpCore, "Source %s offline", UTI_IPToString(&inst->remote_addr.ip_addr));
      break;
    case MD_BURST_WAS_OFFLINE:
      break;
  }

}

/* ================================================== */

void
NCR_ModifyMinpoll(NCR_Instance inst, int new_minpoll)
{
  if (new_minpoll < MIN_POLL || new_minpoll > MAX_POLL)
    return;
  inst->minpoll = new_minpoll;
  LOG(LOGS_INFO, LOGF_NtpCore, "Source %s new minpoll %d", UTI_IPToString(&inst->remote_addr.ip_addr), new_minpoll);
  if (inst->maxpoll < inst->minpoll)
    NCR_ModifyMaxpoll(inst, inst->minpoll);
}

/* ================================================== */

void
NCR_ModifyMaxpoll(NCR_Instance inst, int new_maxpoll)
{
  if (new_maxpoll < MIN_POLL || new_maxpoll > MAX_POLL)
    return;
  inst->maxpoll = new_maxpoll;
  LOG(LOGS_INFO, LOGF_NtpCore, "Source %s new maxpoll %d", UTI_IPToString(&inst->remote_addr.ip_addr), new_maxpoll);
  if (inst->minpoll > inst->maxpoll)
    NCR_ModifyMinpoll(inst, inst->maxpoll);
}

/* ================================================== */

void
NCR_ModifyMaxdelay(NCR_Instance inst, double new_max_delay)
{
  inst->max_delay = new_max_delay;
  LOG(LOGS_INFO, LOGF_NtpCore, "Source %s new max delay %f",
      UTI_IPToString(&inst->remote_addr.ip_addr), new_max_delay);
}

/* ================================================== */

void
NCR_ModifyMaxdelayratio(NCR_Instance inst, double new_max_delay_ratio)
{
  inst->max_delay_ratio = new_max_delay_ratio;
  LOG(LOGS_INFO, LOGF_NtpCore, "Source %s new max delay ratio %f",
      UTI_IPToString(&inst->remote_addr.ip_addr), new_max_delay_ratio);
}

/* ================================================== */

void
NCR_ModifyMaxdelaydevratio(NCR_Instance inst, double new_max_delay_dev_ratio)
{
  inst->max_delay_dev_ratio = new_max_delay_dev_ratio;
  LOG(LOGS_INFO, LOGF_NtpCore, "Source %s new max delay dev ratio %f",
      UTI_IPToString(&inst->remote_addr.ip_addr), new_max_delay_dev_ratio);
}

/* ================================================== */

void
NCR_ModifyMinstratum(NCR_Instance inst, int new_min_stratum)
{
  inst->min_stratum = new_min_stratum;
  LOG(LOGS_INFO, LOGF_NtpCore, "Source %s new minstratum %d",
      UTI_IPToString(&inst->remote_addr.ip_addr), new_min_stratum);
}

/* ================================================== */

void
NCR_ModifyPolltarget(NCR_Instance inst, int new_poll_target)
{
  inst->poll_target = new_poll_target;
  LOG(LOGS_INFO, LOGF_NtpCore, "Source %s new polltarget %d",
      UTI_IPToString(&inst->remote_addr.ip_addr), new_poll_target);
}

/* ================================================== */

void
NCR_InitiateSampleBurst(NCR_Instance inst, int n_good_samples, int n_total_samples)
{

  if (inst->mode == MODE_CLIENT) {

    /* We want to prevent burst mode being used on symmetric active
       associations - it will play havoc with the peer's sampling
       strategy. (This obviously relies on us having the peer
       configured that way if he has us configured symmetric active -
       but there's not much else we can do.) */

    switch (inst->opmode) {
      case MD_BURST_WAS_OFFLINE:
      case MD_BURST_WAS_ONLINE:
        /* If already burst sampling, don't start again */
        break;

      case MD_ONLINE:
      case MD_OFFLINE:
        inst->opmode = inst->opmode == MD_ONLINE ?
          MD_BURST_WAS_ONLINE : MD_BURST_WAS_OFFLINE;
        inst->burst_good_samples_to_go = n_good_samples;
        inst->burst_total_samples_to_go = n_total_samples;
        start_initial_timeout(inst);
        break;
      default:
        assert(0);
        break;
    }
  }

}

/* ================================================== */

void
NCR_ReportSource(NCR_Instance inst, RPT_SourceReport *report, struct timespec *now)
{
  report->poll = inst->local_poll;

  switch (inst->mode) {
    case MODE_CLIENT:
      report->mode = RPT_NTP_CLIENT;
      break;
    case MODE_ACTIVE:
      report->mode = RPT_NTP_PEER;
      break;
    default:
      assert(0);
  }
}

/* ================================================== */

int
NCR_AddAccessRestriction(IPAddr *ip_addr, int subnet_bits, int allow, int all)
 {
  ADF_Status status;

  if (allow) {
    if (all) {
      status = ADF_AllowAll(access_auth_table, ip_addr, subnet_bits);
    } else {
      status = ADF_Allow(access_auth_table, ip_addr, subnet_bits);
    }
  } else {
    if (all) {
      status = ADF_DenyAll(access_auth_table, ip_addr, subnet_bits);
    } else {
      status = ADF_Deny(access_auth_table, ip_addr, subnet_bits);
    }
  }

  if (status != ADF_SUCCESS)
    return 0;

  /* Keep server sockets open only when an address allowed */
  if (allow) {
    NTP_Remote_Address remote_addr;

    if (server_sock_fd4 == INVALID_SOCK_FD &&
        ADF_IsAnyAllowed(access_auth_table, IPADDR_INET4)) {
      remote_addr.ip_addr.family = IPADDR_INET4;
      server_sock_fd4 = NIO_OpenServerSocket(&remote_addr);
    }
    if (server_sock_fd6 == INVALID_SOCK_FD &&
        ADF_IsAnyAllowed(access_auth_table, IPADDR_INET6)) {
      remote_addr.ip_addr.family = IPADDR_INET6;
      server_sock_fd6 = NIO_OpenServerSocket(&remote_addr);
    }
  } else {
    if (server_sock_fd4 != INVALID_SOCK_FD &&
        !ADF_IsAnyAllowed(access_auth_table, IPADDR_INET4)) {
      NIO_CloseServerSocket(server_sock_fd4);
      server_sock_fd4 = INVALID_SOCK_FD;
    }
    if (server_sock_fd6 != INVALID_SOCK_FD &&
        !ADF_IsAnyAllowed(access_auth_table, IPADDR_INET6)) {
      NIO_CloseServerSocket(server_sock_fd6);
      server_sock_fd6 = INVALID_SOCK_FD;
    }
  }

  return 1;
}

/* ================================================== */

int
NCR_CheckAccessRestriction(IPAddr *ip_addr)
{
  return ADF_IsAllowed(access_auth_table, ip_addr);
}

/* ================================================== */

void
NCR_IncrementActivityCounters(NCR_Instance inst, int *online, int *offline,
                              int *burst_online, int *burst_offline)
{
  switch (inst->opmode) {
    case MD_BURST_WAS_OFFLINE:
      ++*burst_offline;
      break;
    case MD_BURST_WAS_ONLINE:
      ++*burst_online;
      break;
    case MD_ONLINE:
      ++*online;
      break;
    case MD_OFFLINE:
      ++*offline;
      break;
    default:
      assert(0);
      break;
  }
}

/* ================================================== */

NTP_Remote_Address *
NCR_GetRemoteAddress(NCR_Instance inst) 
{
  return &inst->remote_addr;
}

/* ================================================== */

uint32_t
NCR_GetLocalRefid(NCR_Instance inst)
{
  return UTI_IPToRefid(&inst->local_addr.ip_addr);
}

/* ================================================== */

int NCR_IsSyncPeer(NCR_Instance inst)
{
  return SRC_IsSyncPeer(inst->source);
}

/* ================================================== */

static void
broadcast_timeout(void *arg)
{
  BroadcastDestination *destination;
  NTP_int64 orig_ts;
  NTP_Local_Timestamp recv_ts;

  destination = ARR_GetElement(broadcasts, (long)arg);

  UTI_ZeroNtp64(&orig_ts);
  UTI_ZeroTimespec(&recv_ts.ts);
  recv_ts.source = NTP_TS_DAEMON;
  recv_ts.err = 0.0;

  transmit_packet(MODE_BROADCAST, 0, log(destination->interval) / log(2.0) + 0.5,
                  NTP_VERSION, 0, 0, &orig_ts, &orig_ts, &recv_ts, NULL, NULL, NULL,
                  &destination->addr, &destination->local_addr);

  /* Requeue timeout.  We don't care if interval drifts gradually. */
  SCH_AddTimeoutInClass(destination->interval, SAMPLING_SEPARATION, SAMPLING_RANDOMNESS,
                        SCH_NtpBroadcastClass, broadcast_timeout, arg);
}

/* ================================================== */

void
NCR_AddBroadcastDestination(IPAddr *addr, unsigned short port, int interval)
{
  BroadcastDestination *destination;

  destination = (BroadcastDestination *)ARR_GetNewElement(broadcasts);

  destination->addr.ip_addr = *addr;
  destination->addr.port = port;
  destination->local_addr.ip_addr.family = IPADDR_UNSPEC;
  destination->local_addr.sock_fd = NIO_OpenServerSocket(&destination->addr);
  destination->interval = CLAMP(1 << MIN_POLL, interval, 1 << MAX_POLL);

  SCH_AddTimeoutInClass(destination->interval, SAMPLING_SEPARATION, SAMPLING_RANDOMNESS,
                        SCH_NtpBroadcastClass, broadcast_timeout,
                        (void *)(long)(ARR_GetSize(broadcasts) - 1));
}
