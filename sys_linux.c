/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2003
 * Copyright (C) John G. Hasler  2009
 * Copyright (C) Miroslav Lichvar  2009-2011
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

  This is the module specific to the Linux operating system.

  */

#include "config.h"

#ifdef LINUX

#include <sys/time.h>
#include <stdio.h>
#include <unistd.h>
#include <math.h>
#include <ctype.h>
#include <assert.h>
#include <sys/utsname.h>

#if defined(HAVE_SCHED_SETSCHEDULER)
#  include <sched.h>
int SchedPriority = 0;
#endif

#if defined(HAVE_MLOCKALL)
#  include <sys/mman.h>
#include <sys/resource.h>
int LockAll = 0;
#endif

#ifdef FEAT_LINUXCAPS
#include <sys/types.h>
#include <pwd.h>
#include <sys/prctl.h>
#include <sys/capability.h>
#include <grp.h>
#endif

#include "localp.h"
#include "sys_linux.h"
#include "sched.h"
#include "util.h"
#include "conf.h"
#include "logging.h"
#include "wrap_adjtimex.h"

static long current_tick;

/* This is the value of tick, in seconds, including the current vernier
   frequency term */
static double current_total_tick;

/* This is the uncompensated system tick value */
static int nominal_tick;

/* This is the scaling required to go between absolute ppm and the
   scaled ppm used as an argument to adjtimex.  Because chronyd is to an extent
   'closed loop' maybe it doesn't matter if this is wrongly determined, UNLESS
   the system's ppm error is close to a multiple of HZ, in which case the
   relationship between changing the frequency and changing the value of 'tick'
   will be wrong.  This would (I imagine) cause the system to thrash between
   two states.
   
   However..., if this effect was not corrected, and the system is left offline
   for a long period, a substantial error would build up.  e.g. with HZ==100,
   the correction required is 128/128.125, giving a drift of about 84 seconds
   per day). */
static double freq_scale;

/* The HZ value from the kernel header file (may be over-ridden from config
   file, e.g. if chronyd binary is moved to a box whose kernel was built with a
   different HZ value). */
static int hz;
static double dhz; /* And dbl prec version of same for arithmetic */


/* ================================================== */

/* The operating system kernel version */
static int version_major;
static int version_minor;
static int version_patchlevel;

/* Flag indicating whether adjtimex() returns the remaining time adjustment
or not.  If not we have to read the outstanding adjustment by setting it to
zero, examining the return value and setting the outstanding adjustment back
again. */

static int have_readonly_adjtime;

/* Flag indicating whether kernel supports PLL in nanosecond resolution.
   If supported, it will be used instead of adjtime() for very small
   adjustments. */
static int have_nanopll;

/* ================================================== */

static void handle_end_of_slew(void *anything);

/* ================================================== */

inline static long
our_round(double x) {
  long y;

  if (x > 0.0)
	  y = x + 0.5;
  else
	  y = x - 0.5;
  return y;
}

/* ================================================== */
/* Amount of outstanding offset to process */
static double offset_register;

/* Flag set true if an adjtime slew was started and still may be running */
static int slow_slewing;

/* Flag set true if a PLL nano slew was started and still may be running */
static int nano_slewing;

/* Flag set true if a fast slew (one done by altering tick) is being
   run at the moment */
static int fast_slewing;

/* The amount by which the fast slew was supposed to slew the clock */
static double fast_slew_wanted;

/* The value programmed into the kernel's 'tick' variable whilst
   slewing a large offset */
static long slewing_tick;

/* The timeval (raw) at which a fast slew was started.  We need to
   know this for two reasons.  First, if we want to change the
   frequency midway through, we will want to abort the slew and return
   the unprocessed portion to the offset register to start again
   later.  Second, when the end of the slew expires, we need to know
   precisely how long we have been slewing for, so that we can negate
   the excess and slew it back the other way. */
static struct timeval slew_start_tv;

/* This is the ID returned to use by the scheduler's timeout handler.
   We need this if we subsequently wish to abort a slew, because we will have to
   dequeue the timeout */
static SCH_TimeoutID slew_timeout_id;

/* The adjustment that we apply to 'tick', in seconds, whilst applying
   a fast slew */
static double delta_total_tick;

/* Max amount of time that we wish to slew by using adjtime (or its
   equivalent).  If more than this is outstanding, we alter the value
   of tick instead, for a set period.  Set this according to the
   amount of time that a dial-up clock might need to be shifted
   assuming it is resync'ed about once per day. (TBC) */
#define MAX_ADJUST_WITH_ADJTIME (0.2)

