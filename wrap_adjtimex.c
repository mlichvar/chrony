/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2002
 * Copyright (C) Miroslav Lichvar  2011
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

#include "config.h"

#ifdef LINUX

#define _LOOSE_KERNEL_NAMES

#include "chrony_timex.h"
#include "wrap_adjtimex.h"

static int status = 0;

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
TMX_SetFrequency(double *freq, long tick)
{
  struct timex txc;
  
  txc.modes = ADJ_TICK | ADJ_FREQUENCY | ADJ_STATUS;

  txc.freq = (long)(*freq * (double)(1 << SHIFT_USEC));
  *freq = txc.freq / (double)(1 << SHIFT_USEC);
  txc.tick = tick;
  txc.status = status; 

  if (!(status & STA_UNSYNC)) {
    /* maxerror has to be reset periodically to prevent kernel
       from enabling UNSYNC flag */
    txc.modes |= ADJ_MAXERROR;
    txc.maxerror = 0;
  }

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

int TMX_SetSync(int sync)
{
  struct timex txc;

  if (sync) {
    status &= ~STA_UNSYNC;
  } else {
    status |= STA_UNSYNC;
  }

  txc.modes = ADJ_STATUS;
  txc.status = status;

  return adjtimex(&txc);
}

int
TMX_EnableNanoPLL(void)
{
  struct timex txc;
  int result;

  txc.modes = ADJ_STATUS | ADJ_OFFSET | ADJ_TIMECONST | ADJ_NANO;
  txc.status = STA_PLL | STA_FREQHOLD;
  txc.offset = 0;
  txc.constant = 0;
  result = adjtimex(&txc);
  if (result < 0 || !(txc.status & STA_NANO) || txc.offset || txc.constant)
    return -1;

  status |= STA_PLL | STA_FREQHOLD;
  return result;
}

int
TMX_ApplyPLLOffset(long offset, long constant)
{
  struct timex txc;

  txc.modes = ADJ_OFFSET | ADJ_TIMECONST | ADJ_NANO;
  txc.offset = offset;
  txc.constant = constant;
  return adjtimex(&txc);
}

int
TMX_GetPLLOffsetLeft(long *offset)
{
  struct timex txc;
  int result;

  txc.modes = 0;
  result = adjtimex(&txc);
  *offset = txc.offset;
  return result;
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

  txc.modes = ADJ_SETOFFSET;
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

  txc.modes = ADJ_SETOFFSET;
  if (offset >= 0) {
    txc.time.tv_sec = offset;
  } else {
    txc.time.tv_sec = offset - 1;
  }

  /* ADJ_NANO changes the status even with ADJ_SETOFFSET, use it only when
     STA_NANO is already enabled */
  if (status & STA_PLL) {
    txc.modes |= ADJ_NANO;
    txc.time.tv_usec = 1e9 * (offset - txc.time.tv_sec);
  } else {
    txc.time.tv_usec = 1e6 * (offset - txc.time.tv_sec);
  }

  return adjtimex(&txc);
}

#endif

