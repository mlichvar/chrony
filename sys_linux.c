/*
  $Header: /cvs/src/chrony/sys_linux.c,v 1.41 2003/07/01 20:56:23 richard Exp $

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

  This is the module specific to the Linux operating system.

  */

#ifdef LINUX

#include <sys/time.h>
#include <stdio.h>
#include <unistd.h>
#include <math.h>
#include <ctype.h>
#include <assert.h>
#include <sys/utsname.h>

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

/* Flag indicating whether adjtimex() with txc.modes equal to zero
returns the remaining time adjustment or not.  If not we have to read
the outstanding adjustment by setting it to zero, examining the return
value and setting the outstanding adjustment back again. */

static int have_readonly_adjtime;

/* ================================================== */

static void handle_end_of_slew(void *anything);

/* ================================================== */

inline static int
round(double x) {
  int y;
  y = (int)(x + 0.5);
  while ((double)y < x - 0.5) y++;
  while ((double)y > x + 0.5) y--;
  return y;
}

/* ================================================== */
/* Amount of outstanding offset to process */
static double offset_register;

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

/* The amount by which we alter 'tick' when doing a large slew */
static int slew_delta_tick;

/* The maximum amount by which 'tick' can be biased away from 'nominal_tick'
   (sys_adjtimex() in the kernel bounds this to 10%) */
static int max_tick_bias;

/* ================================================== */
/* This routine stops a fast slew, determines how long the slew has
   been running for, and consequently how much adjustment has actually
   been applied. It can be used both when a slew finishes naturally
   due to a timeout, and when a slew is being aborted. */

static void
stop_fast_slew(void)
{
  struct timeval T1, T1d, T1a;
  struct timezone tz;
  double end_window;
  double fast_slew_done;
  double slew_duration;
  double introduced_dispersion;

  /* Should never get here unless this is true */
  if (!fast_slewing) {
    CROAK("Should be fast slewing");
  }
  
  /* Now set the thing off */
  if (gettimeofday(&T1, &tz) < 0) {
    CROAK("gettimeofday() failed in stop_fast_slew");
  }
  
  if (TMX_SetTick(current_tick) < 0) {
    CROAK("adjtimex() failed in stop_fast_slew");
  }
  
  if (gettimeofday(&T1d, &tz) < 0) {
    CROAK("gettimeofday() failed in stop_fast_slew");
  }

  fast_slewing = 0;

  UTI_AverageDiffTimevals(&T1, &T1d, &T1a, &end_window);
  UTI_DiffTimevalsToDouble(&slew_duration, &T1a, &slew_start_tv);
  
  /* Compute the dispersion we have introduced by changing tick this
     way.  If the two samples of gettimeofday differ, there is an
     uncertainty window wrt when the frequency change actually applies
     from.  We handle this by adding dispersion to all statistics held
     at higher levels in the system. */

  introduced_dispersion = end_window * delta_total_tick;
  lcl_InvokeDispersionNotifyHandlers(introduced_dispersion);

  fast_slew_done = delta_total_tick * slew_duration /
    (current_total_tick + delta_total_tick);

  offset_register += (fast_slew_wanted + fast_slew_done);

}


/* ================================================== */
/* This routine is called to start a clock offset adjustment */

