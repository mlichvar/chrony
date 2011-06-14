/*
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

  The header file for the adjtimex wrapper
  */

#ifndef GOT_WRAP_ADJTIMEX_H
#define GOT_WRAP_ADJTIMEX_H

/* Cut-down version of struct timex */
struct tmx_params {
  long tick;
  long offset;
  long freq;
  double dfreq;
  long maxerror;
  long esterror;
  
  unsigned sta_pll:1;
  unsigned sta_ppsfreq:1;  
  unsigned sta_ppstime:1;
  unsigned sta_fll:1;
  unsigned sta_ins:1;
  unsigned sta_del:1;
  unsigned sta_unsync:1;
  unsigned sta_freqhold:1;
  unsigned sta_ppssignal:1;
  unsigned sta_ppsjitter:1;
  unsigned sta_ppswander:1;
  unsigned sta_ppserror:1;
  unsigned sta_clockerr:1;
  
  int  status;
  long constant;
  long precision;
  long tolerance;
  long ppsfreq;
  long jitter;
  int  shift;
  long stabil;
  long jitcnt;
  long calcnt;
  long errcnt;
  long stbcnt;
};

int TMX_SetTick(long tick);
int TMX_ApplyOffset(long *offset);
int TMX_SetFrequency(double *freq, long tick);
int TMX_GetFrequency(double *freq, long *tick);
int TMX_GetOffsetLeft(long *offset);
int TMX_ReadCurrentParams(struct tmx_params *params);
int TMX_SetLeap(int leap);
int TMX_SetSync(int sync);
int TMX_EnableNanoPLL(void);
int TMX_ApplyPLLOffset(long offset);
int TMX_GetPLLOffsetLeft(long *offset);

#endif  /* GOT_WRAP_ADJTIMEX_H */

