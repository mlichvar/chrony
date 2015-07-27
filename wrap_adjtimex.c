/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2002
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

  This is a wrapper around the Linux adjtimex system call.

  */

#include "config.h"

#include "wrap_adjtimex.h"

#include <sys/timex.h>

/* Definitions used if missing in the system headers */
#ifndef ADJ_TAI
#define ADJ_TAI                 0x0080  /* set TAI offset */
#endif
#ifndef ADJ_SETOFFSET
#define ADJ_SETOFFSET           0x0100  /* add 'time' to current time */
#endif
#ifndef ADJ_NANO
#define ADJ_NANO                0x2000  /* select nanosecond resolution */
#endif
#ifndef ADJ_OFFSET_SS_READ
#define ADJ_OFFSET_SS_READ      0xa001  /* read-only adjtime */
#endif

/* Frequency offset scale (shift) */
#define SHIFT_USEC 16

static int status = 0;

int
TMX_ResetOffset(void)
{
  struct timex txc;

  /* Reset adjtime() offset */
  txc.modes = ADJ_OFFSET_SINGLESHOT;
  txc.offset = 0;
  if (adjtimex(&txc) < 0)
    return -1;

  /* Reset PLL offset */
  txc.modes = ADJ_OFFSET | ADJ_STATUS;
  txc.status = STA_PLL;
  txc.offset = 0;
  if (adjtimex(&txc) < 0)
    return -1;

  /* Set status back */
  txc.modes = ADJ_STATUS;
  txc.status = status;
  if (adjtimex(&txc) < 0)
    return -1;

  return 0;
}

int
TMX_SetFrequency(double *freq, long tick)
{
  struct timex txc;
  
  txc.modes = ADJ_TICK | ADJ_FREQUENCY;

  txc.freq = (long)(*freq * (double)(1 << SHIFT_USEC));
  *freq = txc.freq / (double)(1 << SHIFT_USEC);
  txc.tick = tick;

  return adjtimex(&txc);
}

int
TMX_GetFrequency(double *freq, long *tick)
{
  struct timex txc;
  int result;
  txc.modes = 0; /* pure read */
  result = adjtimex(&txc);
  *freq = txc.freq / (double)(1 << SHIFT_USEC);
  *tick = txc.tick;
  return result;
}

int
TMX_SetLeap(int leap)
{
  struct timex txc;

  status &= ~(STA_INS | STA_DEL);

  if (leap > 0) {
    status |= STA_INS;
  } else if (leap < 0) {
    status |= STA_DEL;
  }
  
  txc.modes = ADJ_STATUS;
  txc.status = status;

  return adjtimex(&txc);
}

int
TMX_GetLeap(int *leap, int *applied)
{
  struct timex txc;
  int state;

  txc.modes = 0;
  state = adjtimex(&txc);
  if (state < 0)
    return -1;

  if (txc.status & STA_INS)
    *leap = 1;
  else if (txc.status & STA_DEL)
    *leap = -1;
  else
    *leap = 0;

  *applied = state == TIME_WAIT;

  return 0;
}

int TMX_SetSync(int sync, double est_error, double max_error)
{
  struct timex txc;

  if (sync) {
    status &= ~STA_UNSYNC;
  } else {
    status |= STA_UNSYNC;
  }

  txc.modes = ADJ_STATUS | ADJ_ESTERROR | ADJ_MAXERROR;
  txc.status = status;
  txc.esterror = est_error * 1.0e6;
  txc.maxerror = max_error * 1.0e6;

  return adjtimex(&txc);
}

int
TMX_TestStepOffset(void)
{
  struct timex txc;

  /* Zero maxerror and check it's reset to a maximum after ADJ_SETOFFSET.
     This seems to be the only way how to verify that the kernel really
     supports the ADJ_SETOFFSET mode as it doesn't return an error on unknown
     mode. */

  txc.modes = ADJ_MAXERROR;
  txc.maxerror = 0;
  if (adjtimex(&txc) < 0 || txc.maxerror != 0)
    return -1;

  txc.modes = ADJ_SETOFFSET | ADJ_NANO;
  txc.time.tv_sec = 0;
  txc.time.tv_usec = 0;
  if (adjtimex(&txc) < 0 || txc.maxerror < 100000)
    return -1;

  return 0;
}

int
TMX_ApplyStepOffset(double offset)
{
  struct timex txc;

  txc.modes = ADJ_SETOFFSET | ADJ_NANO;
  txc.time.tv_sec = offset;
  txc.time.tv_usec = 1.0e9 * (offset - txc.time.tv_sec);
  if (txc.time.tv_usec < 0) {
    txc.time.tv_sec--;
    txc.time.tv_usec += 1000000000;
  }

  return adjtimex(&txc);
}
