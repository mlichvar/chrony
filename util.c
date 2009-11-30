/*
  $Header: /cvs/src/chrony/util.c,v 1.22 2003/09/28 22:21:17 richard Exp $

  =======================================================================

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

  Various utility functions
  */

#include "sysincl.h"

#include "util.h"
#include "md5.h"

/* ================================================== */

INLINE_STATIC void
UTI_TimevalToDouble(struct timeval *a, double *b)
{
  *b = (double)(a->tv_sec) + 1.0e-6 * (double)(a->tv_usec);

}

/* ================================================== */

INLINE_STATIC void
UTI_DoubleToTimeval(double a, struct timeval *b)
{
  long int_part, frac_part;
  int_part = (long)(a);
  frac_part = (long)(0.5 + 1.0e6 * (a - (double)(int_part)));
  b->tv_sec = int_part;
  b->tv_usec = frac_part;
  UTI_NormaliseTimeval(b);
}

/* ================================================== */

INLINE_STATIC int
UTI_CompareTimevals(struct timeval *a, struct timeval *b)
{
  if (a->tv_sec < b->tv_sec) {
    return -1;
  } else if (a->tv_sec > b->tv_sec) {
    return +1;
  } else {
    if (a->tv_usec < b->tv_usec) {
      return -1;
    } else if (a->tv_usec > b->tv_usec) {
      return +1;
    } else {
      return 0;
    }
  }
}

/* ================================================== */

INLINE_STATIC void
UTI_NormaliseTimeval(struct timeval *x)
{
  /* Reduce tv_usec to within +-1000000 of zero. JGH */
  if ((x->tv_usec >= 1000000) || (x->tv_usec <= -1000000)) {
    x->tv_sec += x->tv_usec/1000000;
    x->tv_usec = x->tv_usec%1000000;
  }

  /* Make tv_usec positive. JGH */
   if (x->tv_usec < 0) {
    --x->tv_sec;
    x->tv_usec += 1000000;
 }

}

/* ================================================== */

INLINE_STATIC void
UTI_DiffTimevals(struct timeval *result,
                 struct timeval *a,
                 struct timeval *b)
{
  result->tv_sec  = a->tv_sec  - b->tv_sec;
  result->tv_usec = a->tv_usec - b->tv_usec;

  /* Correct microseconds field to bring it into the range
     (0,1000000) */

  UTI_NormaliseTimeval(result); /* JGH */

  return;
}

/* ================================================== */

/* Calculate result = a - b and return as a double */
INLINE_STATIC void
UTI_DiffTimevalsToDouble(double *result, 
                         struct timeval *a,
                         struct timeval *b)
{
  *result = (double)(a->tv_sec - b->tv_sec) +
    (double)(a->tv_usec - b->tv_usec) * 1.0e-6;
}

/* ================================================== */

INLINE_STATIC void
UTI_AddDoubleToTimeval(struct timeval *start,
                       double increment,
                       struct timeval *end)
{
  long int_part, frac_part;

  /* Don't want to do this by using (long)(1000000 * increment), since
     that will only cope with increments up to +/- 2148 seconds, which
     is too marginal here. */

  int_part = (long) increment;
  frac_part = (long) (0.5 + 1.0e6 * (increment - (double)int_part));

  end->tv_sec  = int_part  + start->tv_sec;
  end->tv_usec = frac_part + start->tv_usec;

  UTI_NormaliseTimeval(end);
}

/* ================================================== */

/* Calculate the average and difference (as a double) of two timevals */
INLINE_STATIC void
UTI_AverageDiffTimevals (struct timeval *earlier,
                         struct timeval *later,
                         struct timeval *average,
                         double *diff)
{
  struct timeval tvdiff;
  struct timeval tvhalf;

  UTI_DiffTimevals(&tvdiff, later, earlier);
  *diff = (double)tvdiff.tv_sec + 1.0e-6 * (double)tvdiff.tv_usec;

  if (*diff < 0.0) {
    /* Either there's a bug elsewhere causing 'earlier' and 'later' to
       be backwards, or something wierd has happened.  Maybe when we
       change the frequency on Linux? */

    /* This seems to be fairly benign, so don't bother logging it */

#if 0
    LOG(LOGS_INFO, LOGF_Util, "Earlier=[%s] Later=[%s]",
        UTI_TimevalToString(earlier), UTI_TimevalToString(later));
#endif

    /* Assume the required behaviour is to treat it as zero */
    *diff = 0.0;
  }

  tvhalf.tv_sec = tvdiff.tv_sec / 2;
  tvhalf.tv_usec = tvdiff.tv_usec / 2 + (tvdiff.tv_sec % 2) * 500000; /* JGH */
  
  average->tv_sec  = earlier->tv_sec  + tvhalf.tv_sec;
  average->tv_usec = earlier->tv_usec + tvhalf.tv_usec;
  
  /* Bring into range */
  UTI_NormaliseTimeval(average);

 }

/* ================================================== */

#define POOL_ENTRIES 16
#define BUFFER_LENGTH 64
static char buffer_pool[POOL_ENTRIES][BUFFER_LENGTH];
static int  pool_ptr = 0;

