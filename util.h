/*
  $Header: /cvs/src/chrony/util.h,v 1.15 2003/09/22 21:22:30 richard Exp $

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

#ifndef GOT_UTIL_H
#define GOT_UTIL_H

#include "sysincl.h"

#include "ntp.h"

/* Convert a timeval into a floating point number of seconds */
extern void UTI_TimevalToDouble(struct timeval *a, double *b);

/* Convert a number of seconds expressed in floating point into a
   timeval */
extern void UTI_DoubleToTimeval(double a, struct timeval *b);

/* Returns -1 if a comes earlier than b, 0 if a is the same time as b,
   and +1 if a comes after b */
extern int UTI_CompareTimevals(struct timeval *a, struct timeval *b);

/* Normalise a struct timeval, by adding or subtracting seconds to bring
   its microseconds field into range */
extern void UTI_NormaliseTimeval(struct timeval *x);

/* Calculate result = a - b */
extern void UTI_DiffTimevals(struct timeval *result, struct timeval *a, struct timeval *b);

/* Calculate result = a - b and return as a double */
extern void UTI_DiffTimevalsToDouble(double *result, struct timeval *a, struct timeval *b);

/* Add a double increment to a timeval to get a new one. 'start' is
   the starting time, 'end' is the result that we return.  This is
   safe to use if start and end are the same */
extern void UTI_AddDoubleToTimeval(struct timeval *start, double increment, struct timeval *end);

/* Calculate the average and difference (as a double) of two timevals */
extern void UTI_AverageDiffTimevals(struct timeval *earlier, struct timeval *later, struct timeval *average, double *diff);

/* Convert a timeval into a temporary string, largely for diagnostic
   display */
extern char *UTI_TimevalToString(struct timeval *tv);

/* Convert an NTP timestamp into a temporary string, largely for
   diagnostic display */
extern char *UTI_TimestampToString(NTP_int64 *ts);

/* Convert ref_id into a temporary string, for diagnostics */
extern char *UTI_RefidToString(unsigned long ref_id);

/* Convert an IP address to dotted quad notation, for diagnostics */
extern char *UTI_IPToDottedQuad(unsigned long ip);

extern char *UTI_TimeToLogForm(time_t t);

/* Adjust time following a frequency/offset change */
extern void UTI_AdjustTimeval(struct timeval *old_tv, struct timeval *when, struct timeval *new_tv, double dfreq, double doffset);


extern void UTI_TimevalToInt64(struct timeval *src, NTP_int64 *dest);

extern void UTI_Int64ToTimeval(NTP_int64 *src, struct timeval *dest);

#if defined (INLINE_UTILITIES)
#define INLINE_STATIC inline static
#include "util.c"
#else
#define INLINE_STATIC
#endif /* defined (INLINE_UTILITIES) */

#endif /* GOT_UTIL_H */
