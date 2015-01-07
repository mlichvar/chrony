/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2003
 * Copyright (C) Miroslav Lichvar  2009-2014
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
#include "memory.h"
#include "sched.h"
#include "reference.h"
#include "local.h"
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
/* Structure used for holding a single peer/server's
   protocol machine */

struct NCR_Instance_Record {
  NTP_Remote_Address remote_addr; /* Needed for routing transmit packets */
  NTP_Local_Address local_addr; /* Local address/socket used to send packets */
  NTP_Mode mode;                /* The source's NTP mode
                                   (client/server or symmetric active peer) */
  OperatingMode opmode;         /* Whether we are sampling this source
                                   or not and in what way */
  int timer_running;            /* Boolean indicating whether we have a timeout
                                   pending to transmit to the source */
  SCH_TimeoutID timeout_id;     /* Scheduler's timeout ID, if we are
                                   running on a timer. */
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

  int do_auth;                  /* Flag indicating whether we
                                   authenticate packets we send to
                                   this machine (if it's serving us or
                                   the association is symmetric). Note
                                   : we don't authenticate if we can't
                                   find the key in our database. */
  uint32_t auth_key_id;          /* The ID of the authentication key to
                                   use. */

  /* Count of how many packets we have transmitted since last successful
     receive from this peer */
  int tx_count;

  /* Timestamp in tx field of last received packet.  We have to
     reproduce this exactly as the orig field or our outgoing
     packet. */
  NTP_int64 remote_orig;     

  /* Local timestamp when the last packet was received from the
     source.  We have to be prepared to tinker with this if the local
     clock has its frequency adjusted before we repond.  The value we
     store here is what our own local time was when the same arrived.
     Before replying, we have to correct this to fit with the
     parameters for the current reference.  (It must be stored
     relative to local time to permit frequency and offset adjustments
     to be made when we trim the local clock). */
  struct timeval local_rx;

  /* Local timestamp when we last transmitted a packet to the source.
     We store two versions.  The first is in NTP format, and is used
     to validate the next received packet from the source.
     Additionally, this is corrected to bring it into line with the
     current reference.  The second is in timeval format, and is kept
     relative to the local clock.  We modify this in accordance with
     local clock frequency/offset changes, and use this for computing
     statistics about the source when a return packet arrives. */
  NTP_int64 local_ntp_tx;
  struct timeval local_tx;

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

/* Minimum allowed poll interval */
#define MIN_POLL 0

/* Kiss-o'-Death codes */
#define KOD_RATE 0x52415445UL /* RATE */

/* Maximum poll interval set by KoD RATE */
#define MAX_KOD_RATE_POLL SRC_DEFAULT_MAXPOLL

#define INVALID_SOCK_FD -1

/* ================================================== */

/* Server IPv4/IPv6 sockets */
static int server_sock_fd4;
static int server_sock_fd6;

static ADF_AuthTable access_auth_table;

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
  struct timeval now;
  time_t warning_advance = 3600 * 24 * 365 * 10; /* 10 years */

#ifdef HAVE_LONG_TIME_T
  /* Check that time before NTP_ERA_SPLIT underflows correctly */

  struct timeval tv1 = {NTP_ERA_SPLIT, 1}, tv2 = {NTP_ERA_SPLIT - 1, 1};
  NTP_int64 ntv1, ntv2;
  int r;

  UTI_TimevalToInt64(&tv1, &ntv1, 0);
  UTI_TimevalToInt64(&tv2, &ntv2, 0);
  UTI_Int64ToTimeval(&ntv1, &tv1);
  UTI_Int64ToTimeval(&ntv2, &tv2);

  r = tv1.tv_sec == NTP_ERA_SPLIT &&
      tv1.tv_sec + (1ULL << 32) - 1 == tv2.tv_sec;

  assert(r);