#define NEXT_BUFFER (buffer_pool[pool_ptr = ((pool_ptr + 1) % POOL_ENTRIES)])

/* ================================================== */
/* Convert a timeval into a temporary string, largely for diagnostic
   display */

char *
UTI_TimevalToString(struct timeval *tv)
{
  char buffer[64], *result;
  struct tm stm;
  stm = *gmtime((time_t *) &(tv->tv_sec));
  strftime(buffer, sizeof(buffer), "%a %x %X", &stm);
  result = NEXT_BUFFER;
  snprintf(result, BUFFER_LENGTH, "%s.%06ld", buffer, (unsigned long)(tv->tv_usec));
  return result;
}

/* ================================================== */
#define JAN_1970 0x83aa7e80UL

inline static void
int64_to_timeval(NTP_int64 *src,
                 struct timeval *dest)
{
  dest->tv_sec = ntohl(src->hi) - JAN_1970;
  
  /* Until I invent a slick way to do this, just do it the obvious way */
  dest->tv_usec = (int)(0.5 + (double)(ntohl(src->lo)) / 4294.967296);
}

/* ================================================== */
/* Convert an NTP timestamp into a temporary string, largely
   for diagnostic display */

char *
UTI_TimestampToString(NTP_int64 *ts)
{
  struct timeval tv;
  int64_to_timeval(ts, &tv);
  return UTI_TimevalToString(&tv);
}

/* ================================================== */

char *
UTI_RefidToString(unsigned long ref_id)
{
  unsigned int a, b, c, d;
  char *result;
  a = (ref_id>>24) & 0xff;
  b = (ref_id>>16) & 0xff;
  c = (ref_id>> 8) & 0xff;
  d = (ref_id>> 0) & 0xff;
  result = NEXT_BUFFER;
  snprintf(result, BUFFER_LENGTH, "%c%c%c%c", a, b, c, d);
  return result;
}

/* ================================================== */

char *
UTI_IPToString(IPAddr *addr)
{
  unsigned long a, b, c, d, ip;
  uint8_t *ip6;
  char *result;

  result = NEXT_BUFFER;
  switch (addr->family) {
    case IPADDR_UNSPEC:
      snprintf(result, BUFFER_LENGTH, "[UNSPEC]");
      break;
    case IPADDR_INET4:
      ip = addr->addr.in4;
      a = (ip>>24) & 0xff;
      b = (ip>>16) & 0xff;
      c = (ip>> 8) & 0xff;
      d = (ip>> 0) & 0xff;
      snprintf(result, BUFFER_LENGTH, "%ld.%ld.%ld.%ld", a, b, c, d);
      break;
    case IPADDR_INET6:
      ip6 = addr->addr.in6;
#ifdef HAVE_IPV6
      inet_ntop(AF_INET6, ip6, result, BUFFER_LENGTH);
#else
      snprintf(result, BUFFER_LENGTH, "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x",
               ip6[0], ip6[1], ip6[2], ip6[3], ip6[4], ip6[5], ip6[6], ip6[7],
               ip6[8], ip6[9], ip6[10], ip6[11], ip6[12], ip6[13], ip6[14], ip6[15]);
#endif
      break;
    default:
      snprintf(result, BUFFER_LENGTH, "[UNKNOWN]");
  }
  return result;
}

/* ================================================== */

int
UTI_StringToIP(const char *addr, IPAddr *ip)
{
#ifdef HAVE_IPV6
  struct in_addr in4;
  struct in6_addr in6;

  if (inet_pton(AF_INET, addr, &in4) > 0) {
    ip->family = IPADDR_INET4;
    ip->addr.in4 = ntohl(in4.s_addr);
    return 1;
  }

  if (inet_pton(AF_INET6, addr, &in6) > 0) {
    ip->family = IPADDR_INET6;
    memcpy(ip->addr.in6, in6.s6_addr, sizeof (ip->addr.in6));
    return 1;
  }
#else
  unsigned long a, b, c, d, n;

  n = sscanf(addr, "%lu.%lu.%lu.%lu", &a, &b, &c, &d);
  if (n == 4) {
    ip->family = IPADDR_INET4;
    ip->addr.in4 = ((a & 0xff) << 24) | ((b & 0xff) << 16) | 
                   ((c & 0xff) << 8) | (d & 0xff);
    return 1;
  }
#endif

  return 0;
}

/* ================================================== */

unsigned long
UTI_IPToRefid(IPAddr *ip)
{
  MD5_CTX ctx;

  switch (ip->family) {
    case IPADDR_INET4:
      return ip->addr.in4;
    case IPADDR_INET6:
      MD5Init(&ctx);
      MD5Update(&ctx, (unsigned const char *) ip->addr.in6, sizeof (ip->addr.in6));
      MD5Final(&ctx);
      return ctx.digest[0] << 24 | ctx.digest[1] << 16 | ctx.digest[2] << 8 | ctx.digest[3];
  }
  return 0;
}

/* ================================================== */

