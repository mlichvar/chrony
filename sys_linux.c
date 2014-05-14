/*
  chronyd/chronyc - Programs for keeping computer clocks accurate.

 **********************************************************************
 * Copyright (C) Richard P. Curnow  1997-2003
 * Copyright (C) John G. Hasler  2009
 * Copyright (C) Miroslav Lichvar  2009-2012
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

#include "sysincl.h"

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
#include "sys_generic.h"
#include "sys_linux.h"
#include "conf.h"
#include "logging.h"
#include "wrap_adjtimex.h"

/* This is the uncompensated system tick value */
static int nominal_tick;

/* The maximum amount by which 'tick' can be biased away from 'nominal_tick'
   (sys_adjtimex() in the kernel bounds this to 10%) */
static int max_tick_bias;

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

/* The kernel HZ constant (USER_HZ in recent kernels). */
static int hz;
static double dhz; /* And dbl prec version of same for arithmetic */

/* Flag indicating whether adjtimex() can step the clock */
static int have_setoffset;

/* The assumed rate at which the effective frequency and tick values are
   updated in the kernel */
static int tick_update_hz;

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
/* Positive means currently fast of true time, i.e. jump backwards */

static void
apply_step_offset(double offset)
{
  struct timeval old_time, new_time;
  double err;

  if (have_setoffset) {
    if (TMX_ApplyStepOffset(-offset) < 0) {
      LOG_FATAL(LOGF_SysLinux, "adjtimex() failed");
    }
  } else {
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
  }
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
  double required_freq; /* what we use */
  double scaled_freq; /* what adjtimex & the kernel use */
  int required_delta_tick;

  required_delta_tick = our_round(freq_ppm / dhz);
  required_freq = -(freq_ppm - dhz * required_delta_tick);
  required_tick = nominal_tick - required_delta_tick;
  scaled_freq = freq_scale * required_freq;

  if (TMX_SetFrequency(&scaled_freq, required_tick) < 0) {
    LOG_FATAL(LOGF_SysLinux, "adjtimex failed for set_frequency, freq_ppm=%10.4e scaled_freq=%10.4e required_tick=%ld",
        freq_ppm, scaled_freq, required_tick);
  }

  return dhz * (nominal_tick - required_tick) - scaled_freq / freq_scale;
}

/* ================================================== */
/* Read the ppm frequency from the kernel */

static double
read_frequency(void)
{
  double tick_term;
  double unscaled_freq;
  double freq_term;
  long tick;

  if (TMX_GetFrequency(&unscaled_freq, &tick) < 0) {
    LOG_FATAL(LOGF_SysLinux, "adjtimex() failed");
  }

  tick_term = dhz * (double)(nominal_tick - tick);
  freq_term = unscaled_freq / freq_scale;
  
#if 0
  LOG(LOGS_INFO, LOGF_SysLinux, "txc.tick=%ld txc.freq=%ld tick_term=%f freq_term=%f",
      txc.tick, txc.freq, tick_term, freq_term);
#endif

  return tick_term - freq_term;
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
}

/* ================================================== */

static int
get_hz_and_shift_hz(int *hz, int *shift_hz)
{
#ifdef _SC_CLK_TCK
  if ((*hz = sysconf(_SC_CLK_TCK)) < 1) {
    return 0;
  }

  if (*hz == 100) {
    *shift_hz = 7;
    return 1;
  }

  for (*shift_hz = 1; (*hz >> *shift_hz) > 1; (*shift_hz)++)
    ;

  return 1;
#else
  return 0;
#endif
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
  
  if (!get_hz_and_shift_hz(&hz, &shift_hz)) {
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
  }

  CNF_GetLinuxHz(&set_config_hz, &config_hz);
  if (set_config_hz) hz = config_hz;
  /* (If true, presumably freq_scale will be overridden anyway, making shift_hz
     redundant too.) */

  dhz = (double) hz;
  dshift_hz = (double)(1UL << shift_hz);
  basic_freq_scale = dshift_hz / dhz;
  nominal_tick = (1000000L + (hz/2))/hz; /* Mirror declaration in kernel */
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

  DEBUG_LOG(LOGF_SysLinux, "Linux kernel major=%d minor=%d patch=%d", major, minor, patch);

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

  /* ADJ_SETOFFSET support */
  if (kernelvercmp(major, minor, patch, 2, 6, 39) < 0) {
    have_setoffset = 0;
  } else {
    have_setoffset = 1;
  }

  /* Override freq_scale if it appears in conf file */
  CNF_GetLinuxFreqScale(&set_config_freq_scale, &config_freq_scale);
  if (set_config_freq_scale) {
    freq_scale = config_freq_scale;
  }

  DEBUG_LOG(LOGF_SysLinux, "hz=%d shift_hz=%d freq_scale=%.8f nominal_tick=%d max_tick_bias=%d",
      hz, shift_hz, freq_scale, nominal_tick, max_tick_bias);
}

/* ================================================== */
/* Initialisation code for this module */

void
SYS_Linux_Initialise(void)
{
  get_version_specific_details();

  if (TMX_ResetOffset() < 0) {
    LOG_FATAL(LOGF_SysLinux, "adjtimex() failed");
  }

  if (have_setoffset && TMX_TestStepOffset() < 0) {
    LOG(LOGS_INFO, LOGF_SysLinux, "adjtimex() doesn't support ADJ_SETOFFSET");
    have_setoffset = 0;
  }

  TMX_SetSync(CNF_GetRtcSync());

  SYS_Generic_CompleteFreqDriver(1.0e6 * max_tick_bias / nominal_tick,
                                 1.0 / tick_update_hz,
                                 read_frequency, set_frequency,
                                 apply_step_offset, set_leap);
}

/* ================================================== */
/* Finalisation code for this module */

void
SYS_Linux_Finalise(void)
{
  SYS_Generic_Finalise();
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
