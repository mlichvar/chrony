/*
  $Header: /cvs/src/chrony/wrap_adjtimex.c,v 1.9 2002/11/19 21:33:42 richard Exp $

  =======================================================================

  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2002
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

  This is a wrapper around the Linux adjtimex system call.  It isolates the
  inclusion of <linux/adjtimex.h> from the need to include other header files,
  many of which conflict with those in <linux/...> on some recent distributions
  (as of Jul 2000) using kernels around 2.2.16 onwards.

  */

#ifdef LINUX

#define _LOOSE_KERNEL_NAMES

#include "chrony_timex.h"
#include "wrap_adjtimex.h"

/* Save leap status between calls */
static int leap_status = 0;

int
TMX_SetTick(long tick)
{
  struct timex txc;
  txc.modes = ADJ_TICK;
  txc.tick = tick;
  
  return adjtimex(&txc);
}

int
TMX_ApplyOffset(long *offset)
{
  struct timex txc;
  int result;

  txc.modes = ADJ_OFFSET_SINGLESHOT;
  txc.offset = *offset;
  result = adjtimex(&txc);
  *offset = txc.offset;
  return result;
}

int
TMX_SetFrequency(double freq, long tick)
{
  struct timex txc;
  
  txc.modes = ADJ_TICK | ADJ_FREQUENCY | ADJ_STATUS;

  txc.freq = (long)(freq * (double)(1 << SHIFT_USEC));
  txc.tick = tick;
  txc.status = STA_UNSYNC; /* Prevent any of the FLL/PLL stuff coming
                              up */
  txc.status |= leap_status; /* Preserve leap bits */

  return adjtimex(&txc);
}

int
TMX_GetFrequency(double *freq)
{
  struct timex txc;
  int result;
  txc.modes = 0; /* pure read */
  result = adjtimex(&txc);
  *freq = txc.freq / (double)(1 << SHIFT_USEC);
  return result;
}

int
TMX_GetOffsetLeftOld(long *offset)
{
  struct timex txc;
  int result;
  txc.modes = 0; /* pure read */
  result = adjtimex(&txc);
  *offset = txc.offset;
  return result;
}

int
TMX_GetOffsetLeft(long *offset)
{
  struct timex txc;
  int result;
  txc.modes = ADJ_OFFSET_SS_READ;
  result = adjtimex(&txc);
  *offset = txc.offset;
  return result;
}

int
TMX_ReadCurrentParams(struct tmx_params *params)
{
  struct timex txc;
  int result;
  
  txc.modes = 0; /* pure read */
  result = adjtimex(&txc);

  params->tick     = txc.tick;
  params->offset   = txc.offset;
  params->freq     = txc.freq;
  params->dfreq    = txc.freq / (double)(1 << SHIFT_USEC);
  params->maxerror = txc.maxerror;
  params->esterror = txc.esterror;
  
  params->sta_pll       = !!(txc.status & STA_PLL);
  params->sta_ppsfreq   = !!(txc.status & STA_PPSFREQ);
  params->sta_ppstime   = !!(txc.status & STA_PPSTIME);
  params->sta_fll       = !!(txc.status & STA_FLL);
  params->sta_ins       = !!(txc.status & STA_INS);
  params->sta_del       = !!(txc.status & STA_DEL);
  params->sta_unsync    = !!(txc.status & STA_UNSYNC);
  params->sta_freqhold  = !!(txc.status & STA_FREQHOLD);
  params->sta_ppssignal = !!(txc.status & STA_PPSSIGNAL);
  params->sta_ppsjitter = !!(txc.status & STA_PPSJITTER);
  params->sta_ppswander = !!(txc.status & STA_PPSWANDER);
  params->sta_ppserror  = !!(txc.status & STA_PPSERROR);
  params->sta_clockerr  = !!(txc.status & STA_CLOCKERR);

  params->constant  = txc.constant;
  params->precision = txc.precision;
  params->tolerance = txc.tolerance;
  params->ppsfreq   = txc.ppsfreq;
  params->jitter    = txc.jitter;
  params->shift     = txc.shift;
  params->stabil    = txc.stabil;
  params->jitcnt    = txc.jitcnt;
  params->calcnt    = txc.calcnt;
  params->errcnt    = txc.errcnt;
  params->stbcnt    = txc.stbcnt;

  return result;
}

int
TMX_SetLeap(int leap)
{
  struct timex txc;

  if (leap > 0) {
    leap_status = STA_INS;
  } else if (leap < 0) {
    leap_status = STA_DEL;
  } else {
    leap_status = 0;
  }
  
  txc.modes = ADJ_STATUS;
  txc.status = STA_UNSYNC | leap_status;

  return adjtimex(&txc);
}

#endif