void
UTI_IPHostToNetwork(IPAddr *src, IPAddr *dest)
{
  /* Don't send uninitialized bytes over network */
  memset(dest, 0, sizeof (IPAddr));

  dest->family = htons(src->family);

  switch (src->family) {
    case IPADDR_INET4:
      dest->addr.in4 = htonl(src->addr.in4);
      break;
    case IPADDR_INET6:
      memcpy(dest->addr.in6, src->addr.in6, sizeof (dest->addr.in6));
      break;
  }
}

/* ================================================== */

void
UTI_IPNetworkToHost(IPAddr *src, IPAddr *dest)
{
  dest->family = ntohs(src->family);

  switch (dest->family) {
    case IPADDR_INET4:
      dest->addr.in4 = ntohl(src->addr.in4);
      break;
    case IPADDR_INET6:
      memcpy(dest->addr.in6, src->addr.in6, sizeof (dest->addr.in6));
      break;
  }
}

/* ================================================== */

int
UTI_CompareIPs(IPAddr *a, IPAddr *b, IPAddr *mask)
{
  int i, d;

  if (a->family != b->family)
    return a->family - b->family;

  if (mask && mask->family != b->family)
    mask = NULL;

  switch (a->family) {
    case IPADDR_UNSPEC:
      return 0;
    case IPADDR_INET4:
      if (mask)
        return (a->addr.in4 & mask->addr.in4) - (b->addr.in4 & mask->addr.in4);
      else
        return a->addr.in4 - b->addr.in4;
    case IPADDR_INET6:
      for (i = 0, d = 0; !d && i < 16; i++) {
        if (mask)
          d = (a->addr.in6[i] & mask->addr.in6[i]) -
              (b->addr.in6[i] & mask->addr.in6[i]);
        else
          d = a->addr.in6[i] - b->addr.in6[i];
      }
      return d;
  }
  return 0;
}

/* ================================================== */

char *
UTI_TimeToLogForm(time_t t)
{
  struct tm stm;
  char *result;

  result = NEXT_BUFFER;

  stm = *gmtime(&t);
  strftime(result, BUFFER_LENGTH, "%Y-%m-%d %H:%M:%S", &stm);

  return result;
}

/* ================================================== */

void
UTI_AdjustTimeval(struct timeval *old_tv, struct timeval *when, struct timeval *new_tv, double dfreq, double doffset)
{
  double elapsed, delta_time;

  UTI_DiffTimevalsToDouble(&elapsed, when, old_tv);
  delta_time = elapsed * dfreq - doffset;
  UTI_AddDoubleToTimeval(old_tv, delta_time, new_tv);
}

/* ================================================== */

/* Seconds part of RFC1305 timestamp correponding to the origin of the
   struct timeval format. */
#define JAN_1970 0x83aa7e80UL

void
UTI_TimevalToInt64(struct timeval *src,
                   NTP_int64 *dest)
{
  unsigned long usec = src->tv_usec;
  unsigned long sec = src->tv_sec;

  /* Recognize zero as a special case - it always signifies
     an 'unknown' value */
  if (!usec && !sec) {
    dest->hi = dest->lo = 0;
  } else {
    dest->hi = htonl(src->tv_sec + JAN_1970);

    /* This formula gives an error of about 0.1us worst case */
    dest->lo = htonl(4295 * usec - (usec>>5) - (usec>>9));
  }
}

/* ================================================== */

void
UTI_Int64ToTimeval(NTP_int64 *src,
                   struct timeval *dest)
{
  /* As yet, there is no need to check for zero - all processing that
     has to detect that case is in the NTP layer */

  dest->tv_sec = ntohl(src->hi) - JAN_1970;
  
  /* Until I invent a slick way to do this, just do it the obvious way */
  dest->tv_usec = (int)(0.5 + (double)(ntohl(src->lo)) / 4294.967296);
}

/* ================================================== */

void
UTI_TimevalNetworkToHost(Timeval *src, struct timeval *dest)
{
  uint32_t sec_low, sec_high;

  dest->tv_usec = ntohl(src->tv_usec);
  sec_high = ntohl(src->tv_sec_high);
  sec_low = ntohl(src->tv_sec_low);

  /* get the missing bits from current time when received timestamp
     is only 32-bit */
  if (sizeof (time_t) > 4 && sec_high == TV_NOHIGHSEC) {
    struct timeval now;
    struct timezone tz;

    gettimeofday(&now, &tz);
    sec_high = now.tv_sec >> 16 >> 16;
  }
  dest->tv_sec = (time_t)sec_high << 16 << 16 | sec_low;
}

/* ================================================== */

void
UTI_TimevalHostToNetwork(struct timeval *src, Timeval *dest)
{
  dest->tv_usec = htonl(src->tv_usec);
  if (sizeof (time_t) > 4)
    dest->tv_sec_high = htonl(src->tv_sec >> 16 >> 16);
  else
    dest->tv_sec_high = htonl(TV_NOHIGHSEC);
  dest->tv_sec_low = htonl(src->tv_sec);
}


/* ================================================== */
