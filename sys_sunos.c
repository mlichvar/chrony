/*
  $Header: /cvs/src/chrony/sys_sunos.c,v 1.18 2003/03/24 23:35:43 richard Exp $

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
 * 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 * 
 **********************************************************************

  =======================================================================

  Driver file for the SunOS 4.1.x operating system.
  */

#ifdef SUNOS

#include <kvm.h>
#include <fcntl.h>
#include <nlist.h>
#include <assert.h>
#include <sys/time.h>

#include <stdio.h>
#include <signal.h>

#include "sys_sunos.h"
#include "localp.h"
#include "logging.h"
#include "util.h"
#include "sched.h"

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

/* Eventually, this needs to be a user-defined parameter - e.g. user
   might want 5 to get much finer resolution like xntpd.  We stick
   with a reasonable number so that slewing can work.

   This value has to be a factor of 1 million, otherwise the noddy
   method we use for rounding an adjustment to the nearest multiple of
   this value won't work!!

   */
static unsigned long our_tickadj = 100;

/* ================================================== */

static void
clock_initialise(void)
{
  struct timeval newadj, oldadj;
  struct timezone tz;

  offset_register = 0.0;
  adjustment_requested = 0.0;
  current_freq = 0.0;

  if (gettimeofday(&T0, &tz) < 0) {
    CROAK("gettimeofday() failed in clock_initialise()");
  }

  newadj.tv_sec = 0;
  newadj.tv_usec = 0;

  if (adjtime(&newadj, &oldadj) < 0) {
    CROAK("adjtime() failed in clock_initialise");
  }

  if (adjtime(&newadj, &oldadj) < 0) {
    CROAK("adjtime() failed in clock_initialise");
  }

  return;
}

/* ================================================== */

static void
clock_finalise(void)
{
  /* Nothing to do yet */

  return;

}

/* ================================================== */