/* Max amount of time that should be adjusted by kernel PLL */
#define MAX_ADJUST_WITH_NANOPLL (1.0e-5)

/* The amount by which we alter 'tick' when doing a large slew */
static int slew_delta_tick;

/* The maximum amount by which 'tick' can be biased away from 'nominal_tick'
   (sys_adjtimex() in the kernel bounds this to 10%) */
static int max_tick_bias;

/* The latest time at which system clock may still be slewed by previous
   adjtime() call and maximum offset correction error it can cause */
static struct timeval slow_slew_error_end;
static int slow_slew_error;

/* Timeval at which the latest nano PLL adjustment was started and maximum
   offset correction error it can cause */
static struct timeval nano_slew_error_start;
static int nano_slew_error;

/* The latest time at which 'tick' in kernel may be actually updated
   and maximum offset correction error it can cause */
static struct timeval fast_slew_error_end;
static double fast_slew_error;

/* The rate at which frequency and tick values are updated in kernel. */
static int tick_update_hz;

/* ================================================== */
/* These routines are used to estimate maximum error in offset correction */

static void
update_slow_slew_error(int offset)
{
  struct timeval now, newend;

  if (offset == 0 && slow_slew_error == 0)
    return;

  if (gettimeofday(&now, NULL) < 0) {
    LOG_FATAL(LOGF_SysLinux, "gettimeofday() failed");
  }

  if (offset < 0)
    offset = -offset;

  /* assume 500ppm rate and one sec delay, plus 10 percent for fast slewing */
  UTI_AddDoubleToTimeval(&now, (offset + 999) / 500 * 1.1, &newend);

  if (offset > 500)
    offset = 500;

  if (slow_slew_error > offset) {
    double previous_left;

    UTI_DiffTimevalsToDouble(&previous_left, &slow_slew_error_end, &now);
    if (previous_left > 0.0) {
      if (offset == 0)
        newend = slow_slew_error_end;
      offset = slow_slew_error;
    }
  }

  slow_slew_error = offset;
  slow_slew_error_end = newend;
}

static double
get_slow_slew_error(struct timeval *now)
{
  double left;

  if (slow_slew_error == 0)
    return 0.0;

  UTI_DiffTimevalsToDouble(&left, &slow_slew_error_end, now);
  return left > 0.0 ? slow_slew_error / 1e6 : 0.0;
}

static void
update_nano_slew_error(long offset, int new)
{
  struct timeval now;
  double ago;

  if (offset == 0 && nano_slew_error == 0)
    return;

  /* maximum error in offset reported by adjtimex, assuming PLL constant 0 
     and SHIFT_PLL = 2 */
  offset /= new ? 4 : 3;
  if (offset < 0)
    offset = -offset;

  if (new || nano_slew_error_start.tv_sec > 0) {
    if (gettimeofday(&now, NULL) < 0) {
      LOG_FATAL(LOGF_SysLinux, "gettimeofday() failed");
    }
  }

  /* When PLL offset is newly set, use the maximum of the old and new error.
     Otherwise use the minimum, but only when the last update is older than
     1.1 seconds to be sure the previous adjustment is already gone. */
  if (!new) {
    if (nano_slew_error > offset) {
      if (nano_slew_error_start.tv_sec == 0) {
        nano_slew_error = offset;
      } else {
        UTI_DiffTimevalsToDouble(&ago, &now, &nano_slew_error_start);
        if (ago > 1.1) {
          nano_slew_error_start.tv_sec = 0;
          nano_slew_error = offset;
        }
      }
    }
  } else {
    if (nano_slew_error < offset)
      nano_slew_error = offset;
    nano_slew_error_start = now;
  }
}

static double
get_nano_slew_error(void)
{
  if (nano_slew_error == 0)
    return 0.0;

  return nano_slew_error / 1e9;
}

static void
update_fast_slew_error(struct timeval *now)
{
  double max_tick;

  max_tick = current_total_tick +
    (delta_total_tick > 0.0 ? delta_total_tick : 0.0);

  UTI_AddDoubleToTimeval(now, 1e6 * max_tick / nominal_tick / tick_update_hz,
      &fast_slew_error_end);
  fast_slew_error = fabs(1e6 * delta_total_tick / nominal_tick / tick_update_hz);
}

static double
get_fast_slew_error(struct timeval *now)
{
  double left;

  if (fast_slew_error == 0.0)
    return 0.0;

  UTI_DiffTimevalsToDouble(&left, &fast_slew_error_end, now);
  if (left < -10.0)
    fast_slew_error = 0.0;

  return left > 0.0 ? fast_slew_error : 0.0;
}

