/*
  $Header: /cvs/src/chrony/sys_solaris.c,v 1.19 2003/09/22 21:22:30 richard Exp $

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

  Driver file for Solaris operating system
  */

#ifdef SOLARIS

#include <kvm.h>
#include <fcntl.h>
#include <nlist.h>
#include <assert.h>
#include <sys/time.h>
#include <sys/utsname.h>

#include <stdio.h>

#include "sys_solaris.h"
#include "localp.h"
#include "sched.h"
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

/* ================================================== */
/* On Solaris 2.5 & 2.5.1, passing an argument of zero as the new
   delta to adjtime does not zero out the adjustment - the remaining
   adjustment is returned as the old delta arg, but the adjustment keeps
   running.  To get round this, we set adjustments of +/-1us when we
   really want zero.  Alternate adjustments are used to avoid a drift
   from building up. */

static struct timeval zeroes[2] = {
  {0, 1},
  {-1, 999999}
};

static int index=0;

/* If 1, need to run dosynctodr().  If 0, don't */
static int need_dosynctodr = -1;


#define GET_ZERO (zeroes[index^=1])

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

  newadj = GET_ZERO;

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

  /* Determine the amount of error built up since the last adjustment */
  if (gettimeofday(&T1, &tz) < 0) {
    CROAK("gettimeofday() failed in start_adjust");
  }

  UTI_DiffTimevalsToDouble(&elapsed, &T1, &T0);
  accrued_error = elapsed * current_freq;
  
  adjust_required = - (accrued_error + offset_register);

  UTI_DoubleToTimeval(adjust_required, &exact_newadj);

  /* At this point, we will need to call the adjustment rounding
     algorithm in the system-specific layer.  For now, just assume the
     adjustment can be applied exactly. */

  newadj = exact_newadj;
  
  /* Want to *add* rounding error back onto offset register */
  UTI_DiffTimevalsToDouble(&rounding_error, &exact_newadj, &newadj);

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
  double elapsed, elapsed_plus_adjust;


  zeroadj = GET_ZERO;

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
  struct timeval old_time, new_time, rounded_new_time, T1;
  double rounding_error;
  struct timezone tz;
  
  stop_adjust();
  if (gettimeofday(&old_time, &tz) < 0) {
    CROAK("gettimeofday in apply_step_offset");
  }

  UTI_AddDoubleToTimeval(&old_time, -offset, &new_time);

  /* The settimeofday function (on Solaris 2.5/Sparc20 at least) does
     not work quite as we would want.  The time we want to set is
     rounded to the nearest second and that time is used.  Also, the
     clock appears to start from that second boundary plus about 4ms.
     For now we'll tolerate this small error. */

  rounded_new_time.tv_usec = 0;
  if (new_time.tv_usec >= 500000) {
    rounded_new_time.tv_sec = new_time.tv_sec + 1;
  } else {
    rounded_new_time.tv_sec = new_time.tv_sec;
  }

  UTI_DiffTimevalsToDouble(&rounding_error, &rounded_new_time, &new_time);

  if (settimeofday(&new_time, &tz) < 0) {
    CROAK("settimeofday in apply_step_offset");
  }

  UTI_AddDoubleToTimeval(&T0, offset, &T1);
  T0 = T1;

  offset_register += rounding_error;

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
check_need_dosynctodr(void)
{
  struct utsname name;
  int result;
  int major, minor, veryminor, n_fields;

  result = uname(&name);
  if (result < 0) {
    LOG(LOGS_ERR, LOGF_SysSolaris, "Cannot use uname to detect Solaris version");
    need_dosynctodr = 0; /* Assume recent Solaris where it isn't needed */
    return;
  }

  n_fields = sscanf(name.release, "%d.%d.%d\n", &major, &minor, &veryminor);

  if (n_fields < 2) {
    LOG(LOGS_ERR, LOGF_SysSolaris, "Solaris version doesn't appear to be of the form X.Y[.Z]");
    need_dosynctodr = 0; /* Assume recent Solaris where it isn't needed */
    return;
  }

  if (major != 5) {
    LOG(LOGS_ERR, LOGF_SysSolaris, "Solaris major version doesn't appear to be 5");
    need_dosynctodr = 0; /* Assume recent Solaris where it isn't needed */
    return;
  }

  /* The 'rule of thumb' is that from Solaris 2.6 onwards, dosynctodr() doesn't
   * need to be called, and in fact it is counter-productive to do so.  For
   * earlier versions, it is required. */ 

  if (minor < 6) {
    need_dosynctodr = 1;
  } else {
    need_dosynctodr = 0;
  }
  
}

/* ================================================== */

static void
set_dosynctodr(unsigned long on_off)
{
  static struct nlist nl[] = {
    {"dosynctodr"},
    {NULL}
  };

  kvm_t *kt;
  unsigned long read_back;

  if (on_off!=1 && on_off!=0) {
    CROAK("on_off should be 0 or 1");
  }

  kt = kvm_open(NULL, NULL, NULL, O_RDWR, NULL);
  if (!kt) {
    LOG(LOGS_ERR, LOGF_SysSolaris, "Cannot open kvm to change dosynctodr");
    return;
  }

  if (kvm_nlist(kt, nl) < 0) {
    LOG(LOGS_ERR, LOGF_SysSolaris, "Cannot read dosynctodr in nlist");
    kvm_close(kt);
    return;
  }

  if (kvm_write(kt, nl[0].n_value, (char *)(&on_off), sizeof(unsigned long)) < 0) {
    LOG(LOGS_ERR, LOGF_SysSolaris, "Cannot write to dosynctodr");
    kvm_close(kt);
    return;
  }

  if (kvm_read(kt, nl[0].n_value, (char *)(&read_back), sizeof(unsigned long)) < 0) {
    LOG(LOGS_ERR, LOGF_SysSolaris, "Cannot read from dosynctodr");
    kvm_close(kt);
    return;
  }

  kvm_close(kt);

  if (read_back != on_off) {
    CROAK("read_back should equal on_off");
  }

#if 0
  LOG(LOGS_INFO, LOGF_SysSolaris, "Set value of dosynctodr to %d", on_off);
#endif

}

/* ================================================== */

void
SYS_Solaris_Initialise(void)
{

  check_need_dosynctodr();

  /* Need to do KVM stuff to turn off dosynctodr. */

  clock_initialise();

  lcl_RegisterSystemDrivers(read_frequency, set_frequency, 
                            accrue_offset, apply_step_offset,
                            get_offset_correction, NULL /* immediate_step */);

  /* Turn off the kernel switch that keeps the system clock in step
     with the non-volatile clock */
  if (need_dosynctodr) {
    set_dosynctodr(0);
  }

  drift_removal_id = SCH_AddTimeoutByDelay(DRIFT_REMOVAL_INTERVAL, drift_removal_timeout, NULL);
  drift_removal_running = 1;
}

/* ================================================== */

void
SYS_Solaris_Finalise(void)
{

  if (drift_removal_running) {
    SCH_RemoveTimeout(drift_removal_id);
  }

  clock_finalise();

  /* When exiting, we want to return the machine to its 'autonomous'
     tracking mode */
  if (need_dosynctodr) {
    set_dosynctodr(1);
  }

  return;
}

/* ================================================== */

#endif /* SOLARIS */

