/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2003
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

  Processing to perform the equivalent of what ntpdate does.  That is,
  make a rapid-fire set of measurements to a designated set of
  sources, and step or slew the local clock to bring it into line with
  the result.

  This is kept completely separate of the main chronyd processing, by
  using a separate socket for sending/receiving the measurement
  packets.  That way, ntp_core.c can be kept completely independent of
  this functionality.
  
  A few of the finer points of how to construct valid RFC1305 packets
  and validate responses for this case have been cribbed from the
  ntpdate source.

  */

#include "config.h"

#include "sysincl.h"

#include "acquire.h"
#include "memory.h"
#include "sched.h"
#include "local.h"
#include "logging.h"
#include "ntp.h"
#include "util.h"
#include "main.h"
#include "conf.h"

/* ================================================== */

/* Interval between firing off the first sample to successive sources */
#define INTER_SOURCE_START (0.2)

#define MAX_SAMPLES 8

#define MAX_DEAD_PROBES 4
#define N_GOOD_SAMPLES 4

#define RETRANSMISSION_TIMEOUT (1.0)

#define NTP_VERSION 3
#define NTP_MAX_COMPAT_VERSION 4
#define NTP_MIN_COMPAT_VERSION 2

typedef struct {
  IPAddr ip_addr;               /* Address of the server */
  int sanity;                   /* Flag indicating whether source
                                   looks sane or not */
  int n_dead_probes;            /* Number of probes sent to the server
                                   since a good one */
  int n_samples;                /* Number of samples accumulated */
  int n_total_samples;          /* Total number of samples received
                                   including useless ones */
  double offsets[MAX_SAMPLES];  /* In seconds, positive means local
                                   clock is fast of reference */
  double root_distances[MAX_SAMPLES]; /* in seconds */
  double inter_lo;              /* Low end of estimated range of offset */
  double inter_hi;              /* High end of estimated range of offset */

  NTP_int64 last_tx;            /* Transmit timestamp in last packet
                                   transmitted to source. */

  int timer_running;
  SCH_TimeoutID timeout_id;
} SourceRecord;

static SourceRecord *sources;
static int n_sources;
static int n_started_sources;
static int n_completed_sources;

static double init_slew_threshold;

union sockaddr_in46 {
  struct sockaddr_in in4;
#ifdef HAVE_IPV6
  struct sockaddr_in6 in6;
#endif
  struct sockaddr u;
};

static int sock_fd4 = -1;
#ifdef HAVE_IPV6
static int sock_fd6 = -1;
#endif

/* ================================================== */

static void (*saved_after_hook)(void *) = NULL;
static void *saved_after_hook_anything = NULL;

/* ================================================== */

typedef struct {
  double offset;
  enum {LO, HIGH} type;
  int index;
} Endpoint;

typedef struct {
  double lo;
  double hi;
} Interval;

/* ================================================== */

static void read_from_socket(void *anything);
static void transmit_timeout(void *x);
static void wind_up_acquisition(void);
static void start_source_timeout_handler(void *not_used);

/* ================================================== */

static SCH_TimeoutID source_start_timeout_id;

/* ================================================== */

void
ACQ_Initialise(void)
{
}


/* ================================================== */

void
ACQ_Finalise(void)
{
}

/* ================================================== */

static int
prepare_socket(int family)
{
  unsigned short port_number = CNF_GetAcquisitionPort();
  int sock_fd;
  socklen_t addrlen;

  sock_fd = socket(family, SOCK_DGRAM, 0);

  if (sock_fd < 0) {
    LOG_FATAL(LOGF_Acquire, "Could not open socket : %s", strerror(errno));
  }

  /* Close on exec */
  UTI_FdSetCloexec(sock_fd);

  if (port_number == 0) {
    /* Don't bother binding this socket - we're not fussed what port
       number it gets */
  } else {
    union sockaddr_in46 my_addr;

    memset(&my_addr, 0, sizeof (my_addr));

    switch (family) {
      case AF_INET:
        my_addr.in4.sin_family = family;
        my_addr.in4.sin_port = htons(port_number);
        my_addr.in4.sin_addr.s_addr = htonl(INADDR_ANY);
        addrlen = sizeof (my_addr.in4);
        break;
#ifdef HAVE_IPV6
      case AF_INET6:
        my_addr.in6.sin6_family = family;
        my_addr.in6.sin6_port = htons(port_number);
        my_addr.in6.sin6_addr = in6addr_any;
        addrlen = sizeof (my_addr.in6);
        break;
#endif
      default:
        assert(0);
    }

    if (bind(sock_fd, &my_addr.u, addrlen) < 0) {
      LOG(LOGS_ERR, LOGF_Acquire, "Could not bind socket : %s", strerror(errno));
      /* but keep running */
    }
  }

  SCH_AddInputFileHandler(sock_fd, read_from_socket, (void *)(long)sock_fd);

  return sock_fd;
}
/* ================================================== */