/* ================================================== */
/* This routine stops a fast slew, determines how long the slew has
   been running for, and consequently how much adjustment has actually
   been applied. It can be used both when a slew finishes naturally
   due to a timeout, and when a slew is being aborted. */

static void
stop_fast_slew(void)
{
  struct timeval T1;
  double fast_slew_done;
  double slew_duration;

  /* Should never get here unless this is true */
  assert(fast_slewing);
  
  /* Now set the thing off */
  if (gettimeofday(&T1, NULL) < 0) {
    LOG_FATAL(LOGF_SysLinux, "gettimeofday() failed");
  }
  
  if (TMX_SetTick(current_tick) < 0) {
    LOG_FATAL(LOGF_SysLinux, "adjtimex() failed");
  }

  fast_slewing = 0;

  UTI_DiffTimevalsToDouble(&slew_duration, &T1, &slew_start_tv);

  /* Compute the dispersion we have introduced by changing tick this
     way.  We handle this by adding dispersion to all statistics held
     at higher levels in the system. */

  update_fast_slew_error(&T1);
  lcl_InvokeDispersionNotifyHandlers(fast_slew_error);

  fast_slew_done = delta_total_tick * slew_duration /
    (current_total_tick + delta_total_tick);

  offset_register += (fast_slew_wanted + fast_slew_done);

}

/* ================================================== */
/* This routine reschedules fast slew timeout after frequency was changed */

static void
adjust_fast_slew(double old_tick, double old_delta_tick)
{
  struct timeval tv, end_of_slew;
  double fast_slew_done, slew_duration, dseconds;

  assert(fast_slewing);

  if (gettimeofday(&tv, NULL) < 0) {
    LOG_FATAL(LOGF_SysLinux, "gettimeofday() failed");
  }
  
  UTI_DiffTimevalsToDouble(&slew_duration, &tv, &slew_start_tv);

  fast_slew_done = old_delta_tick * slew_duration / (old_tick + old_delta_tick);
  offset_register += fast_slew_wanted + fast_slew_done;

  dseconds = -offset_register * (current_total_tick + delta_total_tick) / delta_total_tick;

  if (dseconds > 3600 * 24 * 7)
    dseconds = 3600 * 24 * 7;
  UTI_AddDoubleToTimeval(&tv, dseconds, &end_of_slew);

  slew_start_tv = tv;
  fast_slew_wanted = offset_register;
  offset_register = 0.0;

  SCH_RemoveTimeout(slew_timeout_id);
  slew_timeout_id = SCH_AddTimeout(&end_of_slew, handle_end_of_slew, NULL);
}

/* ================================================== */
/* This routine is called to start a clock offset adjustment */