  LCL_ReadRawTime(&now);
  if (tv2.tv_sec - now.tv_sec < warning_advance)
    LOG(LOGS_WARN, LOGF_NtpCore, "Assumed NTP time ends at %s!",
        UTI_TimeToLogForm(tv2.tv_sec));
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
      "   Date (UTC) Time     IP Address   L St 123 567 ABCD  LP RP Score Offset     Peer del. Peer disp. Root del.  Root disp.")
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
    assert(!inst->timer_running);
    return;
  }

  /* Stop old timer if running */
  if (inst->timer_running)
    SCH_RemoveTimeout(inst->timeout_id);

  /* Start new timer for transmission */
  inst->timeout_id = SCH_AddTimeoutInClass(delay, SAMPLING_SEPARATION,
                                           SAMPLING_RANDOMNESS,
                                           SCH_NtpSamplingClass,
                                           transmit_timeout, (void *)inst);
  inst->timer_running = 1;
}

/* ================================================== */

static void
start_initial_timeout(NCR_Instance inst)
{
  if (!inst->timer_running) {
    /* This will be the first transmission after mode change */

    /* Mark source active */
    SRC_SetActive(inst->source);
  }

  restart_timeout(inst, INITIAL_DELAY);
}

/* ================================================== */

static void
close_client_socket(NCR_Instance inst)
{
  if (inst->mode == MODE_CLIENT && inst->local_addr.sock_fd != INVALID_SOCK_FD) {
    NIO_CloseClientSocket(inst->local_addr.sock_fd);
    inst->local_addr.sock_fd = INVALID_SOCK_FD;
  }
}

/* ================================================== */

static void
take_offline(NCR_Instance inst)
{
  inst->opmode = MD_OFFLINE;
  if (inst->timer_running) {
    SCH_RemoveTimeout(inst->timeout_id);
    inst->timer_running = 0;
  }

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

  result->minpoll = params->minpoll;
  if (result->minpoll < MIN_POLL)
    result->minpoll = SRC_DEFAULT_MINPOLL;
  result->maxpoll = params->maxpoll;
  if (result->maxpoll < MIN_POLL)
    result->maxpoll = SRC_DEFAULT_MAXPOLL;
  if (result->maxpoll < result->minpoll)
    result->maxpoll = result->minpoll;

  result->min_stratum = params->min_stratum;
  if (result->min_stratum >= NTP_MAX_STRATUM)
    result->min_stratum = NTP_MAX_STRATUM - 1;
  result->presend_minpoll = params->presend_minpoll;

  result->max_delay = params->max_delay;
  result->max_delay_ratio = params->max_delay_ratio;
  result->max_delay_dev_ratio = params->max_delay_dev_ratio;
  result->auto_offline = params->auto_offline;
  result->poll_target = params->poll_target;

  result->version = params->version;
  if (result->version < NTP_MIN_COMPAT_VERSION)
    result->version = NTP_MIN_COMPAT_VERSION;
  else if (result->version > NTP_VERSION)
    result->version = NTP_VERSION;

  if (params->authkey == INACTIVE_AUTHKEY) {
    result->do_auth = 0;
    result->auth_key_id = 0;
  } else {
    result->do_auth = 1;
    result->auth_key_id = params->authkey;
    if (!KEY_KeyKnown(result->auth_key_id)) {
      LOG(LOGS_WARN, LOGF_NtpCore, "Source %s added with unknown key %"PRIu32,
          UTI_IPToString(&result->remote_addr.ip_addr), result->auth_key_id);
    }
  }

  /* Create a source instance for this NTP source */
  result->source = SRC_CreateNewInstance(UTI_IPToRefid(&remote_addr->ip_addr),
                                         SRC_NTP, params->sel_option,
                                         &result->remote_addr.ip_addr,
                                         params->min_samples, params->max_samples);

  result->timer_running = 0;
  result->timeout_id = 0;
  result->tx_suspended = 1;
  result->opmode = params->online ? MD_ONLINE : MD_OFFLINE;
  result->local_poll = result->minpoll;
  
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

  instance->poll_score = 0.0;
  instance->remote_poll = 0;
  instance->remote_stratum = 0;

  instance->remote_orig.hi = 0;
  instance->remote_orig.lo = 0;
  instance->local_rx.tv_sec = 0;
  instance->local_rx.tv_usec = 0;
  instance->local_tx.tv_sec = 0;
  instance->local_tx.tv_usec = 0;
  instance->local_ntp_tx.hi = 0;
  instance->local_ntp_tx.lo = 0;

  if (instance->local_poll != instance->minpoll) {
    instance->local_poll = instance->minpoll;

    /* The timer was set with a longer poll interval, restart it */
    if (instance->timer_running)
      restart_timeout(instance, get_transmit_delay(instance, 0, 0.0));
  }
}