static void
initialise_io(int family)
{
  if (family == IPADDR_INET4 || family == IPADDR_UNSPEC)
    sock_fd4 = prepare_socket(AF_INET);
#ifdef HAVE_IPV6
  if (family == IPADDR_INET6 || family == IPADDR_UNSPEC)
    sock_fd6 = prepare_socket(AF_INET6);
#endif
}

/* ================================================== */

static void
finalise_io(void)
{
  if (sock_fd4 >= 0) {
    SCH_RemoveInputFileHandler(sock_fd4);
    close(sock_fd4);
  }
  sock_fd4 = -1;
#ifdef HAVE_IPV6
  if (sock_fd6 >= 0) {
    SCH_RemoveInputFileHandler(sock_fd6);
    close(sock_fd6);
  }
  sock_fd6 = -1;
#endif
}

/* ================================================== */

static void
probe_source(SourceRecord *src)
{
  NTP_Packet pkt;
  int version = NTP_VERSION;
  NTP_Mode my_mode = MODE_CLIENT;
  struct timeval cooked;
  union sockaddr_in46 his_addr;
  int sock_fd;
  socklen_t addrlen;
  uint32_t ts_fuzz;

#if 0
  printf("Sending probe to %s sent=%d samples=%d\n", UTI_IPToString(&src->ip_addr), src->n_probes_sent, src->n_samples);
#endif

  pkt.lvm = (((LEAP_Unsynchronised << 6) & 0xc0) |
             ((version << 3) & 0x38) |
             ((my_mode) & 0x7));

  pkt.stratum = 0;
  pkt.poll = 4;
  pkt.precision = -6; /* as ntpdate */
  pkt.root_delay = UTI_DoubleToInt32(1.0); /* 1 second */
  pkt.root_dispersion = UTI_DoubleToInt32(1.0); /* likewise */
  pkt.reference_id = 0;
  pkt.reference_ts.hi = 0; /* Set to 0 */
  pkt.reference_ts.lo = 0; /* Set to 0 */
  pkt.originate_ts.hi = 0; /* Set to 0 */
  pkt.originate_ts.lo = 0; /* Set to 0 */
  pkt.receive_ts.hi = 0;   /* Set to 0 */
  pkt.receive_ts.lo = 0;   /* Set to 0 */

  /* And do transmission */

  memset(&his_addr, 0, sizeof (his_addr));
  switch (src->ip_addr.family) {
    case IPADDR_INET4:
      his_addr.in4.sin_addr.s_addr = htonl(src->ip_addr.addr.in4);
      his_addr.in4.sin_port = htons(123); /* Fixed for now */
      his_addr.in4.sin_family = AF_INET;
      addrlen = sizeof (his_addr.in4);
      sock_fd = sock_fd4;
      break;
#ifdef HAVE_IPV6
    case IPADDR_INET6:
      memcpy(&his_addr.in6.sin6_addr.s6_addr, &src->ip_addr.addr.in6,
          sizeof (his_addr.in6.sin6_addr.s6_addr));
      his_addr.in6.sin6_port = htons(123); /* Fixed for now */
      his_addr.in6.sin6_family = AF_INET6;
      addrlen = sizeof (his_addr.in6);
      sock_fd = sock_fd6;
      break;
#endif
    default:
      assert(0);
  }


  ts_fuzz = UTI_GetNTPTsFuzz(LCL_GetSysPrecisionAsLog());
  LCL_ReadCookedTime(&cooked, NULL);
  UTI_TimevalToInt64(&cooked, &pkt.transmit_ts, ts_fuzz);

  if (sendto(sock_fd, (void *) &pkt, NTP_NORMAL_PACKET_SIZE,
             0,
             &his_addr.u, addrlen) < 0) {
    LOG(LOGS_WARN, LOGF_Acquire, "Could not send to %s : %s",
        UTI_IPToString(&src->ip_addr),
        strerror(errno));
  }

  src->last_tx = pkt.transmit_ts;

  ++(src->n_dead_probes);
  src->timer_running = 1;
  src->timeout_id = SCH_AddTimeoutByDelay(RETRANSMISSION_TIMEOUT, transmit_timeout, (void *) src);
}

/* ================================================== */