static void
initiate_slew(void)
{
  double dseconds;
  long tick_adjust;
  long offset;
  struct timeval T0;
  struct timeval end_of_slew;

  /* Don't want to get here if we already have an adjust on the go! */
  assert(!fast_slewing);

  if (offset_register == 0.0) {
    return;
  }

  /* Cancel any slewing that is running */
  if (slow_slewing) {
    offset = 0;
    if (TMX_ApplyOffset(&offset) < 0) {
      LOG_FATAL(LOGF_SysLinux, "adjtimex() failed");
    }
    offset_register -= (double) offset / 1.0e6;
    slow_slewing = 0;
    update_slow_slew_error(0);
  } else if (nano_slewing) {
    if (TMX_GetPLLOffsetLeft(&offset) < 0) {
      LOG_FATAL(LOGF_SysLinux, "adjtimex() failed");
    }
    offset_register -= (double) offset / 1.0e9;
    update_nano_slew_error(offset, 0);

    offset = 0;
    if (TMX_ApplyPLLOffset(offset) < 0) {
      LOG_FATAL(LOGF_SysLinux, "adjtimex() failed");
    }
    nano_slewing = 0;
    update_nano_slew_error(offset, 1);
  }

  if (have_nanopll && fabs(offset_register) < MAX_ADJUST_WITH_NANOPLL) {
    /* Use PLL with fixed frequency to do the shift */
    offset = 1.0e9 * -offset_register;

    if (TMX_ApplyPLLOffset(offset) < 0) {
      LOG_FATAL(LOGF_SysLinux, "adjtimex() failed");
    }
    offset_register = 0.0;
    nano_slewing = 1;
    update_nano_slew_error(offset, 1);
  } else if (fabs(offset_register) < MAX_ADJUST_WITH_ADJTIME) {
    /* Use adjtime to do the shift */
    offset = our_round(1.0e6 * -offset_register);

    offset_register += offset / 1.0e6;

    if (offset != 0) {
      if (TMX_ApplyOffset(&offset) < 0) {
        LOG_FATAL(LOGF_SysLinux, "adjtimex() failed");
      }
      slow_slewing = 1;
      update_slow_slew_error(offset);
    }
  } else {

    /* If the system clock has a high drift rate, the combination of
       current_tick + slew_delta_tick could be outside the range that adjtimex
       will accept.  To prevent this, the tick adjustment that is used to slew
       an error off the clock is clamped according to what tick_adjust is.
    */

    long min_allowed_tick, max_allowed_tick;

    min_allowed_tick = nominal_tick - max_tick_bias;
    max_allowed_tick = nominal_tick + max_tick_bias;

    if (offset_register > 0) {
      if (current_tick <= min_allowed_tick) {
        return;
      }

      slewing_tick = current_tick - slew_delta_tick;
      if (slewing_tick < min_allowed_tick) {
        slewing_tick = min_allowed_tick;
      }
    } else {
      if (current_tick >= max_allowed_tick) {
        return;
      }

      slewing_tick = current_tick + slew_delta_tick;
      if (slewing_tick > max_allowed_tick) {
        slewing_tick = max_allowed_tick;
      }
    }

    tick_adjust = slewing_tick - current_tick;

    delta_total_tick = (double) tick_adjust / 1.0e6;
    dseconds = - offset_register * (current_total_tick + delta_total_tick) / delta_total_tick;

    assert(dseconds > 0.0);

    /* Now set the thing off */
    if (gettimeofday(&T0, NULL) < 0) {
      LOG_FATAL(LOGF_SysLinux, "gettimeofday() failed");
    }

    if (TMX_SetTick(slewing_tick) < 0) {
      LOG_FATAL(LOGF_SysLinux, "adjtimex() failed");
    }

    /* Compute the dispersion we have introduced by changing tick this
    way.  We handle this by adding dispersion to all statistics held
    at higher levels in the system. */

    update_fast_slew_error(&T0);
    lcl_InvokeDispersionNotifyHandlers(fast_slew_error);

    fast_slewing = 1;
    slew_start_tv = T0;

    /* Set up timeout for end of slew, limit to one week */
    if (dseconds > 3600 * 24 * 7)
      dseconds = 3600 * 24 * 7;
    UTI_AddDoubleToTimeval(&T0, dseconds, &end_of_slew);

    slew_timeout_id = SCH_AddTimeout(&end_of_slew, handle_end_of_slew, NULL);

    fast_slew_wanted = offset_register;
    offset_register = 0.0;

  }

  return;
}

/* ================================================== */

/* This is the callback routine invoked by the scheduler at the end of
   a slew. */

static void
handle_end_of_slew(void *anything)
{
  stop_fast_slew();
  initiate_slew(); /* To do any fine trimming required */
}

/* ================================================== */
/* This routine is used to abort a slew that is in progress, if any */

static void
abort_slew(void)
{
  if (fast_slewing) {
    stop_fast_slew();
    SCH_RemoveTimeout(slew_timeout_id);
  }
}

/* ================================================== */
/* This routine accrues an offset into the offset register, and starts
   a slew if required.

   The offset argument is measured in seconds.  Positive means the
   clock needs to be slewed backwards (i.e. is currently fast of true
   time) */

static void
accrue_offset(double offset)
{
  /* Add the new offset to the register */
  offset_register += offset;

  if (!fast_slewing) {
    initiate_slew();
  } /* Otherwise, when the fast slew completes, any other stuff
       in the offset register will be applied */

}

/* ================================================== */
/* Positive means currently fast of true time, i.e. jump backwards */

static void
apply_step_offset(double offset)
{
  struct timeval old_time, new_time;
  double err;

  if (fast_slewing) {
    abort_slew();
  }

  if (gettimeofday(&old_time, NULL) < 0) {
    LOG_FATAL(LOGF_SysLinux, "gettimeofday() failed");
  }

  UTI_AddDoubleToTimeval(&old_time, -offset, &new_time);

  if (settimeofday(&new_time, NULL) < 0) {
    LOG_FATAL(LOGF_SysLinux, "settimeofday() failed");
  }

  if (gettimeofday(&old_time, NULL) < 0) {
    LOG_FATAL(LOGF_SysLinux, "gettimeofday() failed");
  }

  UTI_DiffTimevalsToDouble(&err, &old_time, &new_time);
  lcl_InvokeDispersionNotifyHandlers(fabs(err));

  initiate_slew();

}

/* ================================================== */
/* This call sets the Linux kernel frequency to a given value in parts
   per million relative to the nominal running frequency.  Nominal is taken to
   be tick=10000, freq=0 (for a HZ==100 system, other values otherwise).  The
   convention is that this is called with a positive argument if the local
   clock runs fast when uncompensated.  */

