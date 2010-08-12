/*
  $Header: /cvs/src/chrony/sys_netbsd.c,v 1.2 2002/02/17 22:13:49 richard Exp $

  =======================================================================

  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2001
 * Copyright (C) J. Hannken-Illjes  2001
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

  Driver file for the NetBSD operating system.
  */

#ifdef __NetBSD__

#include <kvm.h>
#include <nlist.h>
#include <fcntl.h>
#include <assert.h>
#include <sys/time.h>

#include <stdio.h>
#include <signal.h>

#include "sys_netbsd.h"
#include "localp.h"
#include "logging.h"
#include "util.h"

/* ================================================== */

/* This register contains the number of seconds by which the local
   clock was estimated to be fast of reference time at the epoch when
   gettimeofday() returned T0 */

static double offset_register;

/* This register contains the epoch to which the offset is referenced */

static struct timeval T0;

/* This register contains the current estimate of the system
   frequency, in absolute (NOT ppm) */

static double current_freq;

/* This register contains the number of seconds of adjustment that
   were passed to adjtime last time it was called. */

static double adjustment_requested;

/* Kernel parameters to calculate adjtime error. */

static int kern_tickadj;
static long kern_bigadj;

/* ================================================== */

static void
clock_initialise(void)
{
  struct timeval newadj, oldadj;

  offset_register = 0.0;
  adjustment_requested = 0.0;
  current_freq = 0.0;

  if (gettimeofday(&T0, NULL) < 0) {
    LOG_FATAL(LOGF_SysNetBSD, "gettimeofday() failed");
  }

  newadj.tv_sec = 0;
  newadj.tv_usec = 0;

  if (adjtime(&newadj, &oldadj) < 0) {
    LOG_FATAL(LOGF_SysNetBSD, "adjtime() failed");
  }

}

/* ================================================== */

static void
clock_finalise(void)
{
  /* Nothing to do yet */

}

/* ================================================== */

static void
start_adjust(void)
{
  struct timeval newadj, oldadj;
  struct timeval T1;
  double elapsed, accrued_error;
  double adjust_required;
  struct timeval exact_newadj;
  long delta, tickdelta;
  double rounding_error;
  double old_adjust_remaining;

  /* Determine the amount of error built up since the last adjustment */
  if (gettimeofday(&T1, NULL) < 0) {
    LOG_FATAL(LOGF_SysNetBSD, "gettimeofday() failed");
  }

  UTI_DiffTimevalsToDouble(&elapsed, &T1, &T0);
  accrued_error = elapsed * current_freq;
  
  adjust_required = - (accrued_error + offset_register);

  UTI_DoubleToTimeval(adjust_required, &exact_newadj);

  /* At this point, we need to round the required adjustment the
     same way the kernel does. */

  delta = exact_newadj.tv_sec * 1000000 + exact_newadj.tv_usec;
  if (delta > kern_bigadj || delta < -kern_bigadj)
    tickdelta = 10 * kern_tickadj;
  else
    tickdelta = kern_tickadj;
  if (delta % tickdelta)
	delta = delta / tickdelta * tickdelta;
  newadj.tv_sec = 0;
  newadj.tv_usec = delta;
  UTI_NormaliseTimeval(&newadj);

  /* Add rounding error back onto offset register. */
  UTI_DiffTimevalsToDouble(&rounding_error, &newadj, &exact_newadj);

  if (adjtime(&newadj, &oldadj) < 0) {
    LOG_FATAL(LOGF_SysNetBSD, "adjtime() failed");
  }

  UTI_TimevalToDouble(&oldadj, &old_adjust_remaining);

  offset_register = rounding_error - old_adjust_remaining;

  T0 = T1;
  UTI_TimevalToDouble(&newadj, &adjustment_requested);

}

/* ================================================== */