static void
transmit_timeout(void *x)
{
  SourceRecord *src = (SourceRecord *) x;

  src->timer_running = 0;

#if 0
  printf("Timeout expired for server %s\n", UTI_IPToString(&src->ip_addr));
#endif

  if (src->n_dead_probes < MAX_DEAD_PROBES) {
    probe_source(src);
  } else {
    /* Source has croaked or is taking too long to respond */
    ++n_completed_sources;
    if (n_completed_sources == n_sources) {
      wind_up_acquisition();
    }
  }
}

/* ================================================== */

#define MAX_STRATUM 15

static void
process_receive(NTP_Packet *msg, SourceRecord *src, struct timeval *now)
{

  unsigned long lvm;
  int leap, version, mode;
  double root_delay, root_dispersion;
  double total_root_delay, total_root_dispersion, total_root_distance;

  struct timeval local_orig, local_average, remote_rx, remote_tx, remote_average;
  double remote_interval, local_interval;
  double delta, theta, epsilon;
  int n;
  
  /* Most of the checks are from ntpdate */

  /* Need to do something about authentication */

  lvm = msg->lvm;
  leap = (lvm >> 6) & 0x3;
  version = (lvm >> 3) & 0x7;
  mode = lvm & 0x7;

  if ((leap == LEAP_Unsynchronised) ||
      (version  < NTP_MIN_COMPAT_VERSION || version > NTP_MAX_COMPAT_VERSION) ||
      (mode != MODE_SERVER && mode != MODE_PASSIVE)) {
    return;
  }

  if (msg->stratum > MAX_STRATUM) {
    return;
  }

  /* Check whether server is responding to our last request */
  if ((msg->originate_ts.hi != src->last_tx.hi) ||
      (msg->originate_ts.lo != src->last_tx.lo)) {
    return;
  }

  /* Check that the server is sane */
  if (((msg->originate_ts.hi == 0) && (msg->originate_ts.lo == 0)) ||
      ((msg->receive_ts.hi == 0) && (msg->receive_ts.lo) == 0)) {
    return;
  }

  root_delay = UTI_Int32ToDouble(msg->root_delay);
  root_dispersion = UTI_Int32ToDouble(msg->root_dispersion);

  UTI_Int64ToTimeval(&src->last_tx, &local_orig);
  UTI_Int64ToTimeval(&msg->receive_ts, &remote_rx);
  UTI_Int64ToTimeval(&msg->transmit_ts, &remote_tx);
  UTI_AverageDiffTimevals(&remote_rx, &remote_tx, &remote_average, &remote_interval);
  UTI_AverageDiffTimevals(&local_orig, now, &local_average, &local_interval);

  delta = local_interval - remote_interval;

  /* Defined as positive if we are fast.  Note this sign convention is
     opposite to that used in ntp_core.c */

  UTI_DiffTimevalsToDouble(&theta, &local_average, &remote_average);
  
  /* Could work out epsilon - leave till later */
  epsilon = 0.0;

  total_root_delay = fabs(delta) + root_delay;
  total_root_dispersion = epsilon + root_dispersion;
  total_root_distance = 0.5 * fabs(total_root_delay) + total_root_dispersion;

  n = src->n_samples;
#if 0
  printf("Sample %d theta=%.6f delta=%.6f root_del=%.6f root_disp=%.6f root_dist=%.6f\n",
         n, theta, delta, total_root_delay, total_root_dispersion, total_root_distance);
#endif
  src->offsets[n] = theta;
  src->root_distances[n] = total_root_distance;
  ++(src->n_samples);

}

/* ================================================== */