static double
set_frequency(double freq_ppm)
{
  long required_tick;
  long min_allowed_tick, max_allowed_tick;
  double required_freq; /* what we use */
  double scaled_freq; /* what adjtimex & the kernel use */
  double old_total_tick;
  int required_delta_tick;
  int neg; /* True if estimate is that local clock runs slow,
              i.e. positive frequency correction required */


  if (freq_ppm < 0.0) {
    neg = 1;
    freq_ppm = -freq_ppm;
  } else {
    neg = 0;
  }

  required_delta_tick = our_round(freq_ppm / dhz);
  required_freq = freq_ppm - dhz * (double) required_delta_tick;

  if (neg) {
    /* Uncompensated local clock runs slow */
    required_tick = nominal_tick + required_delta_tick;
    scaled_freq = freq_scale * required_freq;
  } else {
    /* Uncompensated local clock runs fast */
    required_tick = nominal_tick - required_delta_tick;
    scaled_freq = -freq_scale * required_freq;
  }

  min_allowed_tick = nominal_tick - max_tick_bias;
  max_allowed_tick = nominal_tick + max_tick_bias;

  if (required_tick < min_allowed_tick || required_tick > max_allowed_tick) {
    LOG(LOGS_WARN, LOGF_SysLinux, "Required tick %ld outside allowed range (%ld .. %ld)", required_tick, min_allowed_tick, max_allowed_tick);
    if (required_tick < min_allowed_tick) {
      required_tick = min_allowed_tick;
    } else {
      required_tick = max_allowed_tick;
    }
  }

  current_tick = required_tick;
  old_total_tick = current_total_tick;
  current_total_tick = ((double)current_tick + required_freq/dhz) / 1.0e6 ;

  /* Don't change tick if we are fast slewing, just reschedule the timeout */
  if (fast_slewing) {
    required_tick = slewing_tick;
  }

  if (TMX_SetFrequency(&scaled_freq, required_tick) < 0) {
    LOG_FATAL(LOGF_SysLinux, "adjtimex failed for set_frequency, freq_ppm=%10.4e scaled_freq=%10.4e required_tick=%ld",
        freq_ppm, scaled_freq, required_tick);
  }

  if (fast_slewing) {
    double old_delta_tick;

    old_delta_tick = delta_total_tick;
    delta_total_tick = ((double)slewing_tick + required_freq/dhz) / 1.0e6 -
      current_total_tick;
    adjust_fast_slew(old_total_tick, old_delta_tick);
  }

  return dhz * (nominal_tick - current_tick) - scaled_freq / freq_scale;
}

/* ================================================== */
/* Read the ppm frequency from the kernel */

static double
read_frequency(void)
{
  double tick_term;
  double unscaled_freq;
  double freq_term;

  if (TMX_GetFrequency(&unscaled_freq) < 0) {
    LOG_FATAL(LOGF_SysLinux, "adjtimex() failed");
  }

  /* Use current_tick here rather than txc.tick, otherwise we're
     thrown off course when doing a fast slew (in which case, txc.tick
     is nowhere near the nominal value) */
  tick_term = dhz * (double)(nominal_tick - current_tick);
  freq_term = unscaled_freq / freq_scale;
  
#if 0
  LOG(LOGS_INFO, LOGF_SysLinux, "txc.tick=%ld txc.freq=%ld tick_term=%f freq_term=%f",
      txc.tick, txc.freq, tick_term, freq_term);
#endif

  return tick_term - freq_term;

}

/* ================================================== */
/* Given a raw time, determine the correction in seconds to generate
   the 'cooked' time.  The correction has to be added to the
   raw time */