static void
start_adjust(void)
{
  struct timeval newadj, oldadj;
  struct timeval T1;
  struct timezone tz;
  double elapsed, accrued_error;
  double adjust_required;
  struct timeval exact_newadj;
  double rounding_error;
  double old_adjust_remaining;
  long remainder, multiplier;

  /* Determine the amount of error built up since the last adjustment */
  if (gettimeofday(&T1, &tz) < 0) {
    CROAK("gettimeofday() failed in start_adjust");
  }

  UTI_DiffTimevalsToDouble(&elapsed, &T1, &T0);
  accrued_error = elapsed * current_freq;
  
  adjust_required = - (accrued_error + offset_register);

  UTI_DoubleToTimeval(adjust_required, &exact_newadj);

  /* At this point, we need to round the required adjustment to the
     closest multiple of _tickadj --- because SunOS can't process
     other adjustments exactly and will silently discard the residual.
     Obviously such behaviour can't be tolerated for us. */

  newadj = exact_newadj;
  remainder = newadj.tv_usec % our_tickadj;
  multiplier = newadj.tv_usec / our_tickadj;
  if (remainder >= (our_tickadj >> 1)) {
    newadj.tv_usec = (multiplier + 1) * our_tickadj;
  } else {
    newadj.tv_usec = multiplier * our_tickadj;
  }

  UTI_NormaliseTimeval(&newadj);
  
  /* Want to *add* rounding error back onto offset register.  Note
     that the exact adjustment was the offset register *negated* */
  UTI_DiffTimevalsToDouble(&rounding_error, &newadj, &exact_newadj);

  if (adjtime(&newadj, &oldadj) < 0) {
    CROAK("adjtime() failed in start_adjust");
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
  struct timezone tz;
  struct timeval zeroadj, remadj;
  double adjustment_remaining, adjustment_achieved;
  double gap;
  double elapsed, elapsed_plus_adjust;

  zeroadj.tv_sec = 0;
  zeroadj.tv_usec = 0;

  if (adjtime(&zeroadj, &remadj) < 0) {
    CROAK("adjtime() failed in stop_adjust");
  }

  if (gettimeofday(&T1, &tz) < 0) {
    CROAK("gettimeofday() failed in stop_adjust");
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
  return;
}

/* ================================================== */

/* Positive offset means system clock is fast of true time, therefore
   step backwards */

static void
apply_step_offset(double offset)
{
  struct timeval old_time, new_time, T1;
  struct timezone tz;
  
  stop_adjust();
  if (gettimeofday(&old_time, &tz) < 0) {
    CROAK("gettimeofday in apply_step_offset");
  }

  UTI_AddDoubleToTimeval(&old_time, -offset, &new_time);

  if (settimeofday(&new_time, &tz) < 0) {
    CROAK("settimeofday in apply_step_offset");
  }

  UTI_AddDoubleToTimeval(&T0, offset, &T1);
  T0 = T1;

  start_adjust();

}

/* ================================================== */

static void
set_frequency(double new_freq_ppm)
{
  stop_adjust();
  current_freq = new_freq_ppm * 1.0e-6;
  start_adjust();
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
                      double *corr)
{
  stop_adjust();
  *corr = -offset_register;
  start_adjust();
  return;
}

/* ================================================== */

static void
immediate_step(void)
{
  return;
}

/* ================================================== */

/* Interval in seconds between adjustments to cancel systematic drift */
#define DRIFT_REMOVAL_INTERVAL (4.0)

static int drift_removal_running = 0;
static SCH_TimeoutID drift_removal_id;

/* ================================================== */
/* This is the timer callback routine which is called periodically to
   invoke a time adjustment to take out the machine's drift.
   Otherwise, times reported through this software (e.g. by running
   ntpdate from another machine) show the machine being correct (since
   they correct for drift build-up), but any program on this machine
   that reads the system time will be given an erroneous value, the
   degree of error depending on how long it is since
   get_offset_correction was last called. */

static void
drift_removal_timeout(SCH_ArbitraryArgument not_used)
{
  stop_adjust();
  start_adjust();
  drift_removal_id = SCH_AddTimeoutByDelay(DRIFT_REMOVAL_INTERVAL, drift_removal_timeout, NULL);
}

/* ================================================== */

static void
setup_kernel(unsigned long on_off)
{
  static struct nlist nl[] = {
    {"_dosynctodr"},
    {"_tick"},
    {"_tickadj"},
    {NULL}
  };

  kvm_t *kt;
  unsigned long read_back;
  unsigned long our_tick = 10000;
  unsigned long default_tickadj = 625;

  if (on_off!=1 && on_off!=0) {
    CROAK("on_off should be 0 or 1");
  }

  kt = kvm_open(NULL, NULL, NULL, O_RDWR, NULL);
  if (!kt) {
    LOG(LOGS_ERR, LOGF_SysSunOS, "Cannot open kvm");
    return;
  }

  if (kvm_nlist(kt, nl) < 0) {
    LOG(LOGS_ERR, LOGF_SysSunOS, "Cannot read kernel symbols");
    kvm_close(kt);
    return;
  }

  if (kvm_write(kt, nl[0].n_value, (char *)(&on_off), sizeof(unsigned long)) < 0) {
    LOG(LOGS_ERR, LOGF_SysSunOS, "Cannot write to _dosynctodr");
    kvm_close(kt);
    return;
  }

  if (kvm_write(kt, nl[1].n_value, (char *)(&our_tick), sizeof(unsigned long)) < 0) {
    LOG(LOGS_ERR, LOGF_SysSunOS, "Cannot write to _tick");
    kvm_close(kt);
    return;
  }

  if (kvm_write(kt, nl[2].n_value,
                (char *)(&(on_off ? default_tickadj : our_tickadj)),
                sizeof(unsigned long)) < 0) {
    LOG(LOGS_ERR, LOGF_SysSunOS, "Cannot write to _tickadj");
    kvm_close(kt);
    return;
  }

  kvm_close(kt);

#if 0
  LOG(LOGS_INFO, LOGF_SysSunOS, "Set value of _dosynctodr to %d", on_off);
#endif

}

/* ================================================== */

void
SYS_SunOS_Initialise(void)
{

  /* Need to do KVM stuff to turn off dosynctodr. */

  clock_initialise();

  lcl_RegisterSystemDrivers(read_frequency, set_frequency, 
                            accrue_offset, apply_step_offset,
                            get_offset_correction, NULL /* immediate_step */);

  /* Turn off the kernel switch that keeps the system clock in step
     with the non-volatile clock */
  setup_kernel(0);

  drift_removal_id = SCH_AddTimeoutByDelay(DRIFT_REMOVAL_INTERVAL, drift_removal_timeout, NULL);
  drift_removal_running = 1;

}

/* ================================================== */

void
SYS_SunOS_Finalise(void)
{

  if (drift_removal_running) {
    SCH_RemoveTimeout(drift_removal_id);
  }

  /* Turn dosynctodr back on?? */

  clock_finalise();

  /* When exiting, we want to return the machine to its 'autonomous'
     tracking mode */
  setup_kernel(1);

  return;
}

/* ================================================== */


#endif /* SUNOS */