static void
read_from_socket(void *anything)
{
  int status;
  ReceiveBuffer msg;
  union sockaddr_in46 his_addr;
  int sock_fd;
  socklen_t his_addr_len;
  int flags;
  int message_length;
  IPAddr remote_ip;
  int i, ok;
  struct timeval now;
  SourceRecord *src;

  flags = 0;
  message_length = sizeof(msg);
  his_addr_len = sizeof(his_addr);

  /* Get timestamp */
  SCH_GetFileReadyTime(&now, NULL);

  sock_fd = (long)anything;
  status = recvfrom (sock_fd, (char *)&msg, message_length, flags,
                     &his_addr.u, &his_addr_len);

  if (status < 0) {
    LOG(LOGS_WARN, LOGF_Acquire, "Error reading from socket, %s", strerror(errno));
    return;
  }
  
  switch (his_addr.u.sa_family) {
    case AF_INET:
      remote_ip.family = IPADDR_INET4;
      remote_ip.addr.in4 = ntohl(his_addr.in4.sin_addr.s_addr);
      break;
#ifdef HAVE_IPV6
    case AF_INET6:
      remote_ip.family = IPADDR_INET6;
      memcpy(&remote_ip.addr.in6, his_addr.in6.sin6_addr.s6_addr,
          sizeof (remote_ip.addr.in6));
      break;
#endif
    default:
      assert(0);
  }

#if 0
  printf("Got message from %s\n", UTI_IPToString(&remote_ip));
#endif
  
  /* Find matching host */
  ok = 0;
  for (i=0; i<n_sources; i++) {
    if (UTI_CompareIPs(&remote_ip, &sources[i].ip_addr, NULL) == 0) {
      ok = 1;
      break;
    }
  }

  if (ok) {

    src = sources + i;
    ++src->n_total_samples;

    src->n_dead_probes = 0; /* reset this when we actually receive something */

    /* If we got into this function, we know the retransmission timeout has not
       expired for the source */
    if (src->timer_running) {
      SCH_RemoveTimeout(src->timeout_id);
      src->timer_running = 0;
    }

    process_receive(&msg.ntp_pkt, src, &now);

    /* Check if server done and requeue timeout */
    if ((src->n_samples >= N_GOOD_SAMPLES) ||
        (src->n_total_samples >= MAX_SAMPLES)) {
      ++n_completed_sources;
#if 0
      printf("Source %s completed\n", UTI_IPToString(&src->ip_addr));
#endif
      if (n_completed_sources == n_sources) {
        wind_up_acquisition();
      }
    } else {
      
      /* Send the next probe */
      probe_source(src);

    }
  }

}

/* ================================================== */

static void
start_next_source(void)
{
  probe_source(sources + n_started_sources);
#if 0
  printf("Trying to start source %s\n", UTI_IPToString(&sources[n_started_sources].ip_addr));
#endif
  n_started_sources++;
  
  if (n_started_sources < n_sources) {
    source_start_timeout_id = SCH_AddTimeoutByDelay(INTER_SOURCE_START, start_source_timeout_handler, NULL);
  }
}

/* ================================================== */

static int
endpoint_compare(const void *a, const void *b)
{
  const Endpoint *aa = (const Endpoint *) a;
  const Endpoint *bb = (const Endpoint *) b;

  if (aa->offset < bb->offset) {
    return -1;
  } else if (aa->offset > bb->offset) {
    return +1;
  } else {
    return 0;
  }
}

/* ================================================== */