static void
get_offset_correction(struct timeval *raw,
                      double *corr, double *err)
{

  /* Correction is given by these things :
     1. Any value in offset register
     2. Amount of fast slew remaining
     3. Any amount of adjtime correction remaining
     4. Any amount of nanopll correction remaining */


  double fast_slew_duration;
  double fast_slew_achieved;
  double fast_slew_remaining;
  long offset, noffset, toffset;

  if (!slow_slewing) {
    offset = 0;
  } else {
    if (have_readonly_adjtime) {
      if (TMX_GetOffsetLeft(&offset) < 0) {
        LOG_FATAL(LOGF_SysLinux, "adjtimex() failed");
      }
    } else {
      toffset = 0;
      if (TMX_ApplyOffset(&toffset) < 0) {
        LOG_FATAL(LOGF_SysLinux, "adjtimex() failed");
      }
      offset = toffset;
      if (TMX_ApplyOffset(&toffset) < 0) {
        LOG_FATAL(LOGF_SysLinux, "adjtimex() failed");
      }
    }

    if (offset == 0) {
      /* adjtime slew has finished */
      slow_slewing = 0;
    }
  }

  if (!nano_slewing) {
    noffset = 0;
  } else {
    if (TMX_GetPLLOffsetLeft(&noffset) < 0) {
      LOG_FATAL(LOGF_SysLinux, "adjtimex() failed");
    }
    if (noffset == 0) {
      nano_slewing = 0;
    }
  }

  if (fast_slewing) {
    UTI_DiffTimevalsToDouble(&fast_slew_duration, raw, &slew_start_tv);
    fast_slew_achieved = delta_total_tick * fast_slew_duration /
      (current_total_tick + delta_total_tick);
    fast_slew_remaining = fast_slew_wanted + fast_slew_achieved;
  } else {
    fast_slew_remaining = 0.0;
  }  

  *corr = - (offset_register + fast_slew_remaining) + offset / 1.0e6 + noffset / 1.0e9;

  if (err) {
    update_slow_slew_error(offset);
    update_nano_slew_error(noffset, 0);
    *err = get_slow_slew_error(raw) + get_fast_slew_error(raw) + get_nano_slew_error();;
  }

  return;
}

/* ================================================== */

static void
set_leap(int leap)
{
  if (TMX_SetLeap(leap) < 0) {
    LOG_FATAL(LOGF_SysLinux, "adjtimex() failed in set_leap");
  }

  LOG(LOGS_INFO, LOGF_SysLinux, "System clock status set to %s leap second",
     leap ? (leap > 0 ? "insert" : "delete") : "not insert/delete");

  return;
}

/* ================================================== */

/* Estimate the value of HZ given the value of txc.tick that chronyd finds when
 * it starts.  The only credible values are 100 (Linux/x86) or powers of 2.
 * Also, the bounds checking inside the kernel's adjtimex system call enforces
 * a +/- 10% movement of tick away from the nominal value 1e6/HZ. */

static void
guess_hz_and_shift_hz(int tick, int *hz, int *shift_hz)
{
  int i, tick_lo, tick_hi, ihz;
  double tick_nominal;
  /* Pick off the hz=100 case first */
  if (tick >= 9000 && tick <= 11000) {
    *hz = 100;
    *shift_hz = 7;
    return;
  }

  for (i=4; i<16; i++) { /* surely 16 .. 32768 is a wide enough range? */
    ihz = 1 << i;
    tick_nominal = 1.0e6 / (double) ihz;
    tick_lo = (int)(0.5 + tick_nominal*2.0/3.0);
    tick_hi = (int)(0.5 + tick_nominal*4.0/3.0);
    
    if (tick_lo < tick && tick <= tick_hi) {
      *hz = ihz;
      *shift_hz = i;
      return;
    }
  }

  /* oh dear.  doomed. */
  *hz = 0;
  *shift_hz = 0;
  return;
}

/* ================================================== */

static int
kernelvercmp(int major1, int minor1, int patch1,
    int major2, int minor2, int patch2)
{
  if (major1 != major2)
    return major1 - major2;
  if (minor1 != minor2)
    return minor1 - minor2;
  return patch1 - patch2;
}

/* ================================================== */
/* Compute the scaling to use on any frequency we set, according to
   the vintage of the Linux kernel being used. */