static void
stop_adjust(void)
{
  struct timeval T1;
  struct timeval zeroadj, remadj;
  double adjustment_remaining, adjustment_achieved;
  double elapsed, elapsed_plus_adjust;

  zeroadj.tv_sec = 0;
  zeroadj.tv_usec = 0;

  if (adjtime(&zeroadj, &remadj) < 0) {
    LOG_FATAL(LOGF_SysNetBSD, "adjtime() failed");
  }

  if (gettimeofday(&T1, NULL) < 0) {
    LOG_FATAL(LOGF_SysNetBSD, "gettimeofday() failed");
  }
  
  UTI_DiffTimevalsToDouble(&elapsed, &T1, &T0);
  UTI_TimevalToDouble(&remadj, &adjustment_remaining);

  adjustment_achieved = adjustment_requested - adjustment_remaining;
  elapsed_plus_adjust = elapsed - adjustment_achieved;

  offset_register += current_freq * elapsed_plus_adjust - adjustment_remaining;

  adjustment_requested = 0.0;
  T0 = T1;

}

/* ================================================== */

/* Positive offset means system clock is fast of true time, therefore
   slew backwards */

static void
accrue_offset(double offset)
{
  stop_adjust();
  offset_register += offset;
  start_adjust();

}

/* ================================================== */

/* Positive offset means system clock is fast of true time, therefore
   step backwards */

static void
apply_step_offset(double offset)
{
  struct timeval old_time, new_time, T1;
  
  stop_adjust();

  if (gettimeofday(&old_time, NULL) < 0) {
    LOG_FATAL(LOGF_SysNetBSD, "gettimeofday() failed");
  }

  UTI_AddDoubleToTimeval(&old_time, -offset, &new_time);

  if (settimeofday(&new_time, NULL) < 0) {
    LOG_FATAL(LOGF_SysNetBSD, "settimeofday() failed");
  }

  UTI_AddDoubleToTimeval(&T0, offset, &T1);
  T0 = T1;

  start_adjust();

}

/* ================================================== */

static double
set_frequency(double new_freq_ppm)
{
  stop_adjust();
  current_freq = new_freq_ppm * 1.0e-6;
  start_adjust();

  return current_freq * 1.0e6;
}

/* ================================================== */

static double
read_frequency(void)
{
  return current_freq * 1.0e6;
}

/* ================================================== */

static void
get_offset_correction(struct timeval *raw,
                      double *corr, double *err)
{
  stop_adjust();
  *corr = -offset_register;
  start_adjust();
  if (err)
    *err = 0.0;
}

/* ================================================== */

void
SYS_NetBSD_Initialise(void)
{
  static struct nlist nl[] = {
    {"_tickadj"},
    {"_bigadj"},
    {NULL}
  };

  kvm_t *kt;
  FILE *fp;

  kt = kvm_open(NULL, NULL, NULL, O_RDONLY, NULL);
  if (!kt) {
    LOG_FATAL(LOGF_SysNetBSD, "Cannot open kvm");
  }

  if (kvm_nlist(kt, nl) < 0) {
    LOG_FATAL(LOGF_SysNetBSD, "Cannot read kernel symbols");
  }

  if (kvm_read(kt, nl[0].n_value, (char *)(&kern_tickadj), sizeof(int)) < 0) {
    LOG_FATAL(LOGF_SysNetBSD, "Cannot read from _tickadj");
  }

  if (kvm_read(kt, nl[1].n_value, (char *)(&kern_bigadj), sizeof(long)) < 0) {
    /* kernel doesn't have the symbol, use one second instead */
    kern_bigadj = 1000000;
  }

  kvm_close(kt);

  clock_initialise();

  lcl_RegisterSystemDrivers(read_frequency, set_frequency, 
                            accrue_offset, apply_step_offset,
                            get_offset_correction,
                            NULL /* set_leap */);

}

/* ================================================== */

void
SYS_NetBSD_Finalise(void)
{
  clock_finalise();
}

/* ================================================== */


#endif /* __NetBSD__ */