static void
process_measurements(void)
{

  SourceRecord *s;
  Endpoint *eps;
  int i, j;
  int n_sane_sources;
  double lo, hi;
  double inter_lo, inter_hi;

  int depth;
  int best_depth;
  int n_at_best_depth;
  Interval *intervals;
  double estimated_offset;
  int index1, index2;

  n_sane_sources = 0;

  /* First, get a consistent interval for each source.  Those for
     which this is not possible are considered to be insane. */

  for (i=0; i<n_sources; i++) {
    s = sources + i;
    /* If we got no measurements, the source is insane */
    if (s->n_samples == 0) {
      s->sanity = 0;
    } else {
      s->sanity = 1; /* so far ... */
      lo = s->offsets[0] - s->root_distances[0];
      hi = s->offsets[0] + s->root_distances[0];
      inter_lo = lo;
      inter_hi = hi;
      for (j=1; j<s->n_samples; j++) {
        lo = s->offsets[j] - s->root_distances[j];
        hi = s->offsets[j] + s->root_distances[j];
        if ((inter_hi <= lo) || (inter_lo >= hi)) {
          /* Oh dear, we won't get an interval for this source */
          s->sanity = 0;
          break;
        } else {
          inter_lo = (lo < inter_lo) ? inter_lo : lo;
          inter_hi = (hi > inter_hi) ? inter_hi : hi;
        }
      }
      if (s->sanity) {
        s->inter_lo = inter_lo;
        s->inter_hi = inter_hi;
      }
    }

    if (s->sanity) {
      ++n_sane_sources;
    }

  }

  /* Now build the endpoint list, similar to the RFC1305 clock
     selection algorithm. */
  eps = MallocArray(Endpoint, 2*n_sane_sources);
  intervals = MallocArray(Interval, n_sane_sources);

  j = 0;
  for (i=0; i<n_sources; i++) {
    s = sources + i;
    if (s->sanity) {
      eps[j].offset = s->inter_lo;
      eps[j].type = LO;
      eps[j].index = i;
      eps[j+1].offset = s->inter_hi;
      eps[j+1].type = HIGH;
      eps[j+1].index = i;
      j += 2;
    }
  }

  qsort(eps, 2*n_sane_sources, sizeof(Endpoint), endpoint_compare);

  /* Now do depth searching algorithm */
  n_at_best_depth = best_depth = depth = 0;
  for (i=0; i<2*n_sane_sources; i++) {

#if 0
    fprintf(stderr, "Endpoint type %s source index %d [ip=%s] offset=%.6f\n",
            (eps[i].type == LO) ? "LO" : "HIGH",
            eps[i].index,
            UTI_IPToString(&sources[eps[i].index].ip_addr),
            eps[i].offset);
#endif

    switch (eps[i].type) {
      case LO:
        depth++;
        if (depth > best_depth) {
          best_depth = depth;
          n_at_best_depth = 0;
          intervals[0].lo = eps[i].offset;
        } else if (depth == best_depth) {
          intervals[n_at_best_depth].lo = eps[i].offset;
        } else {
          /* Nothing to do */
        }

        break;

      case HIGH:
        if (depth == best_depth) {
          intervals[n_at_best_depth].hi = eps[i].offset;
          n_at_best_depth++;
        }

        depth--;

        break;

    }
  }

  if (best_depth > 0) {
    if ((n_at_best_depth % 2) == 1) {
      index1 = (n_at_best_depth - 1) / 2;
      estimated_offset = 0.5 * (intervals[index1].lo + intervals[index1].hi);
    } else {
      index2 = (n_at_best_depth / 2);
      index1 = index2 - 1;
      estimated_offset = 0.5 * (intervals[index1].lo + intervals[index2].hi);
    }


    /* Apply a step change to the system clock.  As per sign
       convention in local.c and its children, a positive offset means
       the system clock is fast of the reference, i.e. it needs to be
       stepped backwards. */

    if (fabs(estimated_offset) > init_slew_threshold) {
      LOG(LOGS_INFO, LOGF_Acquire, "System's initial offset : %.6f seconds %s of true (step)",
          fabs(estimated_offset),
          (estimated_offset >= 0) ? "fast" : "slow");
      LCL_ApplyStepOffset(estimated_offset);
    } else {
      LOG(LOGS_INFO, LOGF_Acquire, "System's initial offset : %.6f seconds %s of true (slew)",
          fabs(estimated_offset),
          (estimated_offset >= 0) ? "fast" : "slow");
      LCL_AccumulateOffset(estimated_offset, 0.0);
    }

  } else {
    LOG(LOGS_WARN, LOGF_Acquire, "No intersecting endpoints found");
  }  

  Free(intervals);
  Free(eps);
        
}

/* ================================================== */

static void
wind_up_acquisition(void)
{

  /* Now process measurements */
  process_measurements();

  Free(sources);

  finalise_io();

  if (saved_after_hook) {
    (saved_after_hook)(saved_after_hook_anything);
  }

}

/* ================================================== */

static void
start_source_timeout_handler(void *not_used)
{

  start_next_source();
}

/* ================================================== */

void
ACQ_StartAcquisition(int n, IPAddr *ip_addrs, double threshold, void (*after_hook)(void *), void *anything)
{

  int i, ip4, ip6;
  int k, duplicate_ip;

  saved_after_hook = after_hook;
  saved_after_hook_anything = anything;

  init_slew_threshold = threshold;

  n_started_sources = 0;
  n_completed_sources = 0;
  n_sources = 0;
  sources = MallocArray(SourceRecord, n);

  for (i = ip4 = ip6 = 0; i < n; i++) {
    /* check for duplicate IP addresses and ignore them */
    duplicate_ip = 0;
    for (k = 0; k < i; k++) {
      duplicate_ip |= UTI_CompareIPs(&(sources[k].ip_addr),
                                     &ip_addrs[i],
                                     NULL) == 0;
    }
    if (!duplicate_ip) {
      sources[n_sources].ip_addr = ip_addrs[i];
      sources[n_sources].n_samples = 0;
      sources[n_sources].n_total_samples = 0;
      sources[n_sources].n_dead_probes = 0;
      if (ip_addrs[i].family == IPADDR_INET4)
        ip4++;
      else if (ip_addrs[i].family == IPADDR_INET6)
        ip6++;
      n_sources++;
    } else {
      LOG(LOGS_WARN, LOGF_Acquire, "Ignoring duplicate source: %s",
          UTI_IPToString(&ip_addrs[i]));
    }
  }

  initialise_io((ip4 && ip6) ? IPADDR_UNSPEC : (ip4 ? IPADDR_INET4 : IPADDR_INET6));

  /* Start sampling first source */
  start_next_source();
}

/* ================================================== */