static void
get_version_specific_details(void)
{
  int major, minor, patch;
  int shift_hz;
  double dshift_hz;
  double basic_freq_scale; /* what to use if HZ!=100 */
  int config_hz, set_config_hz; /* values of HZ from conf file */
  int set_config_freq_scale;
  double config_freq_scale;
  struct tmx_params tmx_params;
  struct utsname uts;
  
  TMX_ReadCurrentParams(&tmx_params);
  
  guess_hz_and_shift_hz(tmx_params.tick, &hz, &shift_hz);

  if (!shift_hz) {
    LOG_FATAL(LOGF_SysLinux, "Can't determine hz (txc.tick=%ld txc.freq=%ld (%.8f) txc.offset=%ld)",
              tmx_params.tick, tmx_params.freq, tmx_params.dfreq, tmx_params.offset);
  } else {
#if 0
    LOG(LOGS_INFO, LOGF_SysLinux, "Initial txc.tick=%ld txc.freq=%ld (%.8f) txc.offset=%ld => hz=%d shift_hz=%d",
        tmx_params.tick, tmx_params.freq, tmx_params.dfreq, tmx_params.offset, hz, shift_hz);
#endif
  }

  CNF_GetLinuxHz(&set_config_hz, &config_hz);
  if (set_config_hz) hz = config_hz;
  /* (If true, presumably freq_scale will be overridden anyway, making shift_hz
     redundant too.) */

  dhz = (double) hz;
  dshift_hz = (double)(1UL << shift_hz);
  basic_freq_scale = dshift_hz / dhz;
  nominal_tick = (1000000L + (hz/2))/hz; /* Mirror declaration in kernel */
  slew_delta_tick = nominal_tick / 12;
  max_tick_bias = nominal_tick / 10;
  tick_update_hz = hz;

  /* The basic_freq_scale comes from:
     * the kernel increments the usec counter HZ times per second (if the timer
       interrupt period were perfect)
     * the following code in the kernel

       time_adj (+/-)= ltemp >>
         (SHIFT_USEC + SHIFT_HZ - SHIFT_SCALE);

       causes the adjtimex 'freq' value to be divided down by 1<<SHIFT_HZ.

      The net effect is that we have to scale up the value we want by the
      reciprocal of all this, i.e. multiply by (1<<SHIFT_HZ)/HZ.

     If HZ==100, this code in the kernel comes into play too:
#if HZ == 100
    * Compensate for (HZ==100) != (1 << SHIFT_HZ).
     * Add 25% and 3.125% to get 128.125; => only 0.125% error (p. 14)
     *
    if (time_adj < 0)
        time_adj -= (-time_adj >> 2) + (-time_adj >> 5);
    else
        time_adj += (time_adj >> 2) + (time_adj >> 5);
#endif

    Special case that later.
   */

  if (uname(&uts) < 0) {
    LOG_FATAL(LOGF_SysLinux, "Cannot uname(2) to get kernel version, sorry.");
  }

  patch = 0;
  if (sscanf(uts.release, "%d.%d.%d", &major, &minor, &patch) < 2) {
    LOG_FATAL(LOGF_SysLinux, "Cannot read information from uname, sorry");
  }

  LOG(LOGS_INFO, LOGF_SysLinux, "Linux kernel major=%d minor=%d patch=%d", major, minor, patch);

  version_major = major;
  version_minor = minor;
  version_patchlevel = patch;

  if (kernelvercmp(major, minor, patch, 2, 2, 0) < 0) {
    LOG_FATAL(LOGF_SysLinux, "Kernel version not supported, sorry.");
  }

  if (kernelvercmp(major, minor, patch, 2, 6, 27) < 0) {
    freq_scale = (hz == 100) ? (128.0 / 128.125) : basic_freq_scale;
  } else {
    /* These don't seem to need scaling */
    freq_scale = 1.0;

    if (kernelvercmp(major, minor, patch, 2, 6, 33) < 0) {
      /* Tickless kernels before 2.6.33 accumulated ticks only in
         half-second intervals */
      tick_update_hz = 2;
    }
  }

  /* ADJ_OFFSET_SS_READ support */
  if (kernelvercmp(major, minor, patch, 2, 6, 27) < 0) {
    have_readonly_adjtime = 0;
  } else {
    have_readonly_adjtime = 1;
  }

  /* ADJ_NANO support */
  if (kernelvercmp(major, minor, patch, 2, 6, 27) < 0) {
    have_nanopll = 0;
  } else {
    have_nanopll = 1;
  }

  /* Override freq_scale if it appears in conf file */
  CNF_GetLinuxFreqScale(&set_config_freq_scale, &config_freq_scale);
  if (set_config_freq_scale) {
    freq_scale = config_freq_scale;
  }

  LOG(LOGS_INFO, LOGF_SysLinux, "hz=%d shift_hz=%d freq_scale=%.8f nominal_tick=%d slew_delta_tick=%d max_tick_bias=%d",
      hz, shift_hz, freq_scale, nominal_tick, slew_delta_tick, max_tick_bias);
}

/* ================================================== */
/* Initialisation code for this module */

void
SYS_Linux_Initialise(void)
{
  long offset;

  offset_register = 0.0;
  fast_slewing = 0;

  get_version_specific_details();

  current_tick = nominal_tick;
  current_total_tick = 1.0 / dhz;

  lcl_RegisterSystemDrivers(read_frequency, set_frequency,
                            accrue_offset, apply_step_offset,
                            get_offset_correction, set_leap);

  offset = 0;
  if (TMX_ApplyOffset(&offset) < 0) {
    LOG_FATAL(LOGF_SysLinux, "adjtimex() failed");
  }

  if (have_readonly_adjtime && (TMX_GetOffsetLeft(&offset) < 0 || offset)) {
    LOG(LOGS_INFO, LOGF_SysLinux, "adjtimex() doesn't support ADJ_OFFSET_SS_READ");
    have_readonly_adjtime = 0;
  }

  if (have_nanopll && TMX_EnableNanoPLL() < 0) {
    LOG(LOGS_INFO, LOGF_SysLinux, "adjtimex() doesn't support nanosecond PLL");
    have_nanopll = 0;
  }

  TMX_SetSync(CNF_GetRTCSync());
}

