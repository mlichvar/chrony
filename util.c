/*
  $Header: /cvs/src/chrony/util.c,v 1.21 2003/09/22 21:22:30 richard Exp $

  =======================================================================

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
 * 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 * 
 **********************************************************************

  =======================================================================

  Various utility functions
  */

#include "sysincl.h"

#include "util.h"
#include "logging.h"

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
    if (a->tv_sec != b->tv_sec) {
      CROAK("a->tv_sec != b->tv_sec");
    }
    if (a->tv_usec < b->tv_usec) {
      return -1;
    } else if (a->tv_usec > b->tv_usec) {
      return +1;
    } else {
      if (a->tv_usec != b->tv_usec) {
        CROAK("a->tv_usec != b->tv_usec");
      }
      return 0;
    }
  }
  CROAK("Impossible"); /* Shouldn't be able to fall through. */
}

/* ================================================== */

INLINE_STATIC void
UTI_NormaliseTimeval(struct timeval *x)
{
  while (x->tv_usec >= 1000000) {
    ++x->tv_sec;
    x->tv_usec -= 1000000;
  }

  while (x->tv_usec < 0) {
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
     [0,1000000) */

  while (result->tv_usec < 0) {
    result->tv_usec += 1000000;
    --result->tv_sec;
  }

  while (result->tv_usec > 999999) {
    result->tv_usec -= 1000000;
    ++result->tv_sec;
  }

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
  tvhalf.tv_usec = tvdiff.tv_usec / 2 + (tvdiff.tv_sec % 2);
  
  average->tv_sec  = earlier->tv_sec  + tvhalf.tv_sec;
  average->tv_usec = earlier->tv_usec + tvhalf.tv_usec;
  
  /* Bring into range */
  UTI_NormaliseTimeval(average);

  while (average->tv_usec >= 1000000) {
    ++average->tv_sec;
    average->tv_usec -= 1000000;
  }

  while (average->tv_usec < 0) {
    --average->tv_sec;
    average->tv_usec += 1000000;
  }

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
  snprintf(result, sizeof(buffer), "%s.%06ld", buffer, (unsigned long)(tv->tv_usec));
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
UTI_IPToDottedQuad(unsigned long ip)
{
  unsigned long a, b, c, d;
  char *result;
  a = (ip>>24) & 0xff;
  b = (ip>>16) & 0xff;
  c = (ip>> 8) & 0xff;
  d = (ip>> 0) & 0xff;
  result = NEXT_BUFFER;
  snprintf(result, sizeof(result), "%ld.%ld.%ld.%ld", a, b, c, d);
  return result;
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
/* Force a core dump and exit without doing abort() or assert(0).
   These do funny things with the call stack in the core file that is
   generated, which makes diagnosis difficult. */

int
croak(const char *file, int line, const char *msg)
{
  int a;
  LOG(LOGS_ERR, LOGF_Util, "Unexpected condition [%s] at %s:%d, core dumped",
      msg, file, line);
  a = * (int *) 0;
  return a; /* Can't happen - this stops the optimiser optimising the
               line above */
}

/* ================================================== */