static void
initiate_slew(void)
{
  double dseconds;
  long tick_adjust;
  long offset;
  struct timeval T0, T0d, T0a;
  struct timeval end_of_slew;
  struct timezone tz;
  double start_window;
  double introduced_dispersion;

  /* Don't want to get here if we already have an adjust on the go! */
  if (fast_slewing) {
    CROAK("Should not be fast slewing");
  }

  if (offset_register == 0.0) {
    return;
  }

  if (fabs(offset_register) < MAX_ADJUST_WITH_ADJTIME) {
    /* Use adjtime to do the shift */
    offset = (long)(0.5 + 1.0e6*(-offset_register));

    if (TMX_ApplyOffset(&offset) < 0) {
      CROAK("adjtimex() failed in initiate_slew");
    }

    offset_register = 0.0;

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
      slewing_tick = current_tick - slew_delta_tick;
      if (slewing_tick <= min_allowed_tick) {
        slewing_tick = min_allowed_tick + 1;
      }
    } else {
      slewing_tick = current_tick + slew_delta_tick;
      if (slewing_tick >= max_allowed_tick) {
        slewing_tick = max_allowed_tick - 1;
      }
    }

    tick_adjust = slewing_tick - current_tick;

    delta_total_tick = (double) tick_adjust / 1.0e6;
    dseconds = - offset_register * (current_total_tick + delta_total_tick) / delta_total_tick;

    /* Now set the thing off */
    if (gettimeofday(&T0, &tz) < 0) {
      CROAK("gettimeofday() failed in initiate_slew");
    }

    if (TMX_SetTick(slewing_tick) < 0) {
      LOG(LOGS_INFO, LOGF_SysLinux, "c_t=%ld ta=%ld sl_t=%ld dtt=%e",
          current_tick, tick_adjust, slewing_tick, delta_total_tick);
      CROAK("adjtimex() failed to start big slew");
    }

    if (gettimeofday(&T0d, &tz) < 0) {
      CROAK("gettimeofday() failed in initiate_slew");
    }

    /* Now work out the uncertainty in when we actually started the
       slew. */
    
    UTI_AverageDiffTimevals(&T0, &T0d, &T0a, &start_window);

    /* Compute the dispersion we have introduced by changing tick this
    way.  If the two samples of gettimeofday differ, there is an
    uncertainty window wrt when the frequency change actually applies
    from.  We handle this by adding dispersion to all statistics held
    at higher levels in the system. */

    introduced_dispersion = start_window * delta_total_tick;
    lcl_InvokeDispersionNotifyHandlers(introduced_dispersion);

    fast_slewing = 1;
    slew_start_tv = T0a;

    /* Set up timeout for end of slew */
    UTI_AddDoubleToTimeval(&T0a, dseconds, &end_of_slew);

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
  long toffset;
  
  /* Add the new offset to the register */
  offset_register += offset;

  /* Cancel any standard adjtime that is running */
  toffset = 0;
  if (TMX_ApplyOffset(&toffset) < 0) {
    CROAK("adjtimex() failed in accrue_offset");
  }

  offset_register -= (double) toffset / 1.0e6;

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
  struct timezone tz;

  if (fast_slewing) {
    abort_slew();
  }

  if (gettimeofday(&old_time, &tz) < 0) {
    CROAK("gettimeofday in apply_step_offset");
  }

  UTI_AddDoubleToTimeval(&old_time, -offset, &new_time);

  if (settimeofday(&new_time, &tz) < 0) {
    CROAK("settimeofday in apply_step_offset");
  }

  initiate_slew();

}

/* ================================================== */
/* This call sets the Linux kernel frequency to a given value in parts
   per million relative to the nominal running frequency.  Nominal is taken to
   be tick=10000, freq=0 (for a HZ==100 system, other values otherwise).  The
   convention is that this is called with a positive argument if the local
   clock runs fast when uncompensated.  */