/* ================================================== */

void
NCR_ChangeRemoteAddress(NCR_Instance inst, NTP_Remote_Address *remote_addr)
{
  inst->remote_addr = *remote_addr;
  inst->tx_count = 0;
  inst->presend_done = 0;

  if (inst->mode == MODE_CLIENT)
    close_client_socket(inst);
  else {
    NIO_CloseServerSocket(inst->local_addr.sock_fd);
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

static int
transmit_packet(NTP_Mode my_mode, /* The mode this machine wants to be */
                int my_poll, /* The log2 of the local poll interval */
                int version, /* The NTP version to be set in the packet */
                int do_auth, /* Boolean indicating whether to authenticate the packet or not */
                uint32_t key_id, /* The authentication key ID */
                NTP_int64 *orig_ts, /* Originate timestamp (from received packet) */
                struct timeval *local_rx, /* Local time request packet was received */
                struct timeval *local_tx, /* RESULT : Time this reply
                                             is sent as local time, or
                                             NULL if don't want to
                                             know */
                NTP_int64 *local_ntp_tx, /* RESULT : Time reply sent
                                            as NTP timestamp
                                            (including adjustment to
                                            reference), ignored if
                                            NULL */
                NTP_Remote_Address *where_to, /* Where to address the reponse to */
                NTP_Local_Address *from /* From what address to send it */
                )
{
  NTP_Packet message;
  int leap, auth_len, length, ret;
  struct timeval local_transmit;

  /* Parameters read from reference module */
  int are_we_synchronised, our_stratum;
  NTP_Leap leap_status;
  uint32_t our_ref_id, ts_fuzz;
  struct timeval our_ref_time;
  double our_root_delay, our_root_dispersion;

  /* Don't reply with version higher than ours */
  if (version > NTP_VERSION) {
    version = NTP_VERSION;
  }

  /* This is accurate enough and cheaper than calling LCL_ReadCookedTime.
     A more accurate time stamp will be taken later in this function. */
  SCH_GetLastEventTime(&local_transmit, NULL, NULL);

  REF_GetReferenceParams(&local_transmit,
                         &are_we_synchronised, &leap_status,
                         &our_stratum,
                         &our_ref_id, &our_ref_time,
                         &our_root_delay, &our_root_dispersion);

  if (are_we_synchronised) {
    leap = (int) leap_status;
  } else {
    leap = LEAP_Unsynchronised;
  }

  /* Generate transmit packet */
  message.lvm = NTP_LVM(leap, version, my_mode);
  /* Stratum 16 and larger are invalid */
  if (our_stratum < NTP_MAX_STRATUM) {
    message.stratum = our_stratum;
  } else {
    message.stratum = NTP_INVALID_STRATUM;
  }
 
  message.poll = my_poll;
  message.precision = LCL_GetSysPrecisionAsLog();

  /* If we're sending a client mode packet and we aren't synchronized yet, 
     we might have to set up artificial values for some of these parameters */
  message.root_delay = UTI_DoubleToInt32(our_root_delay);
  message.root_dispersion = UTI_DoubleToInt32(our_root_dispersion);

  message.reference_id = htonl((NTP_int32) our_ref_id);

  /* Now fill in timestamps */
  UTI_TimevalToInt64(&our_ref_time, &message.reference_ts, 0);

  /* Originate - this comes from the last packet the source sent us */
  message.originate_ts = *orig_ts;

  /* Receive - this is when we received the last packet from the source.
     This timestamp will have been adjusted so that it will now look to
     the source like we have been running on our latest estimate of
     frequency all along */
  UTI_TimevalToInt64(local_rx, &message.receive_ts, 0);

  /* Prepare random bits which will be added to the transmit timestamp. */
  ts_fuzz = UTI_GetNTPTsFuzz(message.precision);

  /* Transmit - this our local time right now!  Also, we might need to
     store this for our own use later, next time we receive a message
     from the source we're sending to now. */
  LCL_ReadCookedTime(&local_transmit, NULL);

  length = NTP_NORMAL_PACKET_LENGTH;

  /* Authenticate */
  if (do_auth && key_id) {
    /* Pre-compensate the transmit time by approx. how long it will
       take to generate the authentication data. */
    local_transmit.tv_usec += KEY_GetAuthDelay(key_id);
    UTI_NormaliseTimeval(&local_transmit);
    UTI_TimevalToInt64(&local_transmit, &message.transmit_ts, ts_fuzz);

    auth_len = KEY_GenerateAuth(key_id, (unsigned char *) &message,
        offsetof(NTP_Packet, auth_keyid),
        (unsigned char *)&message.auth_data, sizeof (message.auth_data));
    if (auth_len > 0) {
      message.auth_keyid = htonl(key_id);
      length += sizeof (message.auth_keyid) + auth_len;
    } else {
      DEBUG_LOG(LOGF_NtpCore,
                "Could not generate auth data with key %"PRIu32" to send packet",
                key_id);
      return 0;
    }
  } else {
    if (do_auth) {
      /* Zero key ID means crypto-NAK, append only the ID without any data */
      message.auth_keyid = 0;
      length += sizeof (message.auth_keyid);
    }
    UTI_TimevalToInt64(&local_transmit, &message.transmit_ts, ts_fuzz);
  }

  ret = NIO_SendPacket(&message, where_to, from, length);

  if (local_tx) {
    *local_tx = local_transmit;
  }

  if (local_ntp_tx) {
    *local_ntp_tx = message.transmit_ts;
  }

  return ret;
}

/* ================================================== */
/* Timeout handler for transmitting to a source. */

static void
transmit_timeout(void *arg)
{
  NCR_Instance inst = (NCR_Instance) arg;
  int sent;

  inst->timer_running = 0;

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

  /* Check whether we need to 'warm up' the link to the other end by
     sending an NTP exchange to ensure both ends' ARP caches are
     primed.  On loaded systems this might also help ensure that bits
     of the program are paged in properly before we start. */

  if ((inst->presend_minpoll > 0) &&
      (inst->presend_minpoll <= inst->local_poll) &&
      !inst->presend_done) {
    
    /* Send a client packet, don't store the local tx values
       as the reply will be ignored */
    transmit_packet(MODE_CLIENT, inst->local_poll, inst->version, 0, 0,
                    &inst->remote_orig, &inst->local_rx, NULL, NULL,
                    &inst->remote_addr, &inst->local_addr);

    inst->presend_done = 1;

    /* Requeue timeout */
    restart_timeout(inst, WARM_UP_DELAY);

    return;
  }

  inst->presend_done = 0; /* Reset for next time */

  sent = transmit_packet(inst->mode, inst->local_poll,
                         inst->version,
                         inst->do_auth, inst->auth_key_id,
                         &inst->remote_orig,
                         &inst->local_rx, &inst->local_tx, &inst->local_ntp_tx,
                         &inst->remote_addr,
                         &inst->local_addr);

  ++inst->tx_count;

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
check_packet_auth(NTP_Packet *pkt, int length, int *has_auth, uint32_t *key_id)
{
  int i, remainder, ext_length;
  unsigned char *data;
  uint32_t id;

  /* Go through extension fields and see if there is a valid MAC */

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
        if (key_id)
          *key_id = id;
        if (has_auth)
          *has_auth = 1;
        return 1;
      }
    }

    /* Check if this is a valid field extension.  They consist of 16-bit type,
       16-bit length of the whole field aligned to 32 bits and data. */
    if (remainder >= NTP_MIN_EXTENSION_LENGTH) {
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
     length.  Not a big problem, at worst we won't reply with a crypto-NAK. */
  if (has_auth)
    *has_auth = remainder >= NTP_MIN_MAC_LENGTH;

  return 0;
}

/* ================================================== */

static int
receive_packet(NTP_Packet *message, struct timeval *now, double now_err, NCR_Instance inst, NTP_Local_Address *local_addr, int length)
{
  int pkt_leap;
  uint32_t pkt_refid;
  double pkt_root_delay;
  double pkt_root_dispersion;

  /* The local time to which the (offset, delay, dispersion) triple will
     be taken to relate.  For client/server operation this is practically
     the same as either the transmit or receive time.  The difference comes
     in symmetric active mode, when the receive may come minutes after the
     transmit, and this time will be midway between the two */
  struct timeval sample_time;

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

  /* These are the timeval equivalents of the remote epochs */  
  struct timeval remote_receive_tv, remote_transmit_tv;
  struct timeval remote_reference_tv;
  struct timeval local_average, remote_average;
  double local_interval, remote_interval;

  /* RFC 5905 packet tests */
  int test1, test2, test3, test5, test6, test7;
  int valid_packet;

  /* Additional tests */
  int testA, testB, testC, testD;
  int good_packet;

  /* Kiss-o'-Death codes */
  int kod_rate;

  /* Characters used to print synchronisation status */
  static const char sync_stats[4] = {'N', '+', '-', '?'};

  /* The estimated offset predicted from previous samples.  The
     convention here is that positive means local clock FAST of
     reference, i.e. backwards to the way that 'offset' is defined. */
  double estimated_offset;

  /* The absolute difference between the offset estimate and
     measurement in seconds */
  double error_in_estimate;

  double delay_time, precision;
  int requeue_transmit;

  /* ==================== */

  pkt_leap = NTP_LVM_TO_LEAP(message->lvm);
  pkt_refid = ntohl(message->reference_id);
  pkt_root_delay = UTI_Int32ToDouble(message->root_delay);
  pkt_root_dispersion = UTI_Int32ToDouble(message->root_dispersion);

  UTI_Int64ToTimeval(&message->receive_ts, &remote_receive_tv);
  UTI_Int64ToTimeval(&message->transmit_ts, &remote_transmit_tv);
  UTI_Int64ToTimeval(&message->reference_ts, &remote_reference_tv);

  /* Check if the packet is valid per RFC 5905, section 8.
     The test values are 1 when passed and 0 when failed. */
  
  /* Test 1 checks for duplicate packet */
  test1 = message->transmit_ts.hi != inst->remote_orig.hi ||
          message->transmit_ts.lo != inst->remote_orig.lo;

  /* Test 2 checks for bogus packet.  This ensures the source is responding to
     the latest packet we sent to it. */
  test2 = message->originate_ts.hi == inst->local_ntp_tx.hi &&
          message->originate_ts.lo == inst->local_ntp_tx.lo;
  
  /* Test 3 checks for invalid timestamps.  This can happen when the
     association if not properly 'up'. */
  test3 = (message->originate_ts.hi || message->originate_ts.lo) &&
          (message->receive_ts.hi || message->receive_ts.lo) &&
          (message->transmit_ts.hi || message->transmit_ts.lo);

  /* Test 4 would check for denied access.  It would always pass as this
     function is called only for known sources. */

  /* Test 5 checks for authentication failure.  If we expect authenticated info
     from this peer/server and the packet doesn't have it or the authentication
     is bad, it's got to fail.  If the peer or server sends us an authenticated
     frame, but we're not bothered about whether he authenticates or not, just
     ignore the test. */
  test5 = inst->do_auth ? check_packet_auth(message, length, NULL, NULL) : 1;

  /* Test 6 checks for unsynchronised server */
  test6 = pkt_leap != LEAP_Unsynchronised &&
          message->stratum < NTP_MAX_STRATUM &&
          message->stratum != NTP_INVALID_STRATUM; 

  /* Test 7 checks for bad data.  The root distance must be smaller than a
     defined maximum and the transmit time must not be before the time of
     the last synchronisation update. */
  test7 = pkt_root_delay / 2.0 + pkt_root_dispersion < NTP_MAX_DISPERSION &&
          UTI_CompareTimevals(&remote_reference_tv, &remote_transmit_tv) < 1;

  /* The packet is considered valid if the tests above passed */
  valid_packet = test1 && test2 && test3 && test5 && test6 && test7;

  /* Check for Kiss-o'-Death codes */
  kod_rate = 0;
  if (test1 && test2 && test5 && pkt_leap == LEAP_Unsynchronised &&
      message->stratum == NTP_INVALID_STRATUM) {
    if (pkt_refid == KOD_RATE)
      kod_rate = 1;
  }

  /* Regardless of any validity checks we apply, we are required to
     save these fields from the packet into the ntp source instance record.
     Note we can't do this assignment before test 1 has been carried out. */
  inst->remote_orig = message->transmit_ts;
  inst->local_rx = *now;

  /* This protects against replay of the last packet we sent */
  if (test2)
    inst->local_ntp_tx.hi = inst->local_ntp_tx.lo = 0;

  if (valid_packet) {
    precision = LCL_GetSysPrecisionAsQuantum();

    SRC_GetFrequencyRange(inst->source, &source_freq_lo, &source_freq_hi);
    
    UTI_AverageDiffTimevals(&remote_receive_tv, &remote_transmit_tv,
                            &remote_average, &remote_interval);

    UTI_AverageDiffTimevals(&inst->local_tx, now,
                            &local_average, &local_interval);

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
    UTI_DiffTimevalsToDouble(&offset, &remote_average, &local_average);
    
    /* We treat the time of the sample as being midway through the local
       measurement period.  An analysis assuming constant relative
       frequency and zero network delay shows this is the only possible
       choice to estimate the frequency difference correctly for every
       sample pair. */
    sample_time = local_average;
    
    /* Calculate skew */
    skew = (source_freq_hi - source_freq_lo) / 2.0;
    
    /* and then calculate peer dispersion */
    dispersion = precision + now_err + skew * fabs(local_interval);
    
    /* Additional tests required to pass before accumulating the sample */

    /* Test A requires that the round trip delay is less than an
       administrator-defined value */ 
    testA = delay <= inst->max_delay;

    /* Test B requires that the ratio of the round trip delay to the
       minimum one currently in the stats data register is less than an
       administrator-defined value */
    testB = inst->max_delay_ratio <= 1.0 ||
            delay / SRC_MinRoundTripDelay(inst->source) > inst->max_delay_ratio;

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
    testD = message->stratum <= 1 || pkt_refid != UTI_IPToRefid(&local_addr->ip_addr);
  } else {
    offset = delay = dispersion = 0.0;
    sample_time = *now;
    testA = testB = testC = testD = 0;
  }
  
  /* The packet is considered good for synchronisation if
     the additional tests passed */
  good_packet = testA && testB && testC && testD;

  root_delay = pkt_root_delay + delay;
  root_dispersion = pkt_root_dispersion + dispersion;
  distance = dispersion + 0.5 * delay;

  DEBUG_LOG(LOGF_NtpCore, "NTP packet lvm=%o stratum=%d poll=%d prec=%d root_delay=%f root_disp=%f refid=%"PRIx32" [%s]",
            message->lvm, message->stratum, message->poll, message->precision,
            pkt_root_delay, pkt_root_dispersion, pkt_refid,
            message->stratum == NTP_INVALID_STRATUM ? UTI_RefidToString(pkt_refid) : "");
  DEBUG_LOG(LOGF_NtpCore, "reference=%s origin=%s receive=%s transmit=%s",
            UTI_TimestampToString(&message->reference_ts),
            UTI_TimestampToString(&message->originate_ts),
            UTI_TimestampToString(&message->receive_ts),
            UTI_TimestampToString(&message->transmit_ts));
  DEBUG_LOG(LOGF_NtpCore, "offset=%f delay=%f dispersion=%f root_delay=%f root_dispersion=%f",
            offset, delay, dispersion, root_delay, root_dispersion);
  DEBUG_LOG(LOGF_NtpCore, "test123=%d%d%d test567=%d%d%d testABCD=%d%d%d%d kod_rate=%d valid=%d good=%d",
            test1, test2, test3, test5, test6, test7, testA, testB, testC, testD,
            kod_rate, valid_packet, good_packet);

  requeue_transmit = 0;

  /* Reduce polling rate if KoD RATE was received */
  if (kod_rate) {
    if (message->poll > inst->minpoll) {
      /* Set our minpoll to message poll, but use a reasonable maximum */
      if (message->poll <= MAX_KOD_RATE_POLL)
        inst->minpoll = message->poll;
      else if (inst->minpoll < MAX_KOD_RATE_POLL)
        inst->minpoll = MAX_KOD_RATE_POLL;

      if (inst->minpoll > inst->maxpoll)
        inst->maxpoll = inst->minpoll;
      if (inst->minpoll > inst->local_poll)
        inst->local_poll = inst->minpoll;

      LOG(LOGS_WARN, LOGF_NtpCore,
          "Received KoD RATE with poll %d from %s, minpoll set to %d",
          message->poll, UTI_IPToString(&inst->remote_addr.ip_addr),
          inst->minpoll);
    }

    /* Stop ongoing burst */
    if (inst->opmode == MD_BURST_WAS_OFFLINE || inst->opmode == MD_BURST_WAS_ONLINE) {
      inst->burst_good_samples_to_go = 0;
      LOG(LOGS_WARN, LOGF_NtpCore, "Received KoD RATE from %s, burst sampling stopped",
          UTI_IPToString(&inst->remote_addr.ip_addr));
    }

    requeue_transmit = 1;
  }

  if (valid_packet) {
    inst->remote_poll = message->poll;
    inst->remote_stratum = message->stratum;
    inst->tx_count = 0;
    SRC_UpdateReachability(inst->source, 1);

    if (good_packet) {
      /* Do this before we accumulate a new sample into the stats registers, obviously */
      estimated_offset = SRC_PredictOffset(inst->source, &sample_time);

      SRC_AccumulateSample(inst->source,
                           &sample_time,
                           offset, delay, dispersion,
                           root_delay, root_dispersion,
                           message->stratum > inst->min_stratum ?
                             message->stratum : inst->min_stratum,
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
    } else {
      /* Slowly increase the polling interval if we can't get good packet */
      adjust_poll(inst, 0.1);
    }

    /* If in client mode, no more packets are expected to be coming from the
       server and the socket can be closed */
    close_client_socket(inst);

    requeue_transmit = 1;
  }

  /* And now, requeue the timer. */
  if (requeue_transmit && inst->opmode != MD_OFFLINE) {
    delay_time = get_transmit_delay(inst, 0, local_interval);

    if (kod_rate) {
      /* Back off for a while */
      delay_time += (double) (4 * (1UL << inst->minpoll));
    }

    /* Get rid of old timeout and start a new one */
    assert(inst->timer_running);
    restart_timeout(inst, delay_time);
  }

  /* Do measurement logging */
  if (logfileid != -1) {
    LOG_FileWrite(logfileid, "%s %-15s %1c %2d %1d%1d%1d %1d%1d%1d %1d%1d%1d%d  %2d %2d %4.2f %10.3e %10.3e %10.3e %10.3e %10.3e",
            UTI_TimeToLogForm(sample_time.tv_sec),
            UTI_IPToString(&inst->remote_addr.ip_addr),
            sync_stats[pkt_leap],
            message->stratum,
            test1, test2, test3, test5, test6, test7, testA, testB, testC, testD,
            inst->local_poll, inst->remote_poll,
            inst->poll_score,
            offset, delay, dispersion,
            pkt_root_delay, pkt_root_dispersion);
  }            

  return valid_packet;
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

   Association mode 0 is implemented in NCR_ProcessUnknown(), other modes
   in NCR_ProcessKnown().

   Broadcast, manycast and ephemeral symmetric passive associations are not
   supported yet.
 */

/* ================================================== */
/* This routine is called when a new packet arrives off the network,
   and it relates to a source we have an ongoing protocol exchange with */

int
NCR_ProcessKnown
(NTP_Packet *message,           /* the received message */
 struct timeval *now,           /* timestamp at time of receipt */
 double now_err,
 NCR_Instance inst,             /* the instance record for this peer/server */
 NTP_Local_Address *local_addr, /* the receiving address */
 int length                     /* the length of the received packet */
 )
{
  int pkt_mode, proc_packet, proc_as_unknown, log_peer_access;

  if (!check_packet_format(message, length))
    return 0;

  pkt_mode = NTP_LVM_TO_MODE(message->lvm);
  proc_packet = 0;
  proc_as_unknown = 0;
  log_peer_access = 0;

  /* Now, depending on the mode we decide what to do */
  switch (pkt_mode) {
    case MODE_ACTIVE:
      switch (inst->mode) {
        case MODE_ACTIVE:
          /* Ordinary symmetric peering */
          log_peer_access = 1;
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
          log_peer_access = 1;
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
      /* Ignore presend reply */
      if (inst->presend_done)
        break;

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

  if (log_peer_access)
    CLG_LogNTPPeerAccess(&inst->remote_addr.ip_addr, now->tv_sec);

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

    return receive_packet(message, now, now_err, inst, local_addr, length);
  } else if (proc_as_unknown) {
    NCR_ProcessUnknown(message, now, now_err, &inst->remote_addr,
                       local_addr, length);
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
NCR_ProcessUnknown
(NTP_Packet *message,           /* the received message */
 struct timeval *now,           /* timestamp at time of receipt */
 double now_err,                /* assumed error in the timestamp */
 NTP_Remote_Address *remote_addr,
 NTP_Local_Address *local_addr,
 int length                     /* the length of the received packet */
 )
{
  NTP_Mode pkt_mode, my_mode;
  int has_auth, valid_auth;
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
      CLG_LogNTPPeerAccess(&remote_addr->ip_addr, now->tv_sec);
      break;
    case MODE_CLIENT:
      /* Reply with server packet */
      my_mode = MODE_SERVER;
      CLG_LogNTPClientAccess(&remote_addr->ip_addr, now->tv_sec);
      break;
    default:
      /* Discard */
      DEBUG_LOG(LOGF_NtpCore, "NTP packet discarded pkt_mode=%d", pkt_mode);
      return;
  }

  /* Check if the packet includes MAC that authenticates properly */
  valid_auth = check_packet_auth(message, length, &has_auth, &key_id);

  /* If authentication failed, reply with crypto-NAK */
  if (!valid_auth)
    key_id = 0;

  /* Send a reply.
     - copy the poll value as the client may use it to control its polling
       interval
     - authenticate the packet if the request was authenticated
     - originate timestamp is the client's transmit time
     - don't save our transmit timestamp as we aren't maintaining state about
       this client */
  transmit_packet(my_mode, message->poll, NTP_LVM_TO_VERSION(message->lvm),
                  has_auth, key_id, &message->transmit_ts, now, NULL, NULL,
                  remote_addr, local_addr);
}

/* ================================================== */

void
NCR_SlewTimes(NCR_Instance inst, struct timeval *when, double dfreq, double doffset)
{
  double delta;

  if (inst->local_rx.tv_sec || inst->local_rx.tv_usec)
    UTI_AdjustTimeval(&inst->local_rx, when, &inst->local_rx, &delta, dfreq, doffset);
  if (inst->local_tx.tv_sec || inst->local_tx.tv_usec)
    UTI_AdjustTimeval(&inst->local_tx, when, &inst->local_tx, &delta, dfreq, doffset);
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
  if (new_minpoll < MIN_POLL)
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
  if (new_maxpoll < MIN_POLL)
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
NCR_ReportSource(NCR_Instance inst, RPT_SourceReport *report, struct timeval *now)
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
  struct timeval recv_ts;

  destination = ARR_GetElement(broadcasts, (long)arg);

  orig_ts.hi = 0;
  orig_ts.lo = 0;
  recv_ts.tv_sec = 0;
  recv_ts.tv_usec = 0;

  transmit_packet(MODE_BROADCAST, 6 /* FIXME: should this be log2(interval)? */,
                  NTP_VERSION, 0, 0, &orig_ts, &recv_ts, NULL, NULL,
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
  destination->interval = interval;

  SCH_AddTimeoutInClass(destination->interval, SAMPLING_SEPARATION, SAMPLING_RANDOMNESS,
                        SCH_NtpBroadcastClass, broadcast_timeout,
                        (void *)(long)(ARR_GetSize(broadcasts) - 1));
}