/* ================================================== */
/* Finalisation code for this module */

void
SYS_Linux_Finalise(void)
{
  /* Must *NOT* leave a fast slew running - clock would drift way off
     if the daemon is not restarted */
  abort_slew();
}

/* ================================================== */

void
SYS_Linux_GetKernelVersion(int *major, int *minor, int *patchlevel)
{
  *major = version_major;
  *minor = version_minor;
  *patchlevel = version_patchlevel;
}

/* ================================================== */

#ifdef FEAT_LINUXCAPS
void
SYS_Linux_DropRoot(char *user)
{
  struct passwd *pw;
  cap_t cap;

  if (user == NULL)
    return;

  if ((pw = getpwnam(user)) == NULL) {
    LOG_FATAL(LOGF_SysLinux, "getpwnam(%s) failed", user);
  }

  if (prctl(PR_SET_KEEPCAPS, 1)) {
    LOG_FATAL(LOGF_SysLinux, "prcap() failed");
  }
  
  if (setgroups(0, NULL)) {
    LOG_FATAL(LOGF_SysLinux, "setgroups() failed");
  }

  if (setgid(pw->pw_gid)) {
    LOG_FATAL(LOGF_SysLinux, "setgid(%d) failed", pw->pw_gid);
  }

  if (setuid(pw->pw_uid)) {
    LOG_FATAL(LOGF_SysLinux, "setuid(%d) failed", pw->pw_uid);
  }

  if ((cap = cap_from_text("cap_sys_time=ep")) == NULL) {
    LOG_FATAL(LOGF_SysLinux, "cap_from_text() failed");
  }

  if (cap_set_proc(cap)) {
    LOG_FATAL(LOGF_SysLinux, "cap_set_proc() failed");
  }

  cap_free(cap);

#if 0
  LOG(LOGS_INFO, LOGF_SysLinux, "Privileges dropped to user %s", user);
#endif
}
#endif

/* ================================================== */

#if defined(HAVE_SCHED_SETSCHEDULER)
  /* Install SCHED_FIFO real-time scheduler with specified priority */
void SYS_Linux_SetScheduler(int SchedPriority)
{
  int pmax, pmin;
  struct sched_param sched;

  if (SchedPriority < 1 || SchedPriority > 99) {
    LOG_FATAL(LOGF_SysLinux, "Bad scheduler priority: %d", SchedPriority);
  } else {
    sched.sched_priority = SchedPriority;
    pmax = sched_get_priority_max(SCHED_FIFO);
    pmin = sched_get_priority_min(SCHED_FIFO);
    if ( SchedPriority > pmax ) {
      sched.sched_priority = pmax;
    }
    else if ( SchedPriority < pmin ) {
      sched.sched_priority = pmin;
    }
    if ( sched_setscheduler(0, SCHED_FIFO, &sched) == -1 ) {
      LOG(LOGS_ERR, LOGF_SysLinux, "sched_setscheduler() failed");
    }
    else {
#if 0
      LOG(LOGS_INFO, LOGF_SysLinux, "Enabled SCHED_FIFO with priority %d", sched.sched_priority);
#endif
    }
  }
}
#endif /* HAVE_SCHED_SETSCHEDULER */

#if defined(HAVE_MLOCKALL)
/* Lock the process into RAM so that it will never be swapped out */ 
void SYS_Linux_MemLockAll(int LockAll)
{
  struct rlimit rlim;
  if (LockAll == 1 ) {
    /* Make sure that we will be able to lock all the memory we need */
    /* even after dropping privileges.  This does not actually reaerve any memory */
    rlim.rlim_max = RLIM_INFINITY;
    rlim.rlim_cur = RLIM_INFINITY;
    if (setrlimit(RLIMIT_MEMLOCK, &rlim) < 0) {
      LOG(LOGS_ERR, LOGF_SysLinux, "setrlimit() failed: not locking into RAM");
    }
    else {
      if (mlockall(MCL_CURRENT|MCL_FUTURE) < 0) {
	LOG(LOGS_ERR, LOGF_SysLinux, "mlockall() failed");
      }
      else {
#if 0
	LOG(LOGS_INFO, LOGF_SysLinux, "Successfully locked into RAM");
#endif
      }
    }
  }
}
#endif /* HAVE_MLOCKALL */

#endif /* LINUX */

/* vim:ts=8
 * */