static void
set_frequency(double freq_ppm) {

  long required_tick;
  double required_freq; /* what we use */
  double scaled_freq; /* what adjtimex & the kernel use */
  int required_delta_tick;
  int neg; /* True if estimate is that local clock runs slow,
              i.e. positive frequency correction required */


  /* If we in the middle of slewing the time by having the value of
     tick altered, we have to stop doing that, because the timeout
     expiry etc will change if we don't. */

  if (fast_slewing) {
    abort_slew();
  }

  if (freq_ppm < 0.0) {
    neg = 1;
    freq_ppm = -freq_ppm;
  } else {
    neg = 0;
  }

  required_delta_tick = round(freq_ppm / dhz);
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

  if (TMX_SetFrequency(scaled_freq, required_tick) < 0) {
    char buffer[1024];
    sprintf(buffer, "adjtimex failed for set_frequency, freq_ppm=%10.4e scaled_freq=%10.4e required_tick=%ld",
            freq_ppm, scaled_freq, required_tick);
    CROAK(buffer);
  }

  current_tick = required_tick;
  current_total_tick = ((double)current_tick + required_freq/dhz) / 1.0e6 ;

  initiate_slew(); /* Restart any slews that need to be restarted */

  return;

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
    CROAK("adjtimex failed in read_frequency");
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
                      double *corr)
{

  /* Correction is given by these things :
     1. Any value in offset register
     2. Amount of fast slew remaining
     3. Any amount of adjtime correction remaining */


  double adjtime_left;
  double fast_slew_duration;
  double fast_slew_achieved;
  double fast_slew_remaining;
  long offset;

  if (have_readonly_adjtime) {
    if (TMX_GetOffsetLeft(&offset) < 0) {
      CROAK("adjtimex() failed in get_offset_correction");
    }
    
    adjtime_left = (double)offset / 1.0e6;
  } else {
    offset = 0;
    if (TMX_ApplyOffset(&offset) < 0) {
      CROAK("adjtimex() failed in get_offset_correction");
    }
    
    adjtime_left = (double)offset / 1.0e6;

    /* txc.offset still set from return value of last call */
    if (TMX_ApplyOffset(&offset) < 0) {
      CROAK("adjtimex() failed in get_offset_correction");
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

  *corr = - (offset_register + fast_slew_remaining) + adjtime_left;

  return;
}

/* ================================================== */

static void
immediate_step(void)
{
  struct timeval old_time, new_time;
  struct timezone tz;
  long offset;

  if (fast_slewing) {
    abort_slew();
  }

  offset = 0;
  if (TMX_ApplyOffset(&offset) < 0) {
    CROAK("adjtimex() failed in immediate_step");
  }

  offset_register -= (double) offset / 1.0e6;

  if (gettimeofday(&old_time, &tz) < 0) {
    CROAK("gettimeofday() failed in immediate_step");
  }

  UTI_AddDoubleToTimeval(&old_time, -offset_register, &new_time);

  if (settimeofday(&new_time, &tz) < 0) {
    CROAK("settimeofday() failed in immediate_step");
  }

  offset_register = 0.0;

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
  double calculated_freq_scale;
  struct tmx_params tmx_params;
  struct utsname uts;
  
  TMX_ReadCurrentParams(&tmx_params);
  
  guess_hz_and_shift_hz(tmx_params.tick, &hz, &shift_hz);

  if (!shift_hz) {
    LOG_FATAL(LOGF_SysLinux, "Can't determine hz (txc.tick=%ld txc.freq=%ld (%.8f) txc.offset=%ld)",
              tmx_params.tick, tmx_params.freq, tmx_params.dfreq, tmx_params.offset);
  } else {
    LOG(LOGS_INFO, LOGF_SysLinux, "Initial txc.tick=%ld txc.freq=%ld (%.8f) txc.offset=%ld => hz=%d shift_hz=%d",
        tmx_params.tick, tmx_params.freq, tmx_params.dfreq, tmx_params.offset, hz, shift_hz);
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
  if (sscanf(uts.release, "%d.%d.%d", &major, &minor, &patch) != 3) {
    LOG_FATAL(LOGF_SysLinux, "Cannot read information from uname, sorry");
  }

  LOG(LOGS_INFO, LOGF_SysLinux, "Linux kernel major=%d minor=%d patch=%d", major, minor, patch);

  version_major = major;
  version_minor = minor;
  version_patchlevel = patch;
  
  switch (major) {
    case 1:
      /* Does Linux v1.x even support HZ!=100? */
      switch (minor) {
        case 2:
          if (patch == 13) {
            freq_scale = (hz==100) ? (128.0 / 100.0) : basic_freq_scale ; /* I _think_! */
	    have_readonly_adjtime = 1;
          } else {
            LOG_FATAL(LOGF_SysLinux, "Kernel version not supported yet, sorry.");
          }
          break;
        case 3:
          /* I guess the change from the 1.2.x scaling to the 2.0.x
             scaling must have happened during 1.3 development.  I
             haven't a clue where though, until someone looks it
             up. */
          LOG_FATAL(LOGF_SysLinux, "Kernel version not supported yet, sorry.");
          break;
        default:
          LOG_FATAL(LOGF_SysLinux, "Kernel version not supported yet, sorry.");
          break;
      }
      break;
    case 2:
      switch (minor) {
        case 0:
          if (patch < 32) {
            freq_scale = (hz==100) ? (128.0 / 125.0) : basic_freq_scale;
	    have_readonly_adjtime = 1;
          } else if (patch >= 32) {
            freq_scale = (hz==100) ? (128.0 / 128.125) : basic_freq_scale;
	    
	    /* The functionality in kernel/time.c in the kernel source
               was modified with regard to what comes back in the
               txc.offset field on return from adjtimex.  If txc.modes
               was ADJ_OFFSET_SINGLESHOT on entry, the outstanding
               adjustment is returned, however the running offset will
               be modified to the passed value. */
	    have_readonly_adjtime = 0;
          }
          break;
        case 1:
          /* I know that earlier 2.1 kernels were like 2.0.31, hence
             the settings below.  However, the 2.0.32 behaviour may
             have been added late in the 2.1 series, however I have no
             idea at which patch level.  Leave it like this until
             someone supplies some info. */
          freq_scale = (hz==100) ? (128.0 / 125.0) : basic_freq_scale;
          have_readonly_adjtime = 0; /* For safety ! */
          break;
        case 2:
        case 3:
        case 4:
        case 5:
          /* These seem to be like 2.0.32 */
          freq_scale = (hz==100) ? (128.0 / 128.125) : basic_freq_scale;
          have_readonly_adjtime = 0;
          break;
        default:
          LOG_FATAL(LOGF_SysLinux, "Kernel version not supported yet, sorry.");
      }
      break;
    default:
      LOG_FATAL(LOGF_SysLinux, "Kernel's major version not supported yet, sorry");
      break;
  }

  /* Override freq_scale if it appears in conf file */
  CNF_GetLinuxFreqScale(&set_config_freq_scale, &config_freq_scale);
  calculated_freq_scale = freq_scale;
  if (set_config_freq_scale) freq_scale = config_freq_scale;
  
  LOG(LOGS_INFO, LOGF_SysLinux, "calculated_freq_scale=%.8f freq_scale=%.8f",
      calculated_freq_scale, freq_scale);

}

/* ================================================== */
/* Set denorms to flush to zero instead of trapping. */

#if defined(__SH5__)
static void enable_flush_denorms(void)
{
  float fpscr;
  unsigned long ifpscr;
  asm volatile("fgetscr %0" : "=f" (fpscr));
  asm volatile("fmov.sl %1, %0" : "=r" (ifpscr) : "f" (fpscr));
  ifpscr |= 0x40000;
  asm volatile("fmov.ls %1, %0" : "=f" (fpscr) : "r" (ifpscr));
  asm volatile("fputscr %0" : : "f" (fpscr));
  return;
}
#endif

/* ================================================== */
/* Initialisation code for this module */

void
SYS_Linux_Initialise(void)
{
  offset_register = 0.0;
  fast_slewing = 0;

#if defined(__SH5__)
  enable_flush_denorms();
#endif

  get_version_specific_details();

  current_tick = nominal_tick;
  current_total_tick = 1.0 / dhz;

  lcl_RegisterSystemDrivers(read_frequency, set_frequency,
                            accrue_offset, apply_step_offset,
                            get_offset_correction, immediate_step);
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

#endif /* LINUX */

/* vim:ts=8
 * */

